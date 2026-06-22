/**
 * @file videomaster_audio_pipe.h
 * @brief Raw-PCM IPC audio pipe for the VideoMaster demuxer (liveedit fork).
 *
 * Creates a Windows named pipe and a background thread that drains a ring
 * buffer of captured PCM audio into it. Lets a second process consume the
 * audio stream via `-f s16le|s24le -i \\.\pipe\<name>`, independently of
 * the main demuxer flow (packets to the tee are gated by wait_for_input;
 * the pipe is not).
 *
 * On non-Windows platforms the API compiles to no-ops.
 */

#ifndef AVDEVICE_VIDEOMASTER_AUDIO_PIPE_H
#define AVDEVICE_VIDEOMASTER_AUDIO_PIPE_H

#include <stddef.h>
#include <stdint.h>

#include "libavformat/avformat.h"

typedef struct VideoMasterAudioPipe VideoMasterAudioPipe;

/**
 * @brief Creates a named pipe server, allocates the ring buffer and starts
 *        the drain thread.
 *
 * @param avctx         AVFormatContext for logging.
 * @param pipe_name     Full pipe path (e.g. "\\\\.\\pipe\\liveedit_audio").
 *                      Must stay valid only for the duration of this call.
 * @param sample_rate   Audio sample rate (Hz). Used for ring sizing and logs.
 * @param nb_channels   Number of interleaved channels.
 * @param sample_size   Sample size in bits (16 or 24). Expanded to 32 bits
 *                      per sample on the wire when 24 (VideoMaster returns
 *                      S24 in 32-bit containers).
 * @return Opaque handle on success, NULL on failure.
 */
VideoMasterAudioPipe *ff_videomaster_audio_pipe_create(
    AVFormatContext *avctx, const char *pipe_name,
    uint32_t sample_rate, uint32_t nb_channels, uint32_t sample_size);

/**
 * @brief Pushes raw interleaved PCM bytes into the ring. Non-blocking.
 *        If the ring is full, drops the oldest bytes to make room and logs
 *        a throttled warning. Safe to call from the demuxer read thread.
 *
 * @param pipe   Pipe handle (may be NULL — call is then a no-op).
 * @param data   PCM bytes.
 * @param size   Number of bytes.
 */
void ff_videomaster_audio_pipe_push(VideoMasterAudioPipe *pipe,
                                    const uint8_t *data, uint32_t size);

/**
 * @brief Signals the drain thread to exit, joins it and releases all
 *        resources. Safe to call with NULL.
 */
void ff_videomaster_audio_pipe_close(VideoMasterAudioPipe *pipe);

#endif /* AVDEVICE_VIDEOMASTER_AUDIO_PIPE_H */
