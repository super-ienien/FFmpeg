# FFmpeg Deltacast Fork — Custom Parameters

This FFmpeg fork adds DELTACAST VideoMaster and Blackmagic DeckLink capture/output device support with custom options for the Webcenter liveedit project.

## Build

Run `./build_win64.sh` from an MSYS2 MinGW64 shell. The script handles dependencies, SDK header generation, and produces a fully static `ffmpeg.exe` (no DLL dependencies except VideoMaster hardware drivers).

Requirements:
- MSYS2 with MinGW64
- DELTACAST VideoMaster SDK at `../../sdk/VideoMaster/`
- Blackmagic DeckLink SDK (14.2) at `../../sdk/Decklink/Blackmagic_DeckLink_SDK_14.2/`

## VideoMaster Custom Options (`-f videomaster`)

### Standard options (from DELTACAST)

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `board_index` | int | -1 | Board index (required with `-i dummy`) |
| `channel_index` | int | -1 | Channel/stream index (required with `-i dummy`) |
| `timestamp_source` | enum | osc | Timestamp source: `osc`, `system`, `hw`, `ltc_on_board`, `ltc_companion_card` |
| `nb_channels` | int | -1 | Audio channels (SDI only, HDMI auto-detects) |
| `sample_rate` | enum | auto | Audio sample rate: `48000`, `44100`, `32000` |
| `sample_size` | enum | auto | Audio bit depth: `16`, `24` |
| `buffer_packing` | enum | auto | Video buffer packing format (YUV422_8, RGBA_32, etc.) |
| `auto_set_ltc_input` | bool | 0 | Auto-configure REF_IN for LTC on board |

### Custom options added for liveedit

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `wait_for_input` | bool | 0 | Wait for user to press 'r' before capturing. Frames are dropped until key received on stdin. |
| `no_autodetect_timeout` | bool | 0 | Wait indefinitely for signal instead of 3sec timeout. |
| `list_formats` | bool | 0 | List all VideoMaster video modes with index, resolution, fps, and exit. |
| `signal_no_stop` | bool | 0 | Do not stop on signal loss. Generate black video + silent audio instead. Requires `default_video_mode` when starting without signal. |
| `default_video_mode` | int | -1 | Default `VHD_VIDEOSTANDARD` index for no-signal startup. Use `-list_formats 1` to see the mode table. |
| `disjoined_streams` | bool | 0 | Open separate disjoined video and ANC streams instead of a single joined stream. Slot timestamps are compared to ensure video and ANC buffers are temporally synchronized. SDI only. |
| `audio_pipe` | string | none | Windows named pipe path (e.g. `\\.\pipe\liveedit_audio`). When set, raw interleaved PCM audio is continuously streamed to this pipe — independently of `wait_for_input` / `wait_for_tc` / signal loss. A second process consumes it with `-f s16le\|s24le -ar <rate> -ac <channels> -i <pipe>`. Single consumer, drops oldest bytes on consumer lag. Windows only. |

### Usage examples

```bash
# List available devices
ffmpeg -sources videomaster

# List all video modes (to find the index for default_video_mode)
ffmpeg -f videomaster -list_formats 1 -i dummy

# Basic capture
ffmpeg -f videomaster -board_index 0 -channel_index 0 -i dummy -c:v libx264 output.mp4

# Capture with wait for user input
ffmpeg -f videomaster -wait_for_input 1 -i dummy output.mp4

# Wait indefinitely for signal
ffmpeg -f videomaster -no_autodetect_timeout 1 -i dummy output.mp4

# Start without signal, generate black frames (mode 0 = 1080p25)
ffmpeg -f videomaster -signal_no_stop 1 -default_video_mode 0 -i dummy output.mp4

# Resilient capture: continue on signal loss with black frames
ffmpeg -f videomaster -signal_no_stop 1 -default_video_mode 0 -i dummy -c:v libx264 output.mp4

# Capture with disjoined video/ANC streams (timestamp-synchronized)
ffmpeg -f videomaster -disjoined_streams 1 -board_index 0 -channel_index 0 -i dummy -c:v libx264 output.mp4

# Process 1 — waits for 'r' but always streams audio to the named pipe:
ffmpeg -f videomaster -wait_for_input 1 -audio_pipe '\\.\pipe\liveedit_audio' \
    -board_index 0 -channel_index 0 -i dummy -c:v libx264 output.mp4

# Process 2 — consumes the IPC audio (24-bit SDI, 16 channels @ 48 kHz example):
ffmpeg -f s24le -ar 48000 -ac 16 -i '\\.\pipe\liveedit_audio' -c:a copy audio.wav
```

## DeckLink Custom Options (`-f decklink`)

### Custom options added for liveedit

These options extend the standard DeckLink FFmpeg input device:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `wait_for_input` | bool | 0 | Wait for user to press 'r' before capturing. |
| `no_autodetect_timeout` | bool | 0 | Wait indefinitely for signal instead of 3sec timeout. |
| `signal_loss_action` | enum | bars | Action on signal loss: `none`, `bars`, `repeat`, `black` |
| `default_video_mode` | int | -1 | Default display mode index for no-signal startup (0-based, from `-list_formats` output). |

The `black` signal loss action (added for liveedit) generates black video frames and silent audio when input signal is lost, instead of stopping or showing color bars.

### Usage examples

```bash
# List available modes (note the format_code and position index)
ffmpeg -f decklink -list_formats 1 -i "DeckLink Mini Recorder"

# Basic capture
ffmpeg -f decklink -i "DeckLink Mini Recorder" -c:v libx264 output.mp4

# Capture with black frames on signal loss
ffmpeg -f decklink -signal_loss_action black -i "DeckLink Mini Recorder" output.mp4

# Start without signal using mode index 12, black on signal loss
ffmpeg -f decklink -signal_loss_action black -default_video_mode 12 -i "DeckLink Mini Recorder" output.mp4

# Wait for user input before capture
ffmpeg -f decklink -wait_for_input 1 -i "DeckLink Mini Recorder" output.mp4
```

## VideoMaster Video Mode Table (common modes)

| Index | Standard | Resolution | FPS | Type |
|-------|----------|-----------|-----|------|
| 0 | S274M 1080p 25Hz | 1920x1080 | 25 | progressive |
| 1 | S274M 1080p 30Hz | 1920x1080 | 30 | progressive |
| 2 | S274M 1080i 50Hz | 1920x1080 | 50 | interlaced |
| 3 | S274M 1080i 60Hz | 1920x1080 | 60 | interlaced |
| 4 | S296M 720p 50Hz | 1280x720 | 50 | progressive |
| 5 | S296M 720p 60Hz | 1280x720 | 60 | progressive |
| 6 | S259M PAL | 720x576 | 25 | interlaced |
| 7 | S259M NTSC 487 | 720x487 | 30 | interlaced |
| 8 | S274M 1080p 24Hz | 1920x1080 | 24 | progressive |
| 9 | S274M 1080p 60Hz | 1920x1080 | 60 | progressive |
| 10 | S274M 1080p 50Hz | 1920x1080 | 50 | progressive |

Use `ffmpeg -f videomaster -list_formats 1 -i dummy` for the complete list.
