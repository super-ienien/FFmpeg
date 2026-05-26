#include "videomaster_dec.h"

#include <math.h>

#include "libavcodec/packet_internal.h"
#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"
#include "libavformat/demux.h"
#include "libavformat/internal.h"
#include "libavutil/avstring.h"
#include "libavutil/dynarray.h"
#include "libavutil/internal.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/timecode.h"

#include "videomaster_audio_pipe.h"
#include "videomaster_common.h"

#if defined(__APPLE__)
#include <VideoMasterHD/VideoMasterHD_Core.h>
#include <VideoMasterHD/VideoMasterHD_Dv.h>
#include <VideoMasterHD/VideoMasterHD_Sdi_Timecode.h>
#else
#include <VideoMasterHD_Core.h>
#include <VideoMasterHD_Dv.h>
#include <VideoMasterHD_Sdi_Timecode.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <conio.h>
#endif

#define OFFSET(x) offsetof(struct VideoMasterData, x)
#define DEC       AV_OPT_FLAG_DECODING_PARAM

/**
 * @brief Non-blocking read of a single key from stdin.
 * @return The character read, or -1 if no key is available.
 */
static int vm_read_key(void)
{
#ifdef _WIN32
    static int is_pipe;
    static HANDLE input_handle;
    DWORD dw, nchars;
    unsigned char ch;
    if (!input_handle) {
        input_handle = GetStdHandle(STD_INPUT_HANDLE);
        is_pipe = !GetConsoleMode(input_handle, &dw);
    }
    if (is_pipe) {
        if (!PeekNamedPipe(input_handle, NULL, 0, NULL, &nchars, NULL))
            return -1;
        if (nchars != 0) {
            _read(0, &ch, 1);
            return ch;
        }
        return -1;
    }
    if (_kbhit())
        return _getch();
#endif
    return -1;
}

/**
 * @brief Attaches the timecode from ctx->last_tc_* (populated by
 * ff_videomaster_get_timestamp) to the video packet as S12M side data
 * and "timecode" string metadata.
 */
static void attach_timecode_to_packet(AVFormatContext *avctx,
                                      VideoMasterContext *ctx, AVPacket *pkt)
{
    AVRational frame_rate;
    int flags;
    AVTimecode avtc;
    char tcstr[AV_TIMECODE_STR_SIZE];
    const char *tcstr_ptr;

    if (!ctx->last_tc_valid)
        return;

    frame_rate = av_make_q(ctx->video_frame_rate_num,
                           ctx->video_frame_rate_den);
    flags = (ctx->last_tc_flags & 0x01) ? AV_TIMECODE_FLAG_DROPFRAME : 0;

    if (av_timecode_init_from_components(&avtc, frame_rate, flags,
                                         ctx->last_tc_h, ctx->last_tc_m,
                                         ctx->last_tc_s, ctx->last_tc_f,
                                         avctx) < 0)
        return;

    /* S12M timecode side data (SMPTE 12M binary) */
    {
        uint32_t tc_data = av_timecode_get_smpte_from_framenum(&avtc, 0);
        int size = sizeof(uint32_t) * 4;
        uint32_t *sd = (uint32_t *)av_packet_new_side_data(
            pkt, AV_PKT_DATA_S12M_TIMECODE, size);
        if (sd) {
            *sd       = 1;       /* one TC */
            *(sd + 1) = tc_data; /* TC value */
        }
    }

    /* Encode LTC framerate as a rational num/den so it round-trips through
     * the MP4 container (via \251tcr atom in track udta). Common LTC rates
     * are either integer (24, 25, 30) or /1001 fractions (23.976, 29.97).
     * Computed once so the value is attached both to each packet's
     * STRINGS_METADATA side data (which survives transcoding) and to the
     * input stream metadata. */
    {
        char rate_str[16];
        int rate_num, rate_den;
        const char *locked_str = ctx->last_tc_locked ? "1" : "0";

        if (fabsf(ctx->last_tc_fps - roundf(ctx->last_tc_fps)) < 0.01f) {
            rate_num = (int)roundf(ctx->last_tc_fps);
            rate_den = 1;
        } else {
            rate_num = (int)roundf(ctx->last_tc_fps * 1001.0f);
            rate_den = 1001;
        }
        snprintf(rate_str, sizeof(rate_str), "%d/%d", rate_num, rate_den);

        /* String metadata attached to the packet. The mov muxer merges this
         * into the output stream metadata, so it survives re-encoding. */
        tcstr_ptr = av_timecode_make_string(&avtc, tcstr, 0);
        if (tcstr_ptr)
        {
            AVDictionary *meta = NULL;
            if (av_dict_set(&meta, "timecode", tcstr_ptr, 0) >= 0 &&
                av_dict_set(&meta, "timecode_rate", rate_str, 0) >= 0 &&
                av_dict_set(&meta, "timecode_locked", locked_str, 0) >= 0)
            {
                size_t meta_len;
                uint8_t *packed = av_packet_pack_dictionary(meta, &meta_len);
                av_dict_free(&meta);
                if (packed) {
                    if (av_packet_add_side_data(pkt, AV_PKT_DATA_STRINGS_METADATA,
                                                packed, meta_len) < 0)
                        av_freep(&packed);
                }
            } else {
                av_dict_free(&meta);
            }
        }

        /* Also set on stream metadata for the no-transcode path. */
        if (!ctx->initial_tc_set && ctx->video_stream && tcstr_ptr)
        {
            av_dict_set(&ctx->video_stream->metadata, "timecode", tcstr_ptr, 0);
            av_dict_set(&ctx->video_stream->metadata, "timecode_rate", rate_str, 0);
            av_dict_set(&ctx->video_stream->metadata, "timecode_locked", locked_str, 0);
            ctx->initial_tc_set = true;
            av_log(avctx, AV_LOG_INFO,
                   "Initial timecode: %s (locked: %s, rate: %s, fps: %.3f)\n",
                   tcstr_ptr, locked_str, rate_str, ctx->last_tc_fps);
        }
    }

    av_log(avctx, AV_LOG_TRACE, "Timecode: %02d:%02d:%02d:%02d (locked: %s, fps: %.3f)\n",
           ctx->last_tc_h, ctx->last_tc_m, ctx->last_tc_s, ctx->last_tc_f,
           ctx->last_tc_locked ? "yes" : "no", ctx->last_tc_fps);
}

/**
 * @brief Resolves a VHD_VIDEOSTANDARD mode index into video parameters.
 * @return 0 on success, negative on failure.
 */
static int resolve_video_mode(AVFormatContext *avctx, int mode_index,
                              uint32_t *width, uint32_t *height,
                              uint32_t *fps_num, uint32_t *fps_den,
                              bool *interlaced)
{
    ULONG w = 0, h = 0, framerate = 0;
    BOOL32 ilaced = FALSE;
    if (mode_index < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid default_video_mode: %d\n", mode_index);
        return AVERROR(EINVAL);
    }
    if (VHD_GetVideoCharacteristics((VHD_VIDEOSTANDARD)mode_index,
                                    &w, &h, &ilaced, &framerate) != VHDERR_NOERROR) {
        av_log(avctx, AV_LOG_ERROR,
               "Unknown VideoMaster video mode index: %d\n", mode_index);
        return AVERROR(EINVAL);
    }
    *width = w;
    *height = h;
    *interlaced = ilaced;
    *fps_num = framerate * 1000;
    *fps_den = 1000;
    av_log(avctx, AV_LOG_INFO,
           "Resolved video mode %d: %ux%u@%u%s\n",
           mode_index, w, h, framerate, ilaced ? "i" : "p");
    return 0;
}

/**
 * @brief Allocates black video and silent audio buffers for signal loss.
 */
static int allocate_black_and_silent_buffers(VideoMasterContext *ctx)
{
    uint32_t i;
    /* Video: YUV422 black = U=0x80 Y=0x10 V=0x80 Y=0x10 per 2 pixels */
    ctx->black_video_buffer_size = ctx->video_width * ctx->video_height * 2;
    ctx->black_video_buffer = av_malloc(ctx->black_video_buffer_size);
    if (!ctx->black_video_buffer)
        return AVERROR(ENOMEM);
    for (i = 0; i + 3 < ctx->black_video_buffer_size; i += 4) {
        ctx->black_video_buffer[i]     = 0x80; /* U */
        ctx->black_video_buffer[i + 1] = 0x10; /* Y */
        ctx->black_video_buffer[i + 2] = 0x80; /* V */
        ctx->black_video_buffer[i + 3] = 0x10; /* Y */
    }

    /* Audio: silence = zero bytes, one video frame of samples */
    if (ctx->has_audio && ctx->audio_sample_rate > 0 &&
        ctx->audio_nb_channels > 0 && ctx->audio_sample_size > 0) {
        uint32_t samples_per_frame = ctx->audio_sample_rate *
                                     ctx->video_frame_rate_den /
                                     ctx->video_frame_rate_num;
        ctx->silent_audio_buffer_size = samples_per_frame *
                                        ctx->audio_nb_channels *
                                        (ctx->audio_sample_size / 8);
        ctx->silent_audio_buffer = av_mallocz(ctx->silent_audio_buffer_size);
        if (!ctx->silent_audio_buffer)
            return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * @brief Lists all known VideoMaster video standards with their properties.
 */
static void list_video_formats(AVFormatContext *avctx)
{
    int i;
    av_log(avctx, AV_LOG_INFO,
           "Supported VideoMaster video modes (use index with -default_video_mode):\n"
           "\tindex\tresolution\tfps\ttype\tname\n");
    for (i = 0; i < NB_VHD_VIDEOSTANDARDS; i++) {
        ULONG w = 0, h = 0, framerate = 0;
        BOOL32 interlaced = FALSE;
        if (VHD_GetVideoCharacteristics((VHD_VIDEOSTANDARD)i,
                                        &w, &h, &interlaced, &framerate) == VHDERR_NOERROR) {
            av_log(avctx, AV_LOG_INFO, "\t%d\t%lux%lu\t\t%lu\t%s\t%s\n",
                   i, w, h, framerate,
                   interlaced ? "interlaced" : "progressive",
                   VHD_VIDEOSTANDARD_ToPrettyString((VHD_VIDEOSTANDARD)i));
        }
    }
}

/** Static function declaration */
/**
 * @brief Checks the integrity of the audio properties in the
 * VideoMaster context.
 * @param videomaster_context VideoMasterContext pointer to the VideoMaster
 * context
 * @return int  0 on success, or negative AVERROR code on failure
 */
int check_audio_properties(VideoMasterContext *videomaster_context);

/**
 * @brief Checks the integrity of the board index argument in the
 * VideoMaster context.
 * @param videomaster_context VideoMasterContext pointer to the VideoMaster
 * context
 * @return int  0 on success, or negative AVERROR code on failure
 */
static int check_board_index(VideoMasterContext *videomaster_context);

/**
 * @brief Checks the integrity of the channel
 * index argument in the VideoMaster context. Calling this function may
 * override audio_nb_channels, audio_sample_rate, and audio_sample_size.
 * Call this function after verifying the integrity of the audio properties
 * using the check_audio_properties function.
 * @param videomaster_context VideoMasterContext
 * pointer to the VideoMaster context
 * @return int  0 on success, or negative AVERROR
 * code on failure
 */
static int check_channel_index(VideoMasterContext *videomaster_context);

/**
 * @brief Checks the integrity of all arguments passed in the FFmpeg
 * command-line in the VideoMaster context.
 * @param videomaster_context VideoMasterContext pointer to the VideoMaster
 * context
 * @return int  0 on success, or negative AVERROR code on failure
 */
static int check_header_arguments(VideoMasterContext *videomaster_context);

/**
 * @brief Checks the integrity of the timestamp source argument in the
 * VideoMaster context.
 * @param videomaster_context VideoMasterContext pointer to the VideoMaster
 * context
 * @return int  0 on success, or negative AVERROR code on failure
 */
static int check_timestamp_source(VideoMasterContext *videomaster_context);

/**
 * @brief Common error handling for stream operations
 *
 * This function handles errors that occur during stream operations by logging
 * the error message, closing the stream handle, and closing the board handle.
 *
 * @param ctx Pointer to the VideoMaster context
 * @param message Error message to log
 * @param error_code Error code to return
 * @return int 0 on success, or negative AVERROR code on failure
 */
static int handle_stream_error(VideoMasterContext *ctx, const char *message,
                               int error_code);

/**
 * @brief Parses command line arguments for the VideoMaster DELTACAST(c) device.
 *
 * This function extracts the board and channel index from the command line and
 * store them in the context. If a dummy input stream is used, board_index and
 * stream index are taken from the command-line options otherwise, they are
 * deduce from the input name.
 * @param avctx AVFormatContext pointer to the FFmpeg context
 * @return int  0 on success, or negative AVERROR code on failure
 */
static int parse_command_line_arguments(AVFormatContext *avctx);

/**
 * @brief  Sets up the FFmpeg audio stream based on the VideoMaster context
 *
 * This function configures the audio stream properties such as sample rate,
 * channel layout, and codec ID. It also sets up the AVStream parameters for
 * audio data.
 *
 * @param videomaster_context VideoMasterContext pointer to the VideoMaster
 * context
 * @return int 0 on success, or negative AVERROR code on failure
 */
static int setup_audio_stream(VideoMasterContext *videomaster_context);

/**
 * @brief Sets up the streams for the VideoMaster context.
 *
 * This function initializes the video and audio FFmpeg streams based on the
 * properties defined in the VideoMaster context. It configures the AVStream
 * parameters, codec IDs, and other stream-related settings.
 * @param videomaster_context VideoMasterContext pointer to the VideoMaster
 * context
 * @return int 0 on success, or negative AVERROR code on failure.
 */
static int setup_streams(VideoMasterContext *videomaster_context);

/**
 * @brief   Sets up the FFmpeg video stream based
 * on the VideoMaster context
 *
 * This function configures the video stream
 * properties such as width, height, frame rate,
 * pixel format, and codec ID. It also sets up
 * the AVStream parameters for video data. The
 * function is called during the initialization
 * phase of the VideoMaster device to ensure that
 * the video stream is properly configured before
 * starting the stream.
 * @param videomaster_context VideoMasterContext
 * pointer to the VideoMaster
 * @return int 0 on success, or negative AVERROR
 * code on failure
 */
static int setup_video_stream(VideoMasterContext *videomaster_context);

/**** Static functions definitions */
int check_audio_properties(VideoMasterContext *videomaster_context)
{
    enum AVVideoMasterChannelType channel_type =
        ff_videomaster_get_channel_type_from_index(
            videomaster_context->avctx, videomaster_context->board_handle,
            videomaster_context->channel_index);
    if (channel_type == AV_VIDEOMASTER_CHANNEL_HDMI &&
        (videomaster_context->audio_nb_channels != -1 ||
         videomaster_context->audio_sample_rate != -1 ||
         videomaster_context->audio_sample_size != -1))
    {
        av_log(videomaster_context->avctx, AV_LOG_WARNING,
               "Audio properties are not applicable for HDMI channels. These "
               "value will be overridden with auto-detection.\n");
    }
    else
    {
        if (videomaster_context->audio_nb_channels == -1 ||
            videomaster_context->audio_sample_rate ==
                AV_VIDEOMASTER_SAMPLE_RATE_UNKNOWN ||
            videomaster_context->audio_sample_size ==
                AV_VIDEOMASTER_SAMPLE_SIZE_UNKNOWN)
        {
            if (channel_type == AV_VIDEOMASTER_CHANNEL_SDI ||
                channel_type == AV_VIDEOMASTER_CHANNEL_ASISDI)
            {
                av_log(videomaster_context->avctx, AV_LOG_INFO,
                       "SDI audio properties not specified, "
                       "will default to 2ch 48kHz 16-bit. "
                       "Use -nb_channels, -sample_rate, "
                       "-sample_size to override.\n");
            }
            else
            {
                av_log(videomaster_context->avctx, AV_LOG_WARNING,
                       "Invalid audio properties: "
                       "audio_nb_channels=%d, audio_sample_rate=%s, "
                       "audio_sample_size=%s. Audio will be ignored if audio "
                       "stream is present.\n",
                       videomaster_context->audio_nb_channels,
                       ff_videomaster_sample_rate_to_string(
                           videomaster_context->audio_sample_rate),
                       ff_videomaster_sample_size_to_string(
                           videomaster_context->audio_sample_size));
            }
        }
    }
    return 0;
}

int check_board_index(VideoMasterContext *videomaster_context)
{

    if (videomaster_context->number_of_boards == 0)
    {
        av_log(videomaster_context->avctx, AV_LOG_ERROR,
               "No DELTACAST boards detected\n");
        return AVERROR(EIO);
    }

    if (videomaster_context->board_index >=
        videomaster_context->number_of_boards)
    {
        av_log(videomaster_context->avctx, AV_LOG_ERROR,
               "Invalid board index: %d\n", videomaster_context->board_index);
        return AVERROR(EINVAL);
    }

    av_log(videomaster_context->avctx, AV_LOG_TRACE, "Board index is valid.\n");

    if (ff_videomaster_open_board_handle(videomaster_context) != 0)
    {
        av_log(videomaster_context->avctx, AV_LOG_ERROR,
               "Failed to open board handle.\n");
        return AVERROR(EIO);
    }

    av_log(videomaster_context->avctx, AV_LOG_TRACE,
           "Board handle opened successfully\n");

    return 0;
}

int check_channel_index(VideoMasterContext *videomaster_context)
{
    videomaster_context->has_video = false;
    videomaster_context->has_audio = false;

    if (ff_videomaster_get_nb_rx_channels(videomaster_context) == 0)
    {
        if (videomaster_context->channel_index >=
            videomaster_context->nb_rx_channels)
        {
            av_log(videomaster_context->avctx, AV_LOG_ERROR,
                   "Invalid channel index: %d\n",
                   videomaster_context->channel_index);
            return AVERROR(EINVAL);
        }
        else if (!ff_videomaster_is_channel_locked(videomaster_context))
        {
            if (videomaster_context->no_autodetect_timeout)
            {
                av_log(videomaster_context->avctx, AV_LOG_INFO,
                       "Waiting for signal on channel %d...\n",
                       videomaster_context->channel_index);
                while (!ff_videomaster_is_channel_locked(videomaster_context))
                    av_usleep(100000);
                av_log(videomaster_context->avctx, AV_LOG_INFO,
                       "Signal detected on channel %d\n",
                       videomaster_context->channel_index);
            }
            else
            {
                int i;
                for (i = 0; i < 30; i++)
                {
                    av_usleep(100000);
                    if (ff_videomaster_is_channel_locked(videomaster_context))
                        break;
                }
                if (i >= 30)
                {
                    av_log(videomaster_context->avctx, AV_LOG_TRACE,
                           "Channel %d is not locked\n",
                           videomaster_context->channel_index);
                    /* Fall through to default format if signal_no_stop */
                }
            }

            /* If still no signal, use default format or give up */
            if (!ff_videomaster_is_channel_locked(videomaster_context))
            {
                struct VideoMasterData *data =
                    (struct VideoMasterData *)videomaster_context->avctx->priv_data;
                if (videomaster_context->signal_no_stop &&
                    data->default_video_mode >= 0)
                {
                    if (resolve_video_mode(
                            videomaster_context->avctx,
                            (int)data->default_video_mode,
                            &videomaster_context->video_width,
                            &videomaster_context->video_height,
                            &videomaster_context->video_frame_rate_num,
                            &videomaster_context->video_frame_rate_den,
                            &videomaster_context->video_interlaced) < 0)
                        return AVERROR(EINVAL);

                    videomaster_context->has_video = true;
                    videomaster_context->video_codec = AV_CODEC_ID_RAWVIDEO;
                    videomaster_context->video_pixel_format = AV_PIX_FMT_UYVY422;
                    videomaster_context->video_buffer_packing =
                        AV_VIDEOMASTER_BUFFER_PACKING_YUV422_8;

                    /* Default audio: 48kHz 16-bit stereo */
                    if (videomaster_context->audio_sample_rate == 0 ||
                        videomaster_context->audio_sample_rate ==
                            AV_VIDEOMASTER_SAMPLE_RATE_UNKNOWN)
                        videomaster_context->audio_sample_rate = 48000;
                    if (videomaster_context->audio_nb_channels == 0 ||
                        (int32_t)videomaster_context->audio_nb_channels == -1)
                        videomaster_context->audio_nb_channels = 2;
                    if (videomaster_context->audio_sample_size == 0 ||
                        videomaster_context->audio_sample_size ==
                            AV_VIDEOMASTER_SAMPLE_SIZE_UNKNOWN)
                        videomaster_context->audio_sample_size = 16;
                    videomaster_context->audio_codec = AV_CODEC_ID_PCM_S16LE;
                    videomaster_context->has_audio = true;

                    videomaster_context->generating_black = true;
                    av_log(videomaster_context->avctx, AV_LOG_INFO,
                           "No signal - using default format %ux%u@%u/%u, "
                           "generating black frames\n",
                           videomaster_context->video_width,
                           videomaster_context->video_height,
                           videomaster_context->video_frame_rate_num,
                           videomaster_context->video_frame_rate_den);
                    return 0;
                }
                return 0;
            }
        }
        else
        {
            av_log(videomaster_context->avctx, AV_LOG_TRACE,
                   "Channel index is valid\n");
            if (ff_videomaster_get_video_stream_properties(
                    videomaster_context->avctx,
                    videomaster_context->board_handle,
                    videomaster_context->stream_handle,
                    videomaster_context->channel_index,
                    &videomaster_context->channel_type,
                    &videomaster_context->video_info,
                    &videomaster_context->video_width,
                    &videomaster_context->video_height,
                    &videomaster_context->video_frame_rate_num,
                    &videomaster_context->video_frame_rate_den,
                    &videomaster_context->video_interlaced) == 0)
            {
                videomaster_context->has_video = true;
                float frame_rate =
                    (float)videomaster_context->video_frame_rate_num /
                    videomaster_context->video_frame_rate_den;
                if (videomaster_context->channel_type ==
                    AV_VIDEOMASTER_CHANNEL_HDMI)
                {
                    av_log(
                        videomaster_context->avctx, AV_LOG_TRACE,
                        "Stream properties: %dx%d@%.3f %s %s\n",
                        videomaster_context->video_width,
                        videomaster_context->video_height, frame_rate,
                        VHD_DV_CS_ToPrettyString(
                            videomaster_context->video_info.hdmi.color_space),
                        VHD_DV_SAMPLING_ToPrettyString(
                            videomaster_context->video_info.hdmi
                                .cable_bit_sampling));
                    av_log(videomaster_context->avctx, AV_LOG_TRACE,
                           "Pixel clock: %d\n",
                           videomaster_context->video_info.hdmi.pixel_clock);
                    av_log(videomaster_context->avctx, AV_LOG_TRACE,
                           "Interlaced: %s\n",
                           videomaster_context->video_interlaced ? "true"
                                                                 : "false");
                    av_log(
                        videomaster_context->avctx, AV_LOG_TRACE,
                        "Color space: %s\n",
                        VHD_DV_CS_ToPrettyString(
                            videomaster_context->video_info.hdmi.color_space));
                    av_log(videomaster_context->avctx, AV_LOG_TRACE,
                           "Cable bit sampling: %s\n",
                           VHD_DV_SAMPLING_ToPrettyString(
                               videomaster_context->video_info.hdmi
                                   .cable_bit_sampling));
                    av_log(videomaster_context->avctx, AV_LOG_TRACE,
                           "Selected Buffer Packing: %s\n",
                           VHD_BUFFERPACKING_ToPrettyString(
                               videomaster_context->video_buffer_packing));
                }
                else
                {
                    av_log(
                        videomaster_context->avctx, AV_LOG_TRACE,
                        "Stream properties: %dx%d@%.3f %s %s\n",
                        videomaster_context->video_width,
                        videomaster_context->video_height, frame_rate,
                        VHD_VIDEOSTANDARD_ToPrettyString(
                            videomaster_context->video_info.sdi.video_standard),
                        VHD_CLOCKDIVISOR_ToPrettyString(
                            videomaster_context->video_info.sdi.clock_divisor));
                    av_log(videomaster_context->avctx, AV_LOG_TRACE,
                           "Interface: %s\n",
                           VHD_INTERFACE_ToPrettyString(
                               videomaster_context->video_info.sdi.interface));
                    av_log(videomaster_context->avctx, AV_LOG_TRACE,
                           "Genlock offset: %d\n",
                           videomaster_context->video_info.sdi.genlock_offset);
                }

                if (ff_videomaster_open_stream_handle(videomaster_context) == 0)
                {
                    av_log(videomaster_context->avctx, AV_LOG_TRACE,
                           "Stream handle opened successfully\n");
                }
                else
                {
                    av_log(videomaster_context->avctx, AV_LOG_ERROR,
                           "Failed to open stream handle.\n");
                    return AVERROR(EIO);
                }
            }
            else
            {
                av_log(videomaster_context->avctx, AV_LOG_ERROR,
                       "Failed to get stream properties\n");
                return AVERROR(EIO);
            }

            if (ff_videomaster_get_audio_stream_properties(
                    videomaster_context->avctx,
                    videomaster_context->board_handle,
                    videomaster_context->stream_handle,
                    videomaster_context->channel_index,
                    videomaster_context->video_buffer_packing,
                    &videomaster_context->channel_type,
                    &videomaster_context->audio_info,
                    &videomaster_context->audio_sample_rate,
                    &videomaster_context->audio_nb_channels,
                    &videomaster_context->audio_sample_size,
                    &videomaster_context->audio_codec) == 0)
            {
                if (videomaster_context->channel_type ==
                    AV_VIDEOMASTER_CHANNEL_HDMI)
                {
                    if (videomaster_context->audio_sample_size != 0 &&
                        videomaster_context->audio_nb_channels != 0)
                    {
                        videomaster_context->has_audio = true;
                        av_log(
                            videomaster_context->avctx, AV_LOG_TRACE,
                            "Audio properties: %d channels @%dHz (%d bits)\n",
                            videomaster_context->audio_nb_channels,
                            videomaster_context->audio_sample_rate,
                            videomaster_context->audio_sample_size);
                    }
                    else
                    {
                        av_log(videomaster_context->avctx, AV_LOG_WARNING,
                               "Audio properties: No audio detected\n");
                    }
                }
                else
                {
                    if (videomaster_context->audio_sample_size !=
                            AV_VIDEOMASTER_SAMPLE_SIZE_UNKNOWN &&
                        videomaster_context->audio_sample_rate !=
                            AV_VIDEOMASTER_SAMPLE_RATE_UNKNOWN &&
                        videomaster_context->audio_nb_channels != 0)
                    {
                        videomaster_context->has_audio = true;
                        av_log(
                            videomaster_context->avctx, AV_LOG_TRACE,
                            "Audio properties: %d channels @%dHz (%d bits)\n",
                            videomaster_context->audio_nb_channels,
                            videomaster_context->audio_sample_rate,
                            videomaster_context->audio_sample_size);
                    }
                    else
                    {
                        av_log(videomaster_context->avctx, AV_LOG_WARNING,
                               "Audio properties: No audio detected\n");
                    }
                }
            }
            else
            {
                av_log(videomaster_context->avctx, AV_LOG_WARNING,
                       "Failed to get audio properties\n");
            }
        }
    }
    else
    {
        av_log(videomaster_context->avctx, AV_LOG_ERROR,
               "Failed to get number of RX channels\n");
        return AVERROR(EIO);
    }

    return 0;
}

int check_header_arguments(VideoMasterContext *videomaster_context)
{

    if (check_board_index(videomaster_context) != 0)
    {
        av_log(videomaster_context->avctx, AV_LOG_ERROR,
               "Failed to check board index integrity\n");
        return AVERROR(EIO);
    }

    if (check_audio_properties(videomaster_context) != 0)
    {
        av_log(videomaster_context->avctx, AV_LOG_ERROR,
               "Failed to check audio properties integrity\n");
        ff_videomaster_close_board_handle(videomaster_context);
        return AVERROR(EIO);
    }

    if (check_channel_index(videomaster_context) != 0)
    {
        av_log(videomaster_context->avctx, AV_LOG_ERROR,
               "Failed to check channel index integrity\n");
        ff_videomaster_close_board_handle(videomaster_context);
        return AVERROR(EIO);
    }

    check_timestamp_source(videomaster_context);

    return 0;
}

int check_timestamp_source(VideoMasterContext *videomaster_context)
{
    VHD_TIMECODE  time_code;
    BOOL32        ltc_source_is_locked;
    float         ltc_source_frame_rate;
    VHD_ERRORCODE error_code;

    if (videomaster_context->timestamp_source ==
            AV_VIDEOMASTER_TIMESTAMP_HARDWARE &&
        !ff_videomaster_is_hardware_timestamp_supported(videomaster_context))
    {
        av_log(videomaster_context->avctx, AV_LOG_WARNING,
               "Hardware time stamping is not supported on the device. "
               "Falling back to system clock.\n");
        videomaster_context->timestamp_source =
            AV_VIDEOMASTER_TIMESTAMP_OSCILLATOR;
    }
    else if (videomaster_context->timestamp_source ==
             AV_VIDEOMASTER_TIMESTAMP_LTC_COMPANION_CARD)
    {
        if (!ff_videomaster_is_ltc_companion_card_supported(
                videomaster_context))
        {
            av_log(videomaster_context->avctx, AV_LOG_WARNING,
                   "LTC companion card feature is not supported on the device. "
                   "Falling back to system clock.\n");
            videomaster_context->timestamp_source =
                AV_VIDEOMASTER_TIMESTAMP_OSCILLATOR;
        }
        else
        {
            if (!ff_videomaster_is_ltc_companion_card_present(
                    videomaster_context))
            {
                av_log(videomaster_context->avctx, AV_LOG_WARNING,
                       "LTC companion card is not detected. "
                       "Falling back to system clock.\n");
                videomaster_context->timestamp_source =
                    AV_VIDEOMASTER_TIMESTAMP_OSCILLATOR;
            }
        }
    }
    else if (videomaster_context->timestamp_source ==
             AV_VIDEOMASTER_TIMESTAMP_LTC_ON_BOARD)
    {
        if (!ff_videomaster_is_ltc_on_board_timestamp_supported(
                videomaster_context))
        {
            av_log(videomaster_context->avctx, AV_LOG_WARNING,
                   "LTC on-board feature is not supported on the device. "
                   "Falling back to system clock.\n");
            videomaster_context->timestamp_source =
                AV_VIDEOMASTER_TIMESTAMP_OSCILLATOR;
        }

        if (videomaster_context->auto_set_ltc_input)
        {
            uint32_t nb_ref_in = 0;
            VHD_GetBoardCapability(videomaster_context->board_handle,
                                   VHD_CORE_BOARD_CAP_REF_IN, &nb_ref_in);

            av_log(videomaster_context->avctx, AV_LOG_DEBUG,
                   "auto_set_ltc_input: board has %u REF_IN connector(s)\n",
                   nb_ref_in);

            if (nb_ref_in >= 1)
            {
                VHD_SetBoardProperty(videomaster_context->board_handle,
                                     VHD_SDI_BP_REF_IN0_DETECTION_ENABLE,
                                     FALSE);
                av_log(videomaster_context->avctx, AV_LOG_DEBUG,
                       "auto_set_ltc_input: disabled REF_IN0 detection\n");
            }

            if (nb_ref_in >= 2)
            {
                VHD_SetBoardProperty(videomaster_context->board_handle,
                                     VHD_SDI_BP_REF_IN1_DETECTION_ENABLE,
                                     TRUE);
                VHD_SetBoardProperty(videomaster_context->board_handle,
                                     VHD_SDI_BP_GENLOCK_SOURCE,
                                     VHD_GENLOCK_REF_IN1);
                av_log(videomaster_context->avctx, AV_LOG_DEBUG,
                       "auto_set_ltc_input: enabled REF_IN1 detection and "
                       "set genlock source to REF_IN1\n");
            }
        }
    }

    if (videomaster_context->timestamp_source ==
            AV_VIDEOMASTER_TIMESTAMP_LTC_COMPANION_CARD ||
        videomaster_context->timestamp_source ==
            AV_VIDEOMASTER_TIMESTAMP_LTC_ON_BOARD)
    {
        error_code = VHD_GetTimecode(
            videomaster_context->board_handle,
            (videomaster_context->timestamp_source ==
             AV_VIDEOMASTER_TIMESTAMP_LTC_COMPANION_CARD)
                ? VHD_TC_SRC_LTC_COMPANION_CARD
                : VHD_TC_SRC_LTC_ONBOARD,
            &ltc_source_is_locked, &ltc_source_frame_rate, &time_code);
        if (error_code == VHDERR_NOERROR)
        {
            av_log(videomaster_context->avctx, AV_LOG_DEBUG,
                   "LTC Time code: %02d:%02d:%02d:%02d\n", time_code.Hour,
                   time_code.Minute, time_code.Second, time_code.Frame);
            if (ltc_source_is_locked)
            {
                av_log(videomaster_context->avctx, AV_LOG_DEBUG,
                       "LTC source is locked at %.3f fps.\n",
                       ltc_source_frame_rate);
                /* Always capture the LTC frame rate when locked — it is used
                 * to convert H:M:S:F to microseconds for the first-PTS anchor
                 * (videomaster_common.c LTC branch). Subsequent PTS are
                 * derived from the video frame duration, so a mismatch between
                 * LTC and video rates only affects the anchor conversion, not
                 * downstream timing. The warning below is kept as info. */
                videomaster_context->ltc_frame_rate = ltc_source_frame_rate;
                if (videomaster_context->has_video)
                {
                    float video_frame_rate =
                        (float)videomaster_context->video_frame_rate_num /
                        videomaster_context->video_frame_rate_den;
                    if (ltc_source_frame_rate != video_frame_rate)
                    {
                        av_log(videomaster_context->avctx, AV_LOG_WARNING,
                               "LTC frame rate (%.3f fps) does not match "
                               "video frame rate (%.3f fps). Anchor PTS is "
                               "still computed from LTC; subsequent PTS "
                               "follow the video frame duration.\n",
                               ltc_source_frame_rate, video_frame_rate);
                    }
                }
            }
            else
            {
                av_log(videomaster_context->avctx, AV_LOG_WARNING,
                       "LTC source is not locked. No timecode will be "
                       "available until the LTC source is locked.\n");
            }
        }
        else
        {
            char pLastErrorMessage[VHD_MAX_ERROR_STRING_SIZE] = { 0 };
            VHD_GetLastErrorMessage(pLastErrorMessage,
                                    VHD_MAX_ERROR_STRING_SIZE);
            av_log(videomaster_context->avctx, AV_LOG_DEBUG,
                   "VHDERR = %d - %s\n%s\n", error_code,
                   VHD_ERRORCODE_ToPrettyString(error_code), pLastErrorMessage);
            av_log(videomaster_context->avctx, AV_LOG_WARNING,
                   "Cannot get LTC timecode. "
                   "Falling back to system clock.\n");
            videomaster_context->timestamp_source =
                AV_VIDEOMASTER_TIMESTAMP_OSCILLATOR;
        }
    }
    return 0;
}

int handle_stream_error(VideoMasterContext *ctx, const char *message,
                        int error_code)
{
    av_log(ctx->avctx, AV_LOG_ERROR, "%s\n", message);
    ff_videomaster_close_stream_handle(ctx);
    ff_videomaster_close_board_handle(ctx);
    return error_code;
}

int parse_command_line_arguments(AVFormatContext *avctx)
{
    struct VideoMasterData    *videomaster_data = NULL;
    struct VideoMasterContext *videomaster_context = NULL;

    if (ff_videomaster_extract_context(avctx, &videomaster_data,
                                       &videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to extract context\n");
        return AVERROR(EINVAL);
    }
    else
    {
        if (strcmp(avctx->url, "dummy") == 0)
        {
            av_log(avctx, AV_LOG_TRACE,
                   "Dummy input is selected. Deduce board and channel index "
                   "from command line "
                   "parameter\n");
            if (videomaster_data->board_index == -1)
            {
                av_log(avctx, AV_LOG_ERROR,
                       "Board index is not set. Please use the dedicated "
                       "option when using "
                       "\"dummy\" "
                       "input source.\n");
                return AVERROR(EINVAL);
            }

            if (videomaster_data->channel_index == -1)
            {
                av_log(avctx, AV_LOG_ERROR,
                       "Board index is not set. Please use the dedicated "
                       "option when using "
                       "\"dummy\" "
                       "input source.\n");
                return AVERROR(EINVAL);
            }
            videomaster_context->board_index = videomaster_data->board_index;
            videomaster_context->channel_index =
                videomaster_data->channel_index;
        }
        else
        {
            av_log(avctx, AV_LOG_TRACE,
                   "\"%s\" is selected. Parse string to get board and channel "
                   "index.\n",
                   avctx->url);
            char board_id[64] = { 0 };
            if (sscanf(avctx->url, "stream %d on board id %63s",
                       &videomaster_context->channel_index, board_id) == 2)
            {
                int ret = ff_videomaster_find_board_index_by_id(
                    videomaster_context, board_id,
                    &videomaster_context->board_index);
                if (ret < 0)
                {
                    av_log(avctx, AV_LOG_ERROR,
                           "No board found with id \"%s\".\n", board_id);
                    return ret;
                }
            }
            else if (sscanf(avctx->url, "stream %d on board %d",
                            &videomaster_context->channel_index,
                            &videomaster_context->board_index) != 2)
            {
                av_log(avctx, AV_LOG_ERROR,
                       "Unknown stream selected : \"%s\". Please use \"ffmpeg "
                       "-sources "
                       "videmaster\" and "
                       "use the correct source name.\n",
                       avctx->url);
                return AVERROR(EINVAL);
            }
        }

        if (videomaster_data->timestamp_source >= 0 &&
            videomaster_data->timestamp_source < AV_VIDEOMASTER_TIMESTAMP_NB)
        {
            videomaster_context->timestamp_source =
                (enum AVVideoMasterTimeStampType)
                    videomaster_data->timestamp_source;
        }
        else
        {
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid timestamp_source value: %" PRId64 "\n",
                   videomaster_data->timestamp_source);
            return AVERROR(EINVAL);
        }

        videomaster_context->audio_nb_channels = videomaster_data->nb_channels;
        videomaster_context->audio_sample_rate = videomaster_data->sample_rate;
        videomaster_context->audio_sample_size = videomaster_data->sample_size;

        switch (videomaster_data->sample_size)
        {
        case AV_VIDEOMASTER_SAMPLE_SIZE_16:
            videomaster_context->audio_codec = AV_CODEC_ID_PCM_S16LE;
            break;
        case AV_VIDEOMASTER_SAMPLE_SIZE_24:
            videomaster_context->audio_codec = AV_CODEC_ID_PCM_S24LE;
            break;
        }

        videomaster_context->video_buffer_packing =
            videomaster_data->buffer_packing;

        videomaster_context->auto_set_ltc_input =
            videomaster_data->auto_set_ltc_input;

        videomaster_context->wait_for_input =
            videomaster_data->wait_for_input;

        videomaster_context->wait_for_tc =
            videomaster_data->wait_for_tc;

        videomaster_context->no_autodetect_timeout =
            videomaster_data->no_autodetect_timeout;

        videomaster_context->signal_no_stop =
            videomaster_data->signal_no_stop;

        videomaster_context->disjoined_streams =
            videomaster_data->disjoined_streams;

        if (videomaster_context->disjoined_streams)
            av_log(avctx, AV_LOG_INFO,
                   "Disjoined streams mode requested\n");
    }

    av_log(avctx, AV_LOG_INFO,
           "Board index: %d, Stream index: %d, Timestamp source: %s, Selected "
           "buffer packing: %s\n",
           videomaster_context->board_index, videomaster_context->channel_index,
           ff_videomaster_timestamp_type_to_string(
               videomaster_context->timestamp_source),
           VHD_BUFFERPACKING_ToPrettyString(
               videomaster_context->video_buffer_packing));

    return 0;
}

int setup_audio_stream(VideoMasterContext *videomaster_context)
{
    if (videomaster_context->has_audio)
    {
        AVStream *av_stream = avformat_new_stream(videomaster_context->avctx,
                                                  NULL);
        if (!av_stream)
        {
            av_log(videomaster_context->avctx, AV_LOG_ERROR,
                   "Failed to create new stream\n");
            return AVERROR(ENOMEM);
        }
        av_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        av_stream->codecpar->codec_id = videomaster_context->audio_codec;
        av_stream->codecpar->sample_rate =
            videomaster_context->audio_sample_rate;
        av_stream->codecpar->ch_layout.nb_channels =
            videomaster_context->audio_nb_channels;
        avpriv_set_pts_info(av_stream, 64, 1, 1000000); /* 64 bits pts in us */
        videomaster_context->audio_stream = av_stream;
    }

    return 0;
}

int setup_streams(VideoMasterContext *videomaster_context)
{
    int error_code = setup_video_stream(videomaster_context);
    if (error_code != 0)
        return handle_stream_error(videomaster_context,
                                   "Failed to setup video stream", error_code);
    error_code = setup_audio_stream(videomaster_context);
    if (error_code != 0)
        return handle_stream_error(videomaster_context,
                                   "Failed to setup audio stream", error_code);

    return 0;
}

int setup_video_stream(VideoMasterContext *videomaster_context)
{
    if (videomaster_context->has_video)
    {
        AVStream *av_stream = avformat_new_stream(videomaster_context->avctx,
                                                  NULL);
        if (!av_stream)
        {
            av_log(videomaster_context->avctx, AV_LOG_ERROR,
                   "Failed to create new stream\n");
            return AVERROR(ENOMEM);
        }
        av_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        av_stream->codecpar->width = videomaster_context->video_width;
        av_stream->codecpar->height = videomaster_context->video_height;
        av_stream->time_base.den = videomaster_context->video_frame_rate_num;
        av_stream->time_base.num = videomaster_context->video_frame_rate_den;
        av_stream->r_frame_rate =
            av_make_q(videomaster_context->video_frame_rate_num,
                      videomaster_context->video_frame_rate_den);
        av_stream->codecpar->bit_rate = videomaster_context->video_bit_rate;
        av_stream->codecpar->codec_id = videomaster_context->video_codec;
        av_stream->codecpar->format = videomaster_context->video_pixel_format;
        av_stream->codecpar->field_order = videomaster_context->video_interlaced ? AV_FIELD_TT : AV_FIELD_PROGRESSIVE;

        avpriv_set_pts_info(av_stream, 64, 1, 1000000); /* 64 bits pts in us */

        videomaster_context->video_stream = av_stream;
    }

    return 0;
}

/**** Public functions definitions */
int ff_videomaster_list_input_devices(AVFormatContext         *avctx,
                                      struct AVDeviceInfoList *device_list)
{
    struct VideoMasterData    *videomaster_data = NULL;
    struct VideoMasterContext *videomaster_context = NULL;

    if (ff_videomaster_extract_context(avctx, &videomaster_data,
                                       &videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to extract context\n");
        return AVERROR(EINVAL);
    }

    if (!device_list)
    {
        av_log(avctx, AV_LOG_ERROR, "device_list is NULL!\n");
        return AVERROR(EINVAL);
    }

    if (ff_videomaster_get_api_info(videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to get API version or number of boards\n");
        return AVERROR(EIO);
    }

    if (videomaster_context->number_of_boards == 0)
    {
        av_log(avctx, AV_LOG_INFO, "No DELTACAST boards detected\n");
        return AVERROR(EIO);
    }

    for (uint32_t i = 0; i < videomaster_context->number_of_boards; i++)
    {

        if (ff_videomaster_create_devices_infos_from_board_index(
                videomaster_context, i, &device_list) < 0)
        {
            av_log(avctx, AV_LOG_ERROR,
                   "Failed to create devices infos for board %d\n", i);
            return AVERROR(EIO);
        }
    }

    return 0;
}

int ff_videomaster_read_close(AVFormatContext *avctx)
{
    int                        return_code = 0;
    struct VideoMasterData    *videomaster_data = NULL;
    struct VideoMasterContext *videomaster_context = NULL;
    if (ff_videomaster_extract_context(avctx, &videomaster_data,
                                       &videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to extract context\n");
        return AVERROR(EINVAL);
    }

    /* liveedit: close the audio IPC pipe early so the drain thread is
     * joined before we free the context it references. */
    if (videomaster_context->audio_pipe)
    {
        ff_videomaster_audio_pipe_close(videomaster_context->audio_pipe);
        videomaster_context->audio_pipe = NULL;
    }

    if (ff_videomaster_release_data(videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to release data\n");
        return AVERROR(EIO);
    }

    if (ff_videomaster_stop_stream(videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to stop stream\n");
        return_code = AVERROR(EIO);
    }
    else
    {
        av_log(avctx, AV_LOG_TRACE, "Stream stopped successfully\n");
    }

    if (videomaster_context->stream_handle)
    {
        if (ff_videomaster_close_stream_handle(videomaster_context) != 0)
        {
            av_log(avctx, AV_LOG_ERROR, "Failed to close stream handle: %d\n",
                   return_code);
            return_code = AVERROR(EIO);
        }
        else
        {
            av_log(avctx, AV_LOG_TRACE, "Stream handle closed successfully\n");
            videomaster_context->stream_handle = NULL;
        }
    }

    if (videomaster_context->board_handle)
    {
        if (ff_videomaster_close_board_handle(videomaster_context) != 0)
        {
            av_log(avctx, AV_LOG_ERROR, "Failed to close board handle: %d\n",
                   return_code);
            return_code = AVERROR(EIO);
        }
        else
        {
            av_log(avctx, AV_LOG_TRACE, "Stream handle board successfully\n");
            videomaster_context->board_handle = NULL;
        }
    }

    if (videomaster_context)
    {
        av_freep(&videomaster_context->black_video_buffer);
        av_freep(&videomaster_context->silent_audio_buffer);
        av_freep(&videomaster_data->context);
        videomaster_data->context = NULL;
        videomaster_context = NULL;
    }

    return return_code;
}

int ff_videomaster_read_header(AVFormatContext *avctx)
{
    struct VideoMasterData    *videomaster_data = NULL;
    struct VideoMasterContext *videomaster_context = NULL;
    void                      *board_init_lock = NULL;

    if (ff_videomaster_extract_context(avctx, &videomaster_data,
                                       &videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to extract context\n");
        return AVERROR(EINVAL);
    }

    if (videomaster_data->list_formats) {
        list_video_formats(avctx);
        return AVERROR_EXIT;
    }

    if (parse_command_line_arguments(avctx) != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to parse command line arguments\n");
        return AVERROR(EINVAL);
    }

    if (ff_videomaster_get_api_info(videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to get API version or number of boards\n");
        return AVERROR(EIO);
    }

    /* Serialize the board-property setup phase across processes. The lock
     * is released as soon as start_stream returns; capture is unaffected. */
    board_init_lock = ff_videomaster_acquire_board_init_lock(
        (uint32_t)videomaster_context->board_index, avctx);

    if (check_header_arguments(videomaster_context) != 0)
    {
        ff_videomaster_release_board_init_lock(board_init_lock, avctx);
        av_log(avctx, AV_LOG_ERROR,
               "Failed to check header arguments integrity\n");
        return AVERROR(EIO);
    }

    if (videomaster_context->has_video)
    {
        av_log(avctx, AV_LOG_INFO, "Found video mode %u x %u with rate %.2f%s\n",
               videomaster_context->video_width,
               videomaster_context->video_height,
               (double)videomaster_context->video_frame_rate_num /
                   videomaster_context->video_frame_rate_den,
               videomaster_context->video_interlaced ? "(i)" : "");
    }

    if (videomaster_context->generating_black)
    {
        /* No hardware stream — release lock immediately, then create
         * AVStreams and black buffers (no board state touched). */
        ff_videomaster_release_board_init_lock(board_init_lock, avctx);
        board_init_lock = NULL;

        if (setup_streams(videomaster_context) != 0)
        {
            return handle_stream_error(videomaster_context,
                                       "Failed to setup Audio and Video streams\n",
                                       AVERROR(EIO));
        }
        if (allocate_black_and_silent_buffers(videomaster_context) != 0)
        {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate black buffers\n");
            return AVERROR(ENOMEM);
        }
    }
    else
    {
        if ((videomaster_context->has_video || videomaster_context->has_audio) &&
            (ff_videomaster_start_stream(videomaster_context) != 0))
        {
            ff_videomaster_release_board_init_lock(board_init_lock, avctx);
            return handle_stream_error(videomaster_context,
                                       "Failed to start stream\n", AVERROR(EIO));
        }

        /* Board-property setup is done — release lock so other processes
         * can proceed in parallel with the rest of our init. */
        ff_videomaster_release_board_init_lock(board_init_lock, avctx);
        board_init_lock = NULL;

        if (setup_streams(videomaster_context) != 0)
        {
            return handle_stream_error(videomaster_context,
                                       "Failed to setup Audio and Video streams\n",
                                       AVERROR(EIO));
        }

        /* Pre-allocate black buffers for signal loss during capture */
        if (videomaster_context->signal_no_stop)
            allocate_black_and_silent_buffers(videomaster_context);
    }

    videomaster_context->return_video_next = true;
    videomaster_context->disjoined_first_after_wait =
        videomaster_context->disjoined_streams &&
        (videomaster_context->wait_for_input || videomaster_context->wait_for_tc);

    if (videomaster_context->wait_for_input)
        av_log(avctx, AV_LOG_INFO, "WAIT FOR USER INPUT KEY : r\n");
    if (videomaster_context->wait_for_tc)
        av_log(avctx, AV_LOG_INFO, "WAIT FOR LOCKED LTC TIMECODE\n");

    /* liveedit: open the audio IPC pipe now that audio properties are
     * known. The pipe runs in its own thread and is decoupled from
     * wait_for_input/wait_for_tc so a second process can consume audio
     * from the very first captured frame. */
    if (videomaster_data->audio_pipe && *videomaster_data->audio_pipe &&
        videomaster_context->has_audio)
    {
        videomaster_context->audio_pipe = ff_videomaster_audio_pipe_create(
            avctx, videomaster_data->audio_pipe,
            videomaster_context->audio_sample_rate,
            videomaster_context->audio_nb_channels,
            videomaster_context->audio_sample_size);
        if (!videomaster_context->audio_pipe)
            av_log(avctx, AV_LOG_WARNING,
                   "audio_pipe init failed — continuing without IPC audio\n");
    }

    return 0;
}

int ff_videomaster_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct VideoMasterData    *videomaster_data = NULL;
    struct VideoMasterContext *videomaster_context = NULL;

    if (ff_videomaster_extract_context(avctx, &videomaster_data,
                                       &videomaster_context) != 0)
    {
        av_log(avctx, AV_LOG_ERROR, "Failed to extract context\n");
        return AVERROR(EINVAL);
    }

    /* ── Unified wait loop: handles 'q', wait_for_input, and wait_for_tc ──
     *
     * Monitors stdin ('q' to quit, 'r' to start, 'f' to force start) and LTC lock status
     * simultaneously on every frame. The LTC status is tracked live: it can
     * go from locked to unlocked and back. When 'r' arrives, we only skip
     * the wait_for_tc loop if the LTC is locked RIGHT NOW.
     */
    {
        int want_tc = videomaster_context->wait_for_tc;
        int tc_is_locked = 0;

        while (videomaster_context->wait_for_input ||
               (want_tc && !tc_is_locked))
        {
            int key;

            /* Consume a frame to keep the hardware running */
            if (ff_videomaster_get_data(videomaster_context) != 0)
            {
                av_usleep(100000);
                key = vm_read_key();
                if (key == 'q' || key == 'Q') {
                    av_log(avctx, AV_LOG_INFO, "Quit requested\n");
                    return AVERROR_EOF;
                }
                continue;
            }

            /* liveedit: audio flows to IPC even during the wait loop so
             * the downstream process sees a continuous stream. */
            if (videomaster_context->audio_pipe &&
                videomaster_context->audio_buffer &&
                videomaster_context->audio_buffer_size > 0)
            {
                ff_videomaster_audio_pipe_push(
                    videomaster_context->audio_pipe,
                    videomaster_context->audio_buffer,
                    videomaster_context->audio_buffer_size);
            }

            /* Always probe LTC status (even during wait_for_input) */
            if (want_tc)
            {
                BOOL32 locked = FALSE;
                float  fps = 0;
                VHD_TIMECODE tc_probe;
                int prev_locked = tc_is_locked;
                tc_is_locked = 0;

                if (VHD_GetTimecode(videomaster_context->board_handle,
                                    VHD_TC_SRC_LTC_ONBOARD, &locked,
                                    &fps, &tc_probe) == VHDERR_NOERROR ||
                    VHD_GetTimecode(videomaster_context->board_handle,
                                    VHD_TC_SRC_LTC_COMPANION_CARD, &locked,
                                    &fps, &tc_probe) == VHDERR_NOERROR)
                {
                    videomaster_context->ltc_frame_rate = fps;
                    if (locked && fps > 0)
                    {
                        tc_is_locked = 1;
                        if (!prev_locked)
                            av_log(avctx, AV_LOG_INFO,
                                   "LTC locked at %.3f fps - TC: %02d:%02d:%02d:%02d\n",
                                   fps, tc_probe.Hour, tc_probe.Minute,
                                   tc_probe.Second, tc_probe.Frame);
                    }
                    else if (prev_locked)
                    {
                        av_log(avctx, AV_LOG_WARNING, "LTC unlocked\n");
                    }
                }
                else if (prev_locked)
                {
                    av_log(avctx, AV_LOG_WARNING, "LTC unlocked\n");
                }
            }

            ff_videomaster_release_data(videomaster_context);

            /* Check stdin */
            key = vm_read_key();
            if (key >= 0)
                av_log(avctx, AV_LOG_DEBUG,
                       "stdin key received: '%c' (0x%02x) "
                       "[wait_for_input=%d, wait_for_tc=%d, tc_locked=%d]\n",
                       key > 31 ? key : '?', key,
                       videomaster_context->wait_for_input,
                       want_tc, tc_is_locked);
            if (key == 'q' || key == 'Q') {
                av_log(avctx, AV_LOG_INFO, "Quit requested\n");
                return AVERROR_EOF;
            }
            if (videomaster_context->wait_for_input) {
                if (key == 'r' || key == 'f') {
                    av_log(avctx, AV_LOG_INFO, "WAIT FOR INPUT END.\n");
                    videomaster_context->wait_for_input = 0;
                }
                if (key == 'f') {
                    if (want_tc) {
                        av_log(avctx, AV_LOG_INFO, "FORCE START.\n");
                        want_tc = 0;
                    }
                }
            }
        }
    }

    /* In disjoined mode, drain one frame after the wait loop to ensure
     * the first real capture frame is not a duplicate of the last
     * consumed frame from the wait loop. */
    if (videomaster_context->disjoined_streams &&
        videomaster_context->disjoined_first_after_wait)
    {
        videomaster_context->disjoined_first_after_wait = false;
        if (ff_videomaster_get_data(videomaster_context) == 0) {
            if (videomaster_context->audio_pipe &&
                videomaster_context->audio_buffer &&
                videomaster_context->audio_buffer_size > 0)
            {
                ff_videomaster_audio_pipe_push(
                    videomaster_context->audio_pipe,
                    videomaster_context->audio_buffer,
                    videomaster_context->audio_buffer_size);
            }
            ff_videomaster_release_data(videomaster_context);
        }
        /* Discard any LTC PTS that was anchored during the wait/drain phase
         * (disjoined mode calls ff_videomaster_get_timestamp from inside
         * ff_videomaster_get_data, so the anchor fired too early). Clearing
         * this flag forces the next real capture frame to re-anchor from
         * the LTC value at the actual recording start moment. */
        videomaster_context->ltc_pts_anchored = false;
    }

    /* ── Generating black frames (no signal) ── */
    if (videomaster_context->generating_black)
    {
        int64_t frame_dur_us = (int64_t)videomaster_context->video_frame_rate_den *
                               1000000 / videomaster_context->video_frame_rate_num;

        if (videomaster_context->return_video_next)
        {
            videomaster_context->return_video_next = false;
            if (av_new_packet(pkt, videomaster_context->black_video_buffer_size) < 0)
                return AVERROR(ENOMEM);
            memcpy(pkt->data, videomaster_context->black_video_buffer,
                   videomaster_context->black_video_buffer_size);
            pkt->stream_index = videomaster_context->video_stream->index;
            videomaster_context->pts += frame_dur_us;
            pkt->pts = videomaster_context->pts;
            pkt->dts = pkt->pts;
            pkt->duration = 1;
        }
        else
        {
            videomaster_context->return_video_next = true;
            if (videomaster_context->has_audio &&
                videomaster_context->silent_audio_buffer)
            {
                if (av_new_packet(pkt, videomaster_context->silent_audio_buffer_size) < 0)
                    return AVERROR(ENOMEM);
                memcpy(pkt->data, videomaster_context->silent_audio_buffer,
                       videomaster_context->silent_audio_buffer_size);
                pkt->stream_index = videomaster_context->audio_stream->index;
                pkt->pts = videomaster_context->pts + 1;
                pkt->dts = pkt->pts;
                pkt->duration = 1;

                /* liveedit: keep the IPC pipe fed with silence while
                 * signal is lost so the downstream consumer doesn't
                 * stall or desync. */
                if (videomaster_context->audio_pipe)
                    ff_videomaster_audio_pipe_push(
                        videomaster_context->audio_pipe,
                        videomaster_context->silent_audio_buffer,
                        videomaster_context->silent_audio_buffer_size);
            }
            /* Pace ourselves at the video frame rate */
            av_usleep(frame_dur_us);

            /* Try to re-acquire signal */
            if (ff_videomaster_is_channel_locked(videomaster_context))
            {
                av_log(avctx, AV_LOG_INFO, "Signal detected, switching to live capture\n");
                if (ff_videomaster_get_video_stream_properties(
                        avctx, videomaster_context->board_handle,
                        videomaster_context->stream_handle,
                        videomaster_context->channel_index,
                        &videomaster_context->channel_type,
                        &videomaster_context->video_info,
                        &videomaster_context->video_width,
                        &videomaster_context->video_height,
                        &videomaster_context->video_frame_rate_num,
                        &videomaster_context->video_frame_rate_den,
                        &videomaster_context->video_interlaced) == 0 &&
                    ff_videomaster_open_stream_handle(videomaster_context) == 0 &&
                    ff_videomaster_start_stream(videomaster_context) == 0)
                {
                    videomaster_context->generating_black = false;
                }
            }
        }
        return 0;
    }

    /* ── Normal capture path ── */
    if (videomaster_context->return_video_next)
    {
        int get_data_result;
        videomaster_context->return_video_next = false;

        get_data_result = ff_videomaster_get_data(videomaster_context);
        if (get_data_result == AVERROR(EAGAIN) &&
            videomaster_context->signal_no_stop &&
            videomaster_context->black_video_buffer)
        {
            /* Signal lost during capture — switch to black frames */
            if (!videomaster_context->generating_black)
                av_log(avctx, AV_LOG_WARNING, "Signal lost, generating black frames\n");
            videomaster_context->generating_black = true;
            videomaster_context->return_video_next = true;
            return ff_videomaster_read_packet(avctx, pkt);
        }
        else if (get_data_result != 0)
        {
            av_log(avctx, AV_LOG_ERROR, "Failed to get data buffers\n");
            return AVERROR(EIO);
        }

        /* liveedit: push audio to the IPC pipe once per frame (on the
         * video half-call, before the audio packet is produced). The
         * audio_buffer remains valid until the matching audio call
         * triggers ff_videomaster_release_data. */
        if (videomaster_context->audio_pipe &&
            videomaster_context->audio_buffer &&
            videomaster_context->audio_buffer_size > 0)
        {
            ff_videomaster_audio_pipe_push(
                videomaster_context->audio_pipe,
                videomaster_context->audio_buffer,
                videomaster_context->audio_buffer_size);
        }

        if (videomaster_context->has_video)
        {
            if (av_new_packet(pkt, videomaster_context->video_buffer_size) < 0)
            {
                av_log(avctx, AV_LOG_ERROR,
                       "Failed to allocate AVPacket for Video\n");
                return AVERROR(ENOMEM);
            }
            else
            {
                memcpy(pkt->data, videomaster_context->video_buffer,
                       videomaster_context->video_buffer_size);
                pkt->stream_index = videomaster_context->video_stream->index;
                /* In disjoined mode, timestamp was already extracted inside
                 * ff_videomaster_get_data before the slot was unlocked. */
                if (!videomaster_context->disjoined_streams)
                    ff_videomaster_get_timestamp(videomaster_context,
                                                 &videomaster_context->pts);
                pkt->pts = videomaster_context->pts;
                pkt->dts = pkt->pts;
                pkt->duration = 1;

                attach_timecode_to_packet(avctx, videomaster_context, pkt);
            }

            if (videomaster_context->frames_received == 0)
            {
                int64_t now = av_gettime();
                av_log(avctx, AV_LOG_INFO, "First frame wallclock : %lld\n", now);
            }

            if (ff_videomaster_get_slots_counter(videomaster_context) != 0)
            {
                av_log(avctx, AV_LOG_ERROR, "Failed to get slots counter\n");
                return AVERROR(EIO);
            }
            else
            {
                av_log(avctx, AV_LOG_TRACE, "%u frames received (%u dropped)\n",
                       videomaster_context->frames_received,
                       videomaster_context->frames_dropped);
            }
        }
    }
    else
    {
        videomaster_context->return_video_next = true;
        if (videomaster_context->has_audio)
        {
            if (av_new_packet(pkt, videomaster_context->audio_buffer_size) < 0)
            {
                av_log(avctx, AV_LOG_ERROR,
                       "Failed to allocate AVPacket for Audio\n");
                return AVERROR(ENOMEM);
            }
            else
            {
                memcpy(pkt->data, videomaster_context->audio_buffer,
                       videomaster_context->audio_buffer_size);
                pkt->stream_index = videomaster_context->audio_stream->index;
                pkt->pts = videomaster_context->pts + 1;
                pkt->dts = pkt->pts;
                pkt->duration = 1;
            }
            videomaster_context->audio_frames_received +=
                videomaster_context->audio_buffer_size;

            av_log(avctx, AV_LOG_TRACE, "%u audio frames received\n",
                   videomaster_context->audio_frames_received);
        }
        if (ff_videomaster_release_data(videomaster_context) != 0)
        {
            av_log(avctx, AV_LOG_ERROR, "Failed to release data\n");
            return AVERROR(EIO);
        }
    }

    return 0;
}

static const AVOption options[] = {
    { "list_formats",
      "List all supported VideoMaster video modes and exit.",
      OFFSET(list_formats),
      AV_OPT_TYPE_BOOL,
      { .i64 = 0 },
      0,
      1,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      NULL },
    { "board_index",
      "Index of the board to use. Only required when the ffmpeg input is set "
      "to dummy (-i dummy). If the input is a source name (from `ffmpeg "
      "-sources videomaster`), the board index is automatically deduced from "
      "the source name.",
      OFFSET(board_index),
      AV_OPT_TYPE_INT64,
      { .i64 = -1 },
      -1,
      INT_MAX,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { "channel_index",
      "Index of the stream to use. Only required when the ffmpeg input is set "
      "to dummy (-i dummy). If the input is a source name (from `ffmpeg "
      "-sources videomaster`), the stream index is automatically deduced from "
      "the source name.",
      OFFSET(channel_index),
      AV_OPT_TYPE_INT64,
      { .i64 = -1 },
      -1,
      INT_MAX,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { "timestamp_source",
      "Selects the source for video frame timestamps. Options are: 'hw' for "
      "hardware-based timestamps (highest precision, if supported), 'osc' for "
      "the device's internal oscillator, or 'system' for the system clock. Use "
      "'hw' for best synchronization accuracy, 'osc' for stable internal "
      "timing, or 'system' for general-purpose timing. Default is 'osc'.",
      OFFSET(timestamp_source),
      AV_OPT_TYPE_INT64,
      { .i64 = AV_VIDEOMASTER_TIMESTAMP_OSCILLATOR },
      AV_VIDEOMASTER_TIMESTAMP_OSCILLATOR,
      AV_VIDEOMASTER_TIMESTAMP_NB - 1,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "timestamp_source" },
    { "osc",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_TIMESTAMP_OSCILLATOR },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "timestamp_source" },
    { "system",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_TIMESTAMP_SYSTEM },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "timestamp_source" },
    { "hw",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_TIMESTAMP_HARDWARE },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "timestamp_source" },
    { "ltc_on_board",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_TIMESTAMP_LTC_ON_BOARD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "timestamp_source" },
    { "ltc_companion_card",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_TIMESTAMP_LTC_COMPANION_CARD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "timestamp_source" },
    { "nb_channels",
      "Number of audio channels to use. This option is only used when the "
      "input source is an SDI stream. "
      "If the input source is an HDMI stream, the number of channels is "
      "automatically deduced from the stream properties.",
      OFFSET(nb_channels),
      AV_OPT_TYPE_INT64,
      { .i64 = -1 },
      -1,
      INT_MAX,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    {
        "sample_rate",
        "Audio sample rate to use. This option is only used when the input "
        "source is an SDI stream. "
        "If the input source is an HDMI stream, the sample rate is "
        "automatically deduced from the stream properties.",
        OFFSET(sample_rate),
        AV_OPT_TYPE_INT64,
        { .i64 = AV_VIDEOMASTER_SAMPLE_RATE_UNKNOWN },
        AV_VIDEOMASTER_SAMPLE_RATE_UNKNOWN,
        AV_VIDEOMASTER_SAMPLE_RATE_48000,
        AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
        .unit = "sample_rate_value",
    },
    { "48000",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_SAMPLE_RATE_48000 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "sample_rate_value" },
    { "44100",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_SAMPLE_RATE_44100 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "sample_rate_value" },
    { "32000",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_SAMPLE_RATE_32000 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "sample_rate_value" },
    {
        "sample_size",
        "Audio sample size to use. This option is only used when the input "
        "source is an SDI stream. "
        "If the input source is an HDMI stream, the sample size is "
        "automatically deduced from the stream properties."
        "Options are: 16 or 24 bits.",
        OFFSET(sample_size),
        AV_OPT_TYPE_INT64,
        { .i64 = AV_VIDEOMASTER_SAMPLE_SIZE_UNKNOWN },
        AV_VIDEOMASTER_SAMPLE_SIZE_UNKNOWN,
        AV_VIDEOMASTER_SAMPLE_SIZE_24,
        AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
        .unit = "sample_size_value",
    },
    { "16",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_SAMPLE_SIZE_16 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "sample_size_value" },
    { "24",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_SAMPLE_SIZE_24 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
      .unit = "sample_size_value" },
    {
        "buffer_packing",
        "Specifies the buffer packing format and whether to enable the FPGA's "
        "color space converter on the board. If not set, the default buffer "
        "packing is YUV422 10-bit when the Line Padding  property can be "
        "enabled, or YUV422 8-bit otherwise.",
        OFFSET(buffer_packing),
        AV_OPT_TYPE_INT64,
        { .i64 = AV_NB_VIDEOMASTER_BUFFER_PACKINGS },
        0,
        AV_NB_VIDEOMASTER_BUFFER_PACKINGS,
        AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
        .unit = "buffer_packing_value",
    },
    { "YUV422_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUV422_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUVK4224_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUVK4224_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUV422_10",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUV422_10 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUVK4224_10",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUVK4224_10 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUV4444_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUV4444_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUVK4444_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUVK4444_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUV444_10",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUV444_10 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUVK4444_10",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUVK4444_10 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "RGB_32",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_RGB_32 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "RGBA_32",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_RGBA_32 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "RGB_24",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_RGB_24 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YVU420_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YVU420_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YUV420_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YUV420_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YVU420_10_MSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YVU420_10_MSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YVU420_10_LSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YVU420_10_LSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YUV420_10_MSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YUV420_10_MSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YUV420_10_LSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YUV420_10_LSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "RGB_64",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_RGB_64 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUV422_16",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUV422_16 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUV444_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUV444_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "ICTCP_422_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_ICTCP_422_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "ICTCP_422_10",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_ICTCP_422_10 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YUV422_10_LSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YUV422_10_LSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YUV422_10_MSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YUV422_10_MSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YVU422_10_LSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YVU422_10_LSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YVU422_10_MSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YVU422_10_MSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YUV422_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YUV422_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YVU422_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_YVU422_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_YUV422_10_NOPAD_BIGEND",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUV422_10_NOPAD_BIGEND },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_PALETTE_RGBA_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PALETTE_RGBA_8 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_NV12",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_NV12 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "PLANAR_RGB444_10_LSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_PLANAR_RGB444_10_LSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "RGBA4444_10_LSB_PAD",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_RGBA4444_10_LSB_PAD },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "RGBA4444_16",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_RGBA4444_16 },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "auto_set_ltc_input",
      "Automatically configure the board REF_IN inputs when using "
      "ltc_on_board timestamp source. When enabled and timestamp_source is "
      "set to ltc_on_board, disables REF_IN0 detection and, if the board has "
      "two REF_IN connectors, enables REF_IN1 detection and sets genlock "
      "source to REF_IN1.",
      OFFSET(auto_set_ltc_input),
      AV_OPT_TYPE_BOOL,
      { .i64 = 0 },
      0,
      1,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { "wait_for_input",
      "Wait for user to press 'r' key before starting capture. "
      "Frames are dropped until the key is received on stdin.",
      OFFSET(wait_for_input),
      AV_OPT_TYPE_BOOL,
      { .i64 = 0 },
      0,
      1,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { "wait_for_tc",
      "Wait for a stable locked LTC timecode before starting capture. "
      "When combined with wait_for_input, waits for 'r' first, then for TC.",
      OFFSET(wait_for_tc),
      AV_OPT_TYPE_BOOL,
      { .i64 = 0 },
      0,
      1,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { "no_autodetect_timeout",
      "Do not exit on autodetect after 3sec. Wait indefinitely for "
      "a signal to be detected on the input channel.",
      OFFSET(no_autodetect_timeout),
      AV_OPT_TYPE_BOOL,
      { .i64 = 0 },
      0,
      1,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { "signal_no_stop",
      "Do not stop when input signal is lost or absent. Generate black "
      "video frames and silent audio instead. Requires default_video_format "
      "when starting without signal.",
      OFFSET(signal_no_stop),
      AV_OPT_TYPE_BOOL,
      { .i64 = 0 },
      0,
      1,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { "default_video_mode",
      "Default VideoMaster video standard index (VHD_VIDEOSTANDARD) when "
      "no signal is detected. Use -1 for none. "
      "Required with signal_no_stop when no signal is present at startup.",
      OFFSET(default_video_mode),
      AV_OPT_TYPE_INT64,
      { .i64 = -1 },
      -1,
      INT_MAX,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM,
      NULL },
    { "disjoined_streams",
      "Open separate disjoined video and ANC streams instead of a single "
      "joined stream. Slot timestamps are compared to ensure video and ANC "
      "buffers are temporally synchronized. SDI only.",
      OFFSET(disjoined_streams),
      AV_OPT_TYPE_BOOL,
      { .i64 = 0 },
      0,
      1,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_VIDEO_PARAM |
          AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { "audio_pipe",
      "Windows named pipe path (e.g. \\\\.\\pipe\\liveedit_audio) where raw "
      "interleaved PCM audio is streamed continuously, independently of "
      "wait_for_input and wait_for_tc. A second ffmpeg process can consume "
      "it with '-f s16le|s24le -ar <rate> -ac <channels> -i <pipe>'. "
      "When unset no pipe is created.",
      OFFSET(audio_pipe),
      AV_OPT_TYPE_STRING,
      { .str = NULL },
      0,
      0,
      AV_OPT_FLAG_DECODING_PARAM | DEC | AV_OPT_FLAG_AUDIO_PARAM,
      NULL },
    { NULL },
};

static const AVClass videomaster_demuxer_class = {
    .class_name = "DELTACAST Videomaster indev",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT,
};

const FFInputFormat ff_videomaster_demuxer = {
    .p.name = "videomaster",
    .p.long_name = NULL_IF_CONFIG_SMALL("DELTACAST Videomaster input"),
    .p.flags = AVFMT_NOFILE,
    .p.priv_class = &videomaster_demuxer_class,
    .priv_data_size = sizeof(struct VideoMasterData),
    .get_device_list = ff_videomaster_list_input_devices,
    .read_header = ff_videomaster_read_header,
    .read_packet = ff_videomaster_read_packet,
    .read_close = ff_videomaster_read_close,
};