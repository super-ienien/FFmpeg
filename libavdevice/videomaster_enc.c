#include "videomaster_enc.h"

#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavformat/mux.h"
#include "libavutil/avstring.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "videomaster_common.h"

#if defined(__APPLE__)
#include <VideoMasterHD/VideoMasterHD_Core.h>
#include <VideoMasterHD/VideoMasterHD_Keyer.h>
#include <VideoMasterHD/VideoMasterHD_Sdi.h>
#include <VideoMasterHD/VideoMasterHD_Sdi_Audio.h>
#else
#include <VideoMasterHD_Core.h>
#include <VideoMasterHD_Keyer.h>
#include <VideoMasterHD_Sdi.h>
#include <VideoMasterHD_Sdi_Audio.h>
#endif

#define OFFSET(x) offsetof(struct VideoMasterEncData, x)
#define ENC       AV_OPT_FLAG_ENCODING_PARAM

/**
 * @brief Video mode table entry, matching Papillon Of Light's table.
 */
typedef struct VideoMasterMode
{
    uint32_t      video_standard;  ///< VHD_VIDEOSTANDARD
    uint32_t      clock_divisor;   ///< VHD_CLOCKDIVISOR
    uint32_t      interface_type;  ///< VHD_INTERFACE
    uint32_t      width;
    uint32_t      height;
    uint32_t      fps_num;
    uint32_t      fps_den;
    bool          interlaced;
    const char   *name;
} VideoMasterMode;

/**
 * @brief Complete video mode table (55 modes, 1-based index).
 * Identical to Papillon Of Light's getVideoStandardTable().
 */
static const VideoMasterMode video_mode_table[] = {
    //  1: NTSC 487i30
    { VHD_VIDEOSTD_S259M_NTSC_487, VHD_CLOCKDIV_1, VHD_INTERFACE_SD_259,
      720, 487, 30, 1, true, "NTSC 487i30" },
    //  2: NTSC 487i29.97
    { VHD_VIDEOSTD_S259M_NTSC_487, VHD_CLOCKDIV_1001, VHD_INTERFACE_SD_259,
      720, 487, 30000, 1001, true, "NTSC 487i29.97" },
    //  3: NTSC 480i30
    { VHD_VIDEOSTD_S259M_NTSC_480, VHD_CLOCKDIV_1, VHD_INTERFACE_SD_259,
      720, 480, 30, 1, true, "NTSC 480i30" },
    //  4: NTSC 480i29.97
    { VHD_VIDEOSTD_S259M_NTSC_480, VHD_CLOCKDIV_1001, VHD_INTERFACE_SD_259,
      720, 480, 30000, 1001, true, "NTSC 480i29.97" },
    //  5: PAL 576i25
    { VHD_VIDEOSTD_S259M_PAL, VHD_CLOCKDIV_1, VHD_INTERFACE_SD_259,
      720, 576, 25, 1, true, "PAL 576i25" },
    //  6: 720p24
    { VHD_VIDEOSTD_S296M_720p_24Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1280, 720, 24, 1, false, "720p24" },
    //  7: 720p23.98
    { VHD_VIDEOSTD_S296M_720p_24Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      1280, 720, 24000, 1001, false, "720p23.98" },
    //  8: 720p25
    { VHD_VIDEOSTD_S296M_720p_25Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1280, 720, 25, 1, false, "720p25" },
    //  9: 720p30
    { VHD_VIDEOSTD_S296M_720p_30Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1280, 720, 30, 1, false, "720p30" },
    // 10: 720p29.97
    { VHD_VIDEOSTD_S296M_720p_30Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      1280, 720, 30000, 1001, false, "720p29.97" },
    // 11: 720p50
    { VHD_VIDEOSTD_S296M_720p_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1280, 720, 50, 1, false, "720p50" },
    // 12: 720p60
    { VHD_VIDEOSTD_S296M_720p_60Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1280, 720, 60, 1, false, "720p60" },
    // 13: 720p59.94
    { VHD_VIDEOSTD_S296M_720p_60Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      1280, 720, 60000, 1001, false, "720p59.94" },
    // 14: 1080i50
    { VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1920, 1080, 25, 1, true, "1080i50" },
    // 15: 1080i60
    { VHD_VIDEOSTD_S274M_1080i_60Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1920, 1080, 30, 1, true, "1080i60" },
    // 16: 1080i59.94
    { VHD_VIDEOSTD_S274M_1080i_60Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      1920, 1080, 30000, 1001, true, "1080i59.94" },
    // 17: 1080p24
    { VHD_VIDEOSTD_S274M_1080p_24Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1920, 1080, 24, 1, false, "1080p24" },
    // 18: 1080p23.98
    { VHD_VIDEOSTD_S274M_1080p_24Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      1920, 1080, 24000, 1001, false, "1080p23.98" },
    // 19: 1080p25
    { VHD_VIDEOSTD_S274M_1080p_25Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1920, 1080, 25, 1, false, "1080p25" },
    // 20: 1080p30
    { VHD_VIDEOSTD_S274M_1080p_30Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1920, 1080, 30, 1, false, "1080p30" },
    // 21: 1080p29.97
    { VHD_VIDEOSTD_S274M_1080p_30Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      1920, 1080, 30000, 1001, false, "1080p29.97" },
    // 22: 1080p50
    { VHD_VIDEOSTD_S274M_1080p_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_A_425_1,
      1920, 1080, 50, 1, false, "1080p50" },
    // 23: 1080p60
    { VHD_VIDEOSTD_S274M_1080p_60Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_A_425_1,
      1920, 1080, 60, 1, false, "1080p60" },
    // 24: 1080p59.94
    { VHD_VIDEOSTD_S274M_1080p_60Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_3G_A_425_1,
      1920, 1080, 60000, 1001, false, "1080p59.94" },
    // 25: 1080psf24
    { VHD_VIDEOSTD_S274M_1080psf_24Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1920, 1080, 24, 1, false, "1080psf24" },
    // 26: 1080psf23.98
    { VHD_VIDEOSTD_S274M_1080psf_24Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      1920, 1080, 24000, 1001, false, "1080psf23.98" },
    // 27: 1080psf25
    { VHD_VIDEOSTD_S274M_1080psf_25Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1920, 1080, 25, 1, false, "1080psf25" },
    // 28: 1080psf30
    { VHD_VIDEOSTD_S274M_1080psf_30Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      1920, 1080, 30, 1, false, "1080psf30" },
    // 29: 1080psf29.97
    { VHD_VIDEOSTD_S274M_1080psf_30Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      1920, 1080, 30000, 1001, false, "1080psf29.97" },
    // 30: 2048p24
    { VHD_VIDEOSTD_S2048M_2048p_24Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      2048, 1080, 24, 1, false, "2048p24" },
    // 31: 2048p23.98
    { VHD_VIDEOSTD_S2048M_2048p_24Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      2048, 1080, 24000, 1001, false, "2048p23.98" },
    // 32: 2048p25
    { VHD_VIDEOSTD_S2048M_2048p_25Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      2048, 1080, 25, 1, false, "2048p25" },
    // 33: 2048p30
    { VHD_VIDEOSTD_S2048M_2048p_30Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      2048, 1080, 30, 1, false, "2048p30" },
    // 34: 2048p29.97
    { VHD_VIDEOSTD_S2048M_2048p_30Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      2048, 1080, 30000, 1001, false, "2048p29.97" },
    // 35: 2048p48
    { VHD_VIDEOSTD_S2048M_2048p_48Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_A_425_1,
      2048, 1080, 48, 1, false, "2048p48" },
    // 36: 2048p47.95
    { VHD_VIDEOSTD_S2048M_2048p_48Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_3G_A_425_1,
      2048, 1080, 48000, 1001, false, "2048p47.95" },
    // 37: 2048p50
    { VHD_VIDEOSTD_S2048M_2048p_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_A_425_1,
      2048, 1080, 50, 1, false, "2048p50" },
    // 38: 2048p60
    { VHD_VIDEOSTD_S2048M_2048p_60Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_A_425_1,
      2048, 1080, 60, 1, false, "2048p60" },
    // 39: 2048p59.94
    { VHD_VIDEOSTD_S2048M_2048p_60Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_3G_A_425_1,
      2048, 1080, 60000, 1001, false, "2048p59.94" },
    // 40: 2048psf24
    { VHD_VIDEOSTD_S2048M_2048psf_24Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      2048, 1080, 24, 1, false, "2048psf24" },
    // 41: 2048psf23.98
    { VHD_VIDEOSTD_S2048M_2048psf_24Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      2048, 1080, 24000, 1001, false, "2048psf23.98" },
    // 42: 2048psf25
    { VHD_VIDEOSTD_S2048M_2048psf_25Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      2048, 1080, 25, 1, false, "2048psf25" },
    // 43: 2048psf30
    { VHD_VIDEOSTD_S2048M_2048psf_30Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_HD_292_1,
      2048, 1080, 30, 1, false, "2048psf30" },
    // 44: 2048psf29.97
    { VHD_VIDEOSTD_S2048M_2048psf_30Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_HD_292_1,
      2048, 1080, 30000, 1001, false, "2048psf29.97" },
    // 45: 2160p24
    { VHD_VIDEOSTD_3840x2160p_24Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 24, 1, false, "2160p24" },
    // 46: 2160p23.98
    { VHD_VIDEOSTD_3840x2160p_24Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 24000, 1001, false, "2160p23.98" },
    // 47: 2160p25
    { VHD_VIDEOSTD_3840x2160p_25Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 25, 1, false, "2160p25" },
    // 48: 2160p30
    { VHD_VIDEOSTD_3840x2160p_30Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 30, 1, false, "2160p30" },
    // 49: 2160p29.97
    { VHD_VIDEOSTD_3840x2160p_30Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 30000, 1001, false, "2160p29.97" },
    // 50: 2160p50
    { VHD_VIDEOSTD_3840x2160p_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 50, 1, false, "2160p50" },
    // 51: 2160psf24
    { VHD_VIDEOSTD_3840x2160psf_24Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 24, 1, false, "2160psf24" },
    // 52: 2160psf23.98
    { VHD_VIDEOSTD_3840x2160psf_24Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 24000, 1001, false, "2160psf23.98" },
    // 53: 2160psf25
    { VHD_VIDEOSTD_3840x2160psf_25Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 25, 1, false, "2160psf25" },
    // 54: 2160psf30
    { VHD_VIDEOSTD_3840x2160psf_30Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 30, 1, false, "2160psf30" },
    // 55: 2160psf29.97
    { VHD_VIDEOSTD_3840x2160psf_30Hz, VHD_CLOCKDIV_1001, VHD_INTERFACE_12G_2082_10,
      3840, 2160, 30000, 1001, false, "2160psf29.97" },
};

#define VIDEO_MODE_TABLE_SIZE \
    (sizeof(video_mode_table) / sizeof(video_mode_table[0]))

/**
 * @brief Extended data structure for the VideoMaster TX encoder.
 */
typedef struct VideoMasterEncData
{
    AVClass *av_class;

    void *context;  ///< DELTACAST board context (VideoMasterContext*)

    /* Command Options */
    int64_t board_index;
    int64_t channel_index;
    int64_t mode;            ///< 1-based video mode index
    int64_t buffer_packing;  ///< buffer packing override
    int64_t queue_depth;
    int64_t keying_mode;
    int64_t set_bidir_outputs;
    int64_t preroll;
    int64_t rx_channel;      ///< RX channel for internal keying
    int64_t nb_channels;     ///< audio channel count
    int64_t sample_rate;     ///< audio sample rate
    int64_t sample_size;     ///< audio sample size in bits
} VideoMasterEncData;

/** Static helper declarations */

static uint32_t get_dual_interface(uint32_t interface_type);
static void     fill_black_buffer(uint8_t *buffer, uint32_t size,
                                  enum AVVideoMasterBufferPacking packing);
static uint32_t get_keyer_input(uint8_t channel);
static uint32_t get_keyer_fill_output(uint8_t channel);
static uint32_t get_keyer_key_output(uint8_t channel);
static uint32_t get_keyer_video_output_property(uint8_t tx_index);
static uint32_t get_genlock_source(uint8_t rx_channel);

/** Static helper implementations */

static uint32_t get_dual_interface(uint32_t interface_type)
{
    switch (interface_type)
    {
    case VHD_INTERFACE_HD_292_1:
        return VHD_INTERFACE_HD_DUAL;
    case VHD_INTERFACE_3G_A_425_1:
        return VHD_INTERFACE_3G_A_DUAL;
    case VHD_INTERFACE_12G_2082_10:
        return VHD_INTERFACE_12G_2082_10_DUAL;
    case VHD_INTERFACE_SD_259:
        return VHD_INTERFACE_SD_DUAL;
    default:
        return interface_type;
    }
}

static void fill_black_buffer(uint8_t *buffer, uint32_t size,
                               enum AVVideoMasterBufferPacking packing)
{
    if (packing == AV_VIDEOMASTER_BUFFER_PACKING_YUVK4224_8)
    {
        for (uint32_t i = 0; i + 5 < size; i += 6)
        {
            buffer[i]     = 0x80;  // U
            buffer[i + 1] = 0x10;  // Y0
            buffer[i + 2] = 0x80;  // V
            buffer[i + 3] = 0x00;  // K0
            buffer[i + 4] = 0x10;  // Y1
            buffer[i + 5] = 0x00;  // K1
        }
    }
    else if (packing == AV_VIDEOMASTER_BUFFER_PACKING_YUV422_8)
    {
        for (uint32_t i = 0; i + 3 < size; i += 4)
        {
            buffer[i]     = 0x80;  // U
            buffer[i + 1] = 0x10;  // Y0
            buffer[i + 2] = 0x80;  // V
            buffer[i + 3] = 0x10;  // Y1
        }
    }
    else
    {
        memset(buffer, 0, size);  // RGBA/RGB black
    }
}

static uint32_t get_keyer_input(uint8_t channel)
{
    switch (channel)
    {
    case 0:  return VHD_KINPUT_TX0;
    case 1:  return VHD_KINPUT_TX1;
    case 2:  return VHD_KINPUT_TX2;
    case 3:  return VHD_KINPUT_TX3;
    default: return 0;
    }
}

static uint32_t get_keyer_rx_input(uint8_t channel)
{
    switch (channel)
    {
    case 0:  return VHD_KINPUT_RX0;
    case 1:  return VHD_KINPUT_RX1;
    case 2:  return VHD_KINPUT_RX2;
    case 3:  return VHD_KINPUT_RX3;
    default: return 0;
    }
}

static uint32_t get_keyer_fill_output(uint8_t channel)
{
    switch (channel)
    {
    case 0:  return VHD_KOUTPUT_TX0_FILL;
    case 1:  return VHD_KOUTPUT_TX1_FILL;
    case 2:  return VHD_KOUTPUT_TX2_FILL;
    case 3:  return VHD_KOUTPUT_TX3_FILL;
    default: return 0;
    }
}

static uint32_t get_keyer_key_output(uint8_t channel)
{
    switch (channel)
    {
    case 0:  return VHD_KOUTPUT_TX0_KEY;
    case 2:  return VHD_KOUTPUT_TX2_KEY;
    default: return 0;
    }
}

static uint32_t get_keyer_video_output_property(uint8_t tx_index)
{
    switch (tx_index)
    {
    case 0:  return VHD_KEYER_BP_VIDEOOUTPUT_TX0;
    case 1:  return VHD_KEYER_BP_VIDEOOUTPUT_TX1;
    case 2:  return VHD_KEYER_BP_VIDEOOUTPUT_TX_2;
    case 3:  return VHD_KEYER_BP_VIDEOOUTPUT_TX_3;
    default: return 0;
    }
}

static uint32_t get_genlock_source(uint8_t rx_channel)
{
    switch (rx_channel)
    {
    case 0:  return VHD_GENLOCK_RX0;
    case 1:  return VHD_GENLOCK_RX1;
    case 2:  return VHD_GENLOCK_RX2;
    case 3:  return VHD_GENLOCK_RX3;
    case 4:  return VHD_GENLOCK_RX4;
    case 5:  return VHD_GENLOCK_RX5;
    case 6:  return VHD_GENLOCK_RX6;
    case 7:  return VHD_GENLOCK_RX7;
    case 8:  return VHD_GENLOCK_RX8;
    case 9:  return VHD_GENLOCK_RX9;
    case 10: return VHD_GENLOCK_RX10;
    case 11: return VHD_GENLOCK_RX11;
    default: return 0;
    }
}

/** Public function implementations */

int ff_videomaster_list_output_devices(AVFormatContext         *avctx,
                                       struct AVDeviceInfoList *device_list)
{
    VideoMasterContext *videomaster_context = NULL;
    int                 av_error = 0;

    videomaster_context = av_mallocz(sizeof(VideoMasterContext));
    if (!videomaster_context)
        return AVERROR(ENOMEM);

    videomaster_context->avctx = avctx;

    av_error = ff_videomaster_get_api_info(videomaster_context);
    if (av_error != 0)
    {
        av_freep(&videomaster_context);
        return av_error;
    }

    for (uint32_t board = 0; board < videomaster_context->number_of_boards;
         board++)
    {
        videomaster_context->board_index = board;
        av_error = ff_videomaster_open_board_handle(videomaster_context);
        if (av_error != 0)
            continue;

        av_error = ff_videomaster_get_nb_tx_channels(videomaster_context);
        if (av_error != 0)
        {
            ff_videomaster_close_board_handle(videomaster_context);
            continue;
        }

        av_log(avctx, AV_LOG_INFO,
               "Board %d has %d TX channels\n", board,
               videomaster_context->nb_tx_channels);

        ff_videomaster_close_board_handle(videomaster_context);
    }

    av_freep(&videomaster_context);
    return 0;
}

int ff_videomaster_write_header(AVFormatContext *avctx)
{
    VideoMasterEncData *enc_data =
        (VideoMasterEncData *)avctx->priv_data;
    VideoMasterContext *ctx = NULL;
    int                 av_error = 0;
    const VideoMasterMode *mode = NULL;

    /* Allocate context */
    ctx = av_mallocz(sizeof(VideoMasterContext));
    if (!ctx)
        return AVERROR(ENOMEM);

    ctx->avctx = avctx;
    enc_data->context = ctx;

    /* Validate mode index (1-based) */
    if (enc_data->mode < 1 || enc_data->mode > (int64_t)VIDEO_MODE_TABLE_SIZE)
    {
        av_log(avctx, AV_LOG_ERROR,
               "Invalid mode index %lld (must be 1-%d)\n",
               (long long)enc_data->mode, (int)VIDEO_MODE_TABLE_SIZE);
        return AVERROR(EINVAL);
    }

    mode = &video_mode_table[enc_data->mode - 1];

    /* Copy parameters to context */
    ctx->board_index = (uint32_t)enc_data->board_index;
    ctx->channel_index = (uint32_t)enc_data->channel_index;
    ctx->channel_type = AV_VIDEOMASTER_CHANNEL_SDI;
    ctx->video_info.sdi.video_standard = mode->video_standard;
    ctx->video_info.sdi.clock_divisor = mode->clock_divisor;
    ctx->video_info.sdi.interface = mode->interface_type;
    ctx->video_width = mode->width;
    ctx->video_height = mode->height;
    ctx->video_frame_rate_num = mode->fps_num;
    ctx->video_frame_rate_den = mode->fps_den;
    ctx->video_interlaced = mode->interlaced;
    ctx->video_buffer_packing =
        (enc_data->buffer_packing != AV_NB_VIDEOMASTER_BUFFER_PACKINGS)
            ? (enum AVVideoMasterBufferPacking)enc_data->buffer_packing
            : AV_VIDEOMASTER_BUFFER_PACKING_YUV422_8;
    ctx->queue_depth = (uint32_t)enc_data->queue_depth;
    ctx->preroll_count = (enc_data->preroll >= 0)
                             ? (uint32_t)enc_data->preroll
                             : (ctx->queue_depth > 1 ? ctx->queue_depth - 1
                                                     : 1);
    ctx->keying_mode = (int)enc_data->keying_mode;
    ctx->has_video = true;
    ctx->has_audio = (enc_data->nb_channels > 0);
    ctx->audio_nb_channels = (uint32_t)enc_data->nb_channels;
    ctx->audio_sample_rate = (uint32_t)enc_data->sample_rate;
    ctx->audio_sample_size = (uint32_t)enc_data->sample_size;

    av_log(avctx, AV_LOG_INFO,
           "VideoMaster TX: board %d, channel %d, mode %lld (%s), "
           "%dx%d%s @ %d/%d fps\n",
           ctx->board_index, ctx->channel_index,
           (long long)enc_data->mode, mode->name,
           mode->width, mode->height,
           mode->interlaced ? "i" : "p",
           mode->fps_num, mode->fps_den);

    /* Get API info */
    av_error = ff_videomaster_get_api_info(ctx);
    if (av_error != 0)
        return av_error;

    /* BiDir configuration */
    if (enc_data->set_bidir_outputs >= 0)
    {
        HANDLE tmp_handle = NULL;

        if (ff_videomaster_handle_vhd_status(
                avctx,
                VHD_OpenBoardHandle(ctx->board_index, &tmp_handle, NULL, 0),
                "Temp board handle opened for BiDir",
                "Failed to open temp board handle for BiDir") == 0)
        {
            VHD_CloseBoardHandle(tmp_handle);

            VHD_ERRORCODE bidir_result =
                VHD_SetBiDirCfg(ctx->board_index,
                                (uint32_t)enc_data->set_bidir_outputs);
            if (bidir_result == VHDERR_NOERROR)
                av_log(avctx, AV_LOG_INFO,
                       "BiDir config set to %lld TX channels\n",
                       (long long)enc_data->set_bidir_outputs);
            else
                av_log(avctx, AV_LOG_WARNING,
                       "VHD_SetBiDirCfg(%lld) failed, board may not support "
                       "BiDir reconfiguration\n",
                       (long long)enc_data->set_bidir_outputs);
        }
    }

    /* Open board handle */
    av_error = ff_videomaster_open_board_handle(ctx);
    if (av_error != 0)
        return av_error;

    /* Detect onboard keyer */
    ctx->has_keyer = false;
    if (ctx->keying_mode != AV_VIDEOMASTER_KEYING_NONE)
    {
        uint32_t nb_keyers = 0;
        if (VHD_GetBoardCapability(ctx->board_handle,
                                    VHD_KEYER_BOARD_CAP_KEYER,
                                    &nb_keyers) == VHDERR_NOERROR &&
            nb_keyers > 0)
        {
            ctx->has_keyer = true;
        }

        if (ctx->keying_mode == AV_VIDEOMASTER_KEYING_INTERNAL &&
            !ctx->has_keyer)
        {
            av_log(avctx, AV_LOG_ERROR,
                   "Internal keying requires an onboard keyer\n");
            ff_videomaster_close_board_handle(ctx);
            return AVERROR(EINVAL);
        }
    }

    /* Compute use_yuvk */
    ctx->use_yuvk = (ctx->keying_mode == AV_VIDEOMASTER_KEYING_EXTERNAL &&
                     !ctx->has_keyer);

    if (ctx->use_yuvk)
    {
        ctx->video_buffer_packing = AV_VIDEOMASTER_BUFFER_PACKING_YUVK4224_8;
        av_log(avctx, AV_LOG_INFO,
               "YUVK fill+key: TX%d (fill) + TX%d (key)\n",
               ctx->channel_index, ctx->channel_index + 1);
    }

    /* Disable loopback */
    ff_videomaster_disable_loopback_on_channel(ctx);
    if (ctx->use_yuvk)
    {
        uint32_t saved_channel = ctx->channel_index;
        ctx->channel_index = saved_channel + 1;
        ff_videomaster_disable_loopback_on_channel(ctx);
        ctx->channel_index = saved_channel;
    }

    /* Configure genlock */
    if (ctx->keying_mode == AV_VIDEOMASTER_KEYING_INTERNAL)
    {
        VHD_SetBoardProperty(ctx->board_handle, VHD_SDI_BP_GENLOCK_SOURCE,
                             get_genlock_source((uint8_t)enc_data->rx_channel));
    }
    else
    {
        VHD_SetBoardProperty(ctx->board_handle, VHD_SDI_BP_GENLOCK_CLOCK_DIV,
                             mode->clock_divisor);
    }

    /* Open TX stream handle */
    av_error = ff_videomaster_open_tx_stream_handle(ctx);
    if (av_error != 0)
    {
        ff_videomaster_close_board_handle(ctx);
        return av_error;
    }

    /* Configure stream properties */
    ff_videomaster_handle_vhd_status(
        avctx,
        VHD_SetStreamProperty(ctx->stream_handle,
                              VHD_SDI_SP_VIDEO_STANDARD,
                              mode->video_standard),
        "", "Failed to set VIDEO_STANDARD");

    if (ctx->use_yuvk)
    {
        uint32_t dual_iface = get_dual_interface(mode->interface_type);
        ff_videomaster_handle_vhd_status(
            avctx,
            VHD_SetStreamProperty(ctx->stream_handle,
                                  VHD_SDI_SP_INTERFACE, dual_iface),
            "", "Failed to set INTERFACE_DUAL");
        ff_videomaster_handle_vhd_status(
            avctx,
            VHD_SetStreamProperty(ctx->stream_handle,
                                  VHD_CORE_SP_BUFFER_PACKING,
                                  VHD_BUFPACK_VIDEO_YUVK4224_8),
            "", "Failed to set BUFFER_PACKING_YUVK");
        ff_videomaster_handle_vhd_status(
            avctx,
            VHD_SetStreamProperty(ctx->stream_handle,
                                  VHD_SDI_SP_YUVK_NO_CHROMA_ON_KEY, TRUE),
            "", "Failed to set YUVK_NO_CHROMA_ON_KEY");
    }
    else
    {
        ff_videomaster_handle_vhd_status(
            avctx,
            VHD_SetStreamProperty(ctx->stream_handle,
                                  VHD_SDI_SP_INTERFACE,
                                  mode->interface_type),
            "", "Failed to set INTERFACE");
        ff_videomaster_handle_vhd_status(
            avctx,
            VHD_SetStreamProperty(ctx->stream_handle,
                                  VHD_CORE_SP_BUFFER_PACKING,
                                  ctx->video_buffer_packing),
            "", "Failed to set BUFFER_PACKING");
    }

    ff_videomaster_handle_vhd_status(
        avctx,
        VHD_SetStreamProperty(ctx->stream_handle,
                              VHD_CORE_SP_BUFFERQUEUE_DEPTH,
                              ctx->queue_depth),
        "", "Failed to set BUFFERQUEUE_DEPTH");

    ff_videomaster_handle_vhd_status(
        avctx,
        VHD_SetStreamProperty(ctx->stream_handle,
                              VHD_CORE_SP_BUFFERQUEUE_PRELOAD,
                              ctx->preroll_count >= 1 ? ctx->preroll_count
                                                       : 1),
        "", "Failed to set BUFFERQUEUE_PRELOAD");

    ff_videomaster_handle_vhd_status(
        avctx,
        VHD_SetStreamProperty(ctx->stream_handle,
                              VHD_SDI_BP_GENLOCK_CLOCK_DIV,
                              mode->clock_divisor),
        "", "Failed to set GENLOCK_CLOCK_DIV");

    /* Configure TX genlock for internal keying */
    if (ctx->keying_mode == AV_VIDEOMASTER_KEYING_INTERNAL)
    {
        VHD_SetStreamProperty(ctx->stream_handle, VHD_SDI_SP_TX_GENLOCK,
                              TRUE);
    }

    /* Configure keyer */
    if (ctx->has_keyer &&
        ctx->keying_mode == AV_VIDEOMASTER_KEYING_EXTERNAL)
    {
        uint8_t ch = (uint8_t)ctx->channel_index;
        uint8_t key_ch = ch + 1;

        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_INPUT_B,
                             get_keyer_input(ch));
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_INPUT_K,
                             get_keyer_input(ch));
        VHD_SetBoardProperty(ctx->board_handle,
                             get_keyer_video_output_property(ch),
                             get_keyer_fill_output(ch));
        VHD_SetBoardProperty(ctx->board_handle,
                             get_keyer_video_output_property(key_ch),
                             get_keyer_key_output(ch));
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_ALPHACLIP_MIN,
                             0);
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_ALPHACLIP_MAX,
                             1020);
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_KCOMPRESSOR,
                             TRUE);
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_ENABLE, TRUE);

        av_log(avctx, AV_LOG_INFO,
               "External keying enabled with onboard keyer\n");
    }
    else if (ctx->has_keyer &&
             ctx->keying_mode == AV_VIDEOMASTER_KEYING_INTERNAL)
    {
        uint8_t ch = (uint8_t)ctx->channel_index;
        uint8_t rx_ch = (uint8_t)enc_data->rx_channel;

        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_INPUT_A,
                             get_keyer_rx_input(rx_ch));
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_INPUT_B,
                             get_keyer_input(ch));
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_INPUT_K,
                             get_keyer_input(ch));
        VHD_SetBoardProperty(ctx->board_handle,
                             get_keyer_video_output_property(ch),
                             VHD_KOUTPUT_KEYER);
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_ALPHACLIP_MIN,
                             0);
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_ALPHACLIP_MAX,
                             1020);
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_ENABLE, TRUE);

        av_log(avctx, AV_LOG_INFO,
               "Internal keying enabled (RX%d -> TX%d)\n",
               rx_ch, ch);
    }

    /* Init audio */
    if (ctx->has_audio)
    {
        av_error = ff_videomaster_init_audio_info(
            ctx, &ctx->audio_info.sdi.audio_info);
        if (av_error != 0)
        {
            av_log(avctx, AV_LOG_WARNING,
                   "Failed to init audio info, continuing without audio\n");
            ctx->has_audio = false;
        }
        else
        {
            /* Allocate ring buffer (~200ms at 48kHz stereo) */
            ctx->audio_ring_size = 48000 * 2;  // ~200ms stereo
            ctx->audio_ring_buffer =
                av_mallocz(ctx->audio_ring_size * sizeof(int16_t));
            if (!ctx->audio_ring_buffer)
            {
                av_log(avctx, AV_LOG_WARNING,
                       "Failed to allocate audio ring buffer\n");
                ctx->has_audio = false;
            }
            else
            {
                ctx->audio_ring_read = 0;
                ctx->audio_ring_write = 0;
                ctx->audio_ring_count = 0;

                VHD_AUDIOCHANNEL *pChn =
                    &ctx->audio_info.sdi.audio_info.pAudioGroups[0]
                         .pAudioChannels[0];
                ctx->audio_block_size =
                    VHD_GetBlockSize(pChn->BufferFormat, pChn->Mode);
            }
        }
    }

    /* Start stream */
    av_error = ff_videomaster_handle_vhd_status(
        avctx, VHD_StartStream(ctx->stream_handle),
        "TX stream started successfully", "Failed to start TX stream");
    if (av_error != 0)
    {
        ff_videomaster_close_stream_handle(ctx);
        ff_videomaster_close_board_handle(ctx);
        return av_error;
    }

    /* Preroll: fill slots with black */
    av_log(avctx, AV_LOG_INFO,
           "Preroll: filling %d slots with black (queue depth %d)\n",
           ctx->preroll_count, ctx->queue_depth);

    for (uint32_t i = 0; i < ctx->preroll_count; i++)
    {
        HANDLE   slot = NULL;
        uint8_t *buffer = NULL;
        uint32_t buffer_size = 0;

        if (VHD_LockSlotHandle(ctx->stream_handle, &slot) != VHDERR_NOERROR)
        {
            av_log(avctx, AV_LOG_WARNING,
                   "Preroll: LockSlotHandle failed at slot %d\n", i);
            break;
        }

        VHD_GetSlotBuffer(slot, VHD_SDI_BT_VIDEO, &buffer, &buffer_size);
        if (buffer)
            fill_black_buffer(buffer, buffer_size, ctx->video_buffer_packing);

        /* Embed silence audio */
        if (ctx->has_audio)
        {
            VHD_AUDIOCHANNEL *pChn =
                &ctx->audio_info.sdi.audio_info.pAudioGroups[0]
                     .pAudioChannels[0];
            pChn->DataSize = 0;

            VHD_ERRORCODE embed_result =
                VHD_SlotEmbedAudio(slot, &ctx->audio_info.sdi.audio_info);
            if (embed_result == VHDERR_BUFFERTOOSMALL && pChn->DataSize > 0)
            {
                memset(pChn->pData, 0, pChn->DataSize);
                VHD_SlotEmbedAudio(slot, &ctx->audio_info.sdi.audio_info);
            }
        }

        VHD_UnlockSlotHandle(slot);
    }

    ctx->playback_started = true;

    av_log(avctx, AV_LOG_INFO, "VideoMaster TX initialized successfully\n");
    return 0;
}

int ff_videomaster_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    VideoMasterEncData *enc_data =
        (VideoMasterEncData *)avctx->priv_data;
    VideoMasterContext *ctx =
        (VideoMasterContext *)enc_data->context;

    if (!ctx || !ctx->playback_started)
        return AVERROR(EIO);

    /* Determine if this is a video or audio packet */
    AVStream *st = avctx->streams[pkt->stream_index];

    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->has_audio)
    {
        /* Store audio in ring buffer */
        int16_t *samples = (int16_t *)pkt->data;
        uint32_t nb_samples = pkt->size / (sizeof(int16_t));
        uint32_t capacity = ctx->audio_ring_size;

        for (uint32_t i = 0; i < nb_samples; i++)
        {
            if (ctx->audio_ring_count >= capacity)
                break;

            ((int16_t *)ctx->audio_ring_buffer)[ctx->audio_ring_write] =
                samples[i];
            ctx->audio_ring_write =
                (ctx->audio_ring_write + 1) % capacity;
            ctx->audio_ring_count++;
        }

        return 0;
    }
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        /* Lock slot, write video, embed audio, unlock */
        HANDLE   slot = NULL;
        uint8_t *buffer = NULL;
        uint32_t buffer_size = 0;

        int lock_result = ff_videomaster_lock_slot(ctx);
        if (lock_result == AVERROR(EAGAIN))
        {
            av_log(avctx, AV_LOG_WARNING, "TX slot lock timeout\n");
            return 0;  // drop frame
        }
        else if (lock_result != 0)
            return AVERROR(EIO);

        slot = ctx->slot_handle;

        VHD_GetSlotBuffer(slot, VHD_SDI_BT_VIDEO, &buffer, &buffer_size);
        if (buffer)
        {
            uint32_t copy_size = FFMIN((uint32_t)pkt->size, buffer_size);
            memcpy(buffer, pkt->data, copy_size);
            if (copy_size < buffer_size)
                memset(buffer + copy_size, 0, buffer_size - copy_size);
        }

        /* Embed audio from ring buffer */
        if (ctx->has_audio)
        {
            VHD_AUDIOCHANNEL *pChn =
                &ctx->audio_info.sdi.audio_info.pAudioGroups[0]
                     .pAudioChannels[0];
            pChn->DataSize = 0;

            VHD_ERRORCODE embed_result =
                VHD_SlotEmbedAudio(slot, &ctx->audio_info.sdi.audio_info);

            if (embed_result == VHDERR_BUFFERTOOSMALL && pChn->DataSize > 0)
            {
                uint32_t needed_samples =
                    pChn->DataSize / ctx->audio_block_size;
                int16_t *dst = (int16_t *)pChn->pData;
                uint32_t capacity = ctx->audio_ring_size;

                for (uint32_t i = 0; i < needed_samples; i++)
                {
                    if (ctx->audio_ring_count >= 2)
                    {
                        dst[2 * i + 0] =
                            ((int16_t *)ctx->audio_ring_buffer)
                                [ctx->audio_ring_read];
                        ctx->audio_ring_read =
                            (ctx->audio_ring_read + 1) % capacity;
                        dst[2 * i + 1] =
                            ((int16_t *)ctx->audio_ring_buffer)
                                [ctx->audio_ring_read];
                        ctx->audio_ring_read =
                            (ctx->audio_ring_read + 1) % capacity;
                        ctx->audio_ring_count -= 2;
                    }
                    else
                    {
                        dst[2 * i + 0] = 0;
                        dst[2 * i + 1] = 0;
                    }
                }

                VHD_SlotEmbedAudio(slot, &ctx->audio_info.sdi.audio_info);
            }
        }

        ff_videomaster_unlock_slot(ctx);
        return 0;
    }

    return 0;
}

int ff_videomaster_write_trailer(AVFormatContext *avctx)
{
    VideoMasterEncData *enc_data =
        (VideoMasterEncData *)avctx->priv_data;
    VideoMasterContext *ctx =
        (VideoMasterContext *)enc_data->context;

    if (!ctx)
        return 0;

    /* Stop stream */
    if (ctx->stream_handle)
    {
        VHD_StopStream(ctx->stream_handle);
        VHD_CloseStreamHandle(ctx->stream_handle);
        ctx->stream_handle = NULL;
    }

    /* Disable keyer */
    if (ctx->has_keyer &&
        ctx->keying_mode != AV_VIDEOMASTER_KEYING_NONE &&
        ctx->board_handle)
    {
        VHD_SetBoardProperty(ctx->board_handle, VHD_KEYER_BP_ENABLE, FALSE);
    }

    /* Re-enable loopback */
    if (ctx->board_handle)
    {
        ff_videomaster_enable_loopback_on_channel(ctx);
        if (ctx->use_yuvk)
        {
            uint32_t saved_channel = ctx->channel_index;
            ctx->channel_index = saved_channel + 1;
            ff_videomaster_enable_loopback_on_channel(ctx);
            ctx->channel_index = saved_channel;
        }
    }

    /* Release audio info and ring buffer */
    if (ctx->has_audio)
    {
        ff_videomaster_release_audio_info(ctx,
                                           &ctx->audio_info.sdi.audio_info);
        av_freep(&ctx->audio_ring_buffer);
    }

    /* Close board handle */
    if (ctx->board_handle)
        ff_videomaster_close_board_handle(ctx);

    av_freep(&enc_data->context);

    av_log(avctx, AV_LOG_INFO, "VideoMaster TX stopped\n");
    return 0;
}

/** AVOption definitions */

static const AVOption options[] = {
    { "board_index",
      "Index of the Deltacast board to use",
      OFFSET(board_index),
      AV_OPT_TYPE_INT64,
      { .i64 = 0 },
      0,
      15,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM },
    { "channel_index",
      "Index of the TX channel to use",
      OFFSET(channel_index),
      AV_OPT_TYPE_INT64,
      { .i64 = 0 },
      0,
      11,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM },
    { "mode",
      "Video mode index (1-55, see documentation for mode table)",
      OFFSET(mode),
      AV_OPT_TYPE_INT64,
      { .i64 = 0 },
      0,
      55,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM },
    { "buffer_packing",
      "Buffer packing format",
      OFFSET(buffer_packing),
      AV_OPT_TYPE_INT64,
      { .i64 = AV_NB_VIDEOMASTER_BUFFER_PACKINGS },
      0,
      AV_NB_VIDEOMASTER_BUFFER_PACKINGS,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUV422_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUV422_8 },
      0,
      0,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "YUVK4224_8",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_YUVK4224_8 },
      0,
      0,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "RGBA_32",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_RGBA_32 },
      0,
      0,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "RGB_32",
      NULL,
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_BUFFER_PACKING_RGB_32 },
      0,
      0,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "buffer_packing_value" },
    { "queue_depth",
      "Buffer queue depth",
      OFFSET(queue_depth),
      AV_OPT_TYPE_INT64,
      { .i64 = 4 },
      2,
      16,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM },
    { "keying_mode",
      "Keying mode for output",
      OFFSET(keying_mode),
      AV_OPT_TYPE_INT64,
      { .i64 = AV_VIDEOMASTER_KEYING_NONE },
      0,
      2,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "keying_mode_value" },
    { "none",
      "No keying",
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_KEYING_NONE },
      0,
      0,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "keying_mode_value" },
    { "external",
      "External keying (fill + key output)",
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_KEYING_EXTERNAL },
      0,
      0,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "keying_mode_value" },
    { "internal",
      "Internal keying (composite with RX signal)",
      0,
      AV_OPT_TYPE_CONST,
      { .i64 = AV_VIDEOMASTER_KEYING_INTERNAL },
      0,
      0,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM,
      .unit = "keying_mode_value" },
    { "set_bidir_outputs",
      "Number of TX channels to configure in BiDir mode (-1 = don't touch)",
      OFFSET(set_bidir_outputs),
      AV_OPT_TYPE_INT64,
      { .i64 = -1 },
      -1,
      12,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM },
    { "preroll",
      "Number of preroll frames (-1 = queue_depth - 1)",
      OFFSET(preroll),
      AV_OPT_TYPE_INT64,
      { .i64 = -1 },
      -1,
      15,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM },
    { "rx_channel",
      "RX channel for internal keying genlock source",
      OFFSET(rx_channel),
      AV_OPT_TYPE_INT64,
      { .i64 = 0 },
      0,
      11,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_VIDEO_PARAM },
    { "nb_channels",
      "Number of audio channels (0 = no audio)",
      OFFSET(nb_channels),
      AV_OPT_TYPE_INT64,
      { .i64 = 2 },
      0,
      8,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_AUDIO_PARAM },
    { "sample_rate",
      "Audio sample rate",
      OFFSET(sample_rate),
      AV_OPT_TYPE_INT64,
      { .i64 = 48000 },
      32000,
      48000,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_AUDIO_PARAM },
    { "sample_size",
      "Audio sample size in bits",
      OFFSET(sample_size),
      AV_OPT_TYPE_INT64,
      { .i64 = 16 },
      16,
      24,
      AV_OPT_FLAG_ENCODING_PARAM | ENC | AV_OPT_FLAG_AUDIO_PARAM },
    { NULL },
};

static const AVClass videomaster_muxer_class = {
    .class_name = "DELTACAST Videomaster outdev",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

const FFOutputFormat ff_videomaster_muxer = {
    .p.name = "videomaster",
    .p.long_name = NULL_IF_CONFIG_SMALL("DELTACAST Videomaster output"),
    .p.audio_codec = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec = AV_CODEC_ID_RAWVIDEO,
    .p.flags = AVFMT_NOFILE,
    .p.priv_class = &videomaster_muxer_class,
    .get_device_list = ff_videomaster_list_output_devices,
    .priv_data_size = sizeof(struct VideoMasterEncData),
    .write_header = ff_videomaster_write_header,
    .write_packet = ff_videomaster_write_packet,
    .write_trailer = ff_videomaster_write_trailer,
};
