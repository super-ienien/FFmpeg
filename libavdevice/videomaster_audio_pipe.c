/**
 * @file videomaster_audio_pipe.c
 * @brief Named-pipe + ring-buffer + drain-thread IPC for VideoMaster audio.
 *
 * Main demuxer pushes PCM bytes into a per-device ring via
 * ff_videomaster_audio_pipe_push(). A background thread:
 *   1. Creates/awaits a client on the named pipe (overlapped
 *      ConnectNamedPipe, cancellable via a shutdown event).
 *   2. Drains bytes from the ring and writes them with overlapped
 *      WriteFile, waiting on [write_complete, shutdown] so a stuck
 *      client cannot block teardown.
 *   3. On ERROR_BROKEN_PIPE disconnects the client and loops back to
 *      waiting for a new one.
 *
 * When the ring overflows we drop the oldest bytes (hardware capture
 * must never stall). A throttled WARNING is emitted at most every
 * 2 seconds so logs stay readable.
 */

#include "videomaster_audio_pipe.h"

#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32

struct VideoMasterAudioPipe {
    AVFormatContext *avctx;

    HANDLE     pipe;
    HANDLE     thread;
    HANDLE     shutdown_event;

    /* Ring buffer protected by cs + data_cv. */
    uint8_t           *ring;
    uint32_t           ring_capacity;
    uint32_t           ring_read;
    uint32_t           ring_write;
    uint32_t           ring_count;
    CRITICAL_SECTION   cs;
    CONDITION_VARIABLE data_cv;

    volatile LONG stop;

    /* Stats + throttled drop warning. */
    uint64_t total_pushed;
    uint64_t total_written;
    uint64_t total_dropped;
    int64_t  last_drop_log_us;

    char pipe_name[256];
};

/* Power-of-two not required — we wrap with modulo on the capacity. */
#define VM_AP_WRITE_CHUNK (64 * 1024)

static DWORD WINAPI drain_thread_proc(LPVOID arg);

VideoMasterAudioPipe *ff_videomaster_audio_pipe_create(
    AVFormatContext *avctx, const char *pipe_name,
    uint32_t sample_rate, uint32_t nb_channels, uint32_t sample_size)
{
    VideoMasterAudioPipe *p;
    uint32_t bytes_per_sample;
    uint32_t bytes_per_second;
    uint32_t ring_capacity;
    DWORD    thread_id;

    if (!pipe_name || !*pipe_name) {
        av_log(avctx, AV_LOG_ERROR, "audio_pipe: empty pipe name\n");
        return NULL;
    }

    /* VideoMaster hands us 16-bit samples on 2 bytes and 24-bit samples
     * on 3 bytes (packed, little-endian) — matches PCM_S16LE / PCM_S24LE
     * exactly, which is what the consumer reads with -f s16le / -f s24le. */
    if (sample_size == 24)
        bytes_per_sample = 3;
    else if (sample_size == 16)
        bytes_per_sample = 2;
    else {
        av_log(avctx, AV_LOG_ERROR,
               "audio_pipe: unsupported sample size %u\n", sample_size);
        return NULL;
    }

    if (nb_channels == 0 || sample_rate == 0) {
        av_log(avctx, AV_LOG_ERROR,
               "audio_pipe: invalid audio params (rate=%u, channels=%u)\n",
               sample_rate, nb_channels);
        return NULL;
    }

    bytes_per_second = sample_rate * nb_channels * bytes_per_sample;
    /* 1 second of audio, clamped to [2 MiB, 8 MiB]. At 48 kHz / 16 ch /
     * 24-bit that's ~3 MiB, at 48 kHz / 2 ch / 16-bit ~188 KiB → floored
     * to 2 MiB. Plenty of slack if the consumer briefly stalls. */
    ring_capacity = bytes_per_second;
    if (ring_capacity < 2 * 1024 * 1024)
        ring_capacity = 2 * 1024 * 1024;
    if (ring_capacity > 8 * 1024 * 1024)
        ring_capacity = 8 * 1024 * 1024;

    p = av_mallocz(sizeof(*p));
    if (!p)
        return NULL;

    p->avctx = avctx;
    p->pipe = INVALID_HANDLE_VALUE;
    p->thread = NULL;
    p->shutdown_event = NULL;
    p->ring_capacity = ring_capacity;

    p->ring = av_malloc(ring_capacity);
    if (!p->ring)
        goto fail;

    InitializeCriticalSection(&p->cs);
    InitializeConditionVariable(&p->data_cv);

    snprintf(p->pipe_name, sizeof(p->pipe_name), "%s", pipe_name);

    /* PIPE_ACCESS_OUTBOUND: we only write. FILE_FLAG_OVERLAPPED: lets the
     * drain thread wait on the connect event together with the shutdown
     * event so close doesn't hang while no client is attached. */
    p->pipe = CreateNamedPipeA(
        p->pipe_name,
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,                  /* 1 instance — single consumer */
        VM_AP_WRITE_CHUNK,  /* out buffer */
        0,                  /* in buffer */
        0,                  /* default timeout */
        NULL);
    if (p->pipe == INVALID_HANDLE_VALUE) {
        av_log(avctx, AV_LOG_ERROR,
               "audio_pipe: CreateNamedPipeA(\"%s\") failed (err=%lu)\n",
               p->pipe_name, (unsigned long)GetLastError());
        goto fail;
    }

    p->shutdown_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!p->shutdown_event) {
        av_log(avctx, AV_LOG_ERROR,
               "audio_pipe: CreateEventA failed (err=%lu)\n",
               (unsigned long)GetLastError());
        goto fail;
    }

    p->thread = CreateThread(NULL, 0, drain_thread_proc, p, 0, &thread_id);
    if (!p->thread) {
        av_log(avctx, AV_LOG_ERROR,
               "audio_pipe: CreateThread failed (err=%lu)\n",
               (unsigned long)GetLastError());
        goto fail;
    }

    av_log(avctx, AV_LOG_INFO,
           "audio_pipe: ready on \"%s\" — PCM %s, %u Hz, %u ch (ring=%u KiB)\n",
           p->pipe_name,
           sample_size == 24 ? "s24le" : "s16le",
           sample_rate, nb_channels, ring_capacity / 1024);

    return p;

fail:
    if (p->pipe != INVALID_HANDLE_VALUE)
        CloseHandle(p->pipe);
    if (p->shutdown_event)
        CloseHandle(p->shutdown_event);
    if (p->ring) {
        DeleteCriticalSection(&p->cs);
        av_free(p->ring);
    }
    av_free(p);
    return NULL;
}

void ff_videomaster_audio_pipe_push(VideoMasterAudioPipe *p,
                                    const uint8_t *data, uint32_t size)
{
    uint32_t room, drop_now, first_chunk;

    if (!p || !data || size == 0)
        return;

    EnterCriticalSection(&p->cs);

    /* If incoming chunk is bigger than the ring, only keep the tail. */
    if (size > p->ring_capacity) {
        uint32_t skip = size - p->ring_capacity;
        data += skip;
        size -= skip;
        p->total_dropped += skip;
    }

    room = p->ring_capacity - p->ring_count;
    if (room < size) {
        /* Drop the oldest bytes to make room. */
        drop_now = size - room;
        p->ring_read = (p->ring_read + drop_now) % p->ring_capacity;
        p->ring_count -= drop_now;
        p->total_dropped += drop_now;

        {
            int64_t now = av_gettime_relative();
            if (now - p->last_drop_log_us > 2000000) {
                p->last_drop_log_us = now;
                av_log(p->avctx, AV_LOG_WARNING,
                       "audio_pipe: consumer lag — dropped %llu bytes so far "
                       "(ring full)\n",
                       (unsigned long long)p->total_dropped);
            }
        }
    }

    /* Copy, possibly in two chunks if we wrap. */
    first_chunk = p->ring_capacity - p->ring_write;
    if (first_chunk > size)
        first_chunk = size;
    memcpy(p->ring + p->ring_write, data, first_chunk);
    if (first_chunk < size)
        memcpy(p->ring, data + first_chunk, size - first_chunk);

    p->ring_write = (p->ring_write + size) % p->ring_capacity;
    p->ring_count += size;
    p->total_pushed += size;

    LeaveCriticalSection(&p->cs);
    WakeConditionVariable(&p->data_cv);
}

/* Pops up to @max bytes from the ring into @out. Returns the number of
 * bytes actually popped. Blocks on data_cv until either data arrives or
 * shutdown is signalled. Returns 0 only on shutdown. */
static uint32_t pop_bytes(VideoMasterAudioPipe *p, uint8_t *out, uint32_t max)
{
    uint32_t n, first_chunk;

    EnterCriticalSection(&p->cs);
    while (p->ring_count == 0 && !p->stop) {
        /* 100 ms tick so we also notice shutdown that was signalled
         * without a Wake (belt-and-braces). */
        SleepConditionVariableCS(&p->data_cv, &p->cs, 100);
    }
    if (p->stop && p->ring_count == 0) {
        LeaveCriticalSection(&p->cs);
        return 0;
    }

    n = p->ring_count < max ? p->ring_count : max;
    first_chunk = p->ring_capacity - p->ring_read;
    if (first_chunk > n)
        first_chunk = n;
    memcpy(out, p->ring + p->ring_read, first_chunk);
    if (first_chunk < n)
        memcpy(out + first_chunk, p->ring, n - first_chunk);

    p->ring_read = (p->ring_read + n) % p->ring_capacity;
    p->ring_count -= n;
    LeaveCriticalSection(&p->cs);

    return n;
}

/* Returns 1 on connect, 0 on shutdown, -1 on fatal error. */
static int wait_for_client(VideoMasterAudioPipe *p)
{
    OVERLAPPED ov;
    HANDLE     waits[2];
    DWORD      wait_result;
    BOOL       ok;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
        av_log(p->avctx, AV_LOG_ERROR,
               "audio_pipe: CreateEventA (connect) failed (err=%lu)\n",
               (unsigned long)GetLastError());
        return -1;
    }

    ok = ConnectNamedPipe(p->pipe, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            /* Client raced us and is already connected — done. */
            CloseHandle(ov.hEvent);
            av_log(p->avctx, AV_LOG_INFO,
                   "audio_pipe: client already connected on \"%s\"\n",
                   p->pipe_name);
            return 1;
        }
        if (err != ERROR_IO_PENDING) {
            av_log(p->avctx, AV_LOG_ERROR,
                   "audio_pipe: ConnectNamedPipe failed (err=%lu)\n",
                   (unsigned long)err);
            CloseHandle(ov.hEvent);
            return -1;
        }
    }

    waits[0] = ov.hEvent;
    waits[1] = p->shutdown_event;
    wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0) {
        DWORD dummy;
        GetOverlappedResult(p->pipe, &ov, &dummy, FALSE);
        CloseHandle(ov.hEvent);
        av_log(p->avctx, AV_LOG_INFO,
               "audio_pipe: client connected on \"%s\"\n", p->pipe_name);
        return 1;
    }

    /* Shutdown: cancel the pending connect before closing the event. */
    CancelIoEx(p->pipe, &ov);
    CloseHandle(ov.hEvent);
    return 0;
}

/* Returns 1 on success, 0 on shutdown, -1 on pipe error (caller should
 * disconnect and wait for a new client). */
static int write_chunk(VideoMasterAudioPipe *p, const uint8_t *buf, DWORD len)
{
    OVERLAPPED ov;
    HANDLE     waits[2];
    DWORD      wait_result;
    DWORD      written = 0;
    BOOL       ok;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
        av_log(p->avctx, AV_LOG_ERROR,
               "audio_pipe: CreateEventA (write) failed (err=%lu)\n",
               (unsigned long)GetLastError());
        return -1;
    }

    ok = WriteFile(p->pipe, buf, len, &written, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            av_log(p->avctx, AV_LOG_INFO,
                   "audio_pipe: WriteFile failed (err=%lu) — dropping client\n",
                   (unsigned long)err);
            CloseHandle(ov.hEvent);
            return -1;
        }
    } else {
        /* Completed synchronously. */
        CloseHandle(ov.hEvent);
        p->total_written += written;
        return 1;
    }

    waits[0] = ov.hEvent;
    waits[1] = p->shutdown_event;
    wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0) {
        if (!GetOverlappedResult(p->pipe, &ov, &written, FALSE)) {
            av_log(p->avctx, AV_LOG_INFO,
                   "audio_pipe: GetOverlappedResult failed (err=%lu) — "
                   "dropping client\n", (unsigned long)GetLastError());
            CloseHandle(ov.hEvent);
            return -1;
        }
        p->total_written += written;
        CloseHandle(ov.hEvent);
        return 1;
    }

    /* Shutdown mid-write — abort the I/O cleanly. */
    CancelIoEx(p->pipe, &ov);
    CloseHandle(ov.hEvent);
    return 0;
}

static DWORD WINAPI drain_thread_proc(LPVOID arg)
{
    VideoMasterAudioPipe *p = (VideoMasterAudioPipe *)arg;
    uint8_t *buf;

    buf = av_malloc(VM_AP_WRITE_CHUNK);
    if (!buf) {
        av_log(p->avctx, AV_LOG_ERROR,
               "audio_pipe: drain thread failed to allocate buffer\n");
        return 1;
    }

    while (!p->stop) {
        int r = wait_for_client(p);
        if (r <= 0)
            break;  /* 0 = shutdown, -1 = fatal */

        /* Connected — drain until error or shutdown. */
        while (!p->stop) {
            uint32_t n = pop_bytes(p, buf, VM_AP_WRITE_CHUNK);
            if (n == 0)
                break;  /* shutdown signalled with empty ring */

            {
                int w = write_chunk(p, buf, n);
                if (w == 0)
                    goto out;  /* shutdown */
                if (w < 0)
                    break;      /* reset client */
            }
        }

        /* Reset the pipe instance so a new client can connect. */
        DisconnectNamedPipe(p->pipe);
    }

out:
    av_free(buf);
    av_log(p->avctx, AV_LOG_INFO,
           "audio_pipe: drain thread exiting "
           "(pushed=%llu, written=%llu, dropped=%llu bytes)\n",
           (unsigned long long)p->total_pushed,
           (unsigned long long)p->total_written,
           (unsigned long long)p->total_dropped);
    return 0;
}

void ff_videomaster_audio_pipe_close(VideoMasterAudioPipe *p)
{
    if (!p)
        return;

    InterlockedExchange(&p->stop, 1);
    if (p->shutdown_event)
        SetEvent(p->shutdown_event);
    WakeAllConditionVariable(&p->data_cv);

    if (p->thread) {
        /* Generous timeout — the drain thread shuts down as soon as it
         * notices either shutdown_event or stop, but we still want a
         * bounded wait in case of a wedged I/O. */
        if (WaitForSingleObject(p->thread, 5000) == WAIT_TIMEOUT) {
            av_log(p->avctx, AV_LOG_WARNING,
                   "audio_pipe: drain thread didn't stop in 5s, terminating\n");
            TerminateThread(p->thread, 1);
        }
        CloseHandle(p->thread);
    }

    if (p->pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(p->pipe);
        CloseHandle(p->pipe);
    }
    if (p->shutdown_event)
        CloseHandle(p->shutdown_event);

    DeleteCriticalSection(&p->cs);
    av_free(p->ring);
    av_free(p);
}

#else /* !_WIN32 */

VideoMasterAudioPipe *ff_videomaster_audio_pipe_create(
    AVFormatContext *avctx, const char *pipe_name,
    uint32_t sample_rate, uint32_t nb_channels, uint32_t sample_size)
{
    (void)pipe_name; (void)sample_rate; (void)nb_channels; (void)sample_size;
    av_log(avctx, AV_LOG_ERROR,
           "audio_pipe: only supported on Windows\n");
    return NULL;
}

void ff_videomaster_audio_pipe_push(VideoMasterAudioPipe *p,
                                    const uint8_t *data, uint32_t size)
{
    (void)p; (void)data; (void)size;
}

void ff_videomaster_audio_pipe_close(VideoMasterAudioPipe *p)
{
    (void)p;
}

#endif /* _WIN32 */
