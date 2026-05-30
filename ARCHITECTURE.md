# Architecture

## Source layout

```
ncradio/
├── ncradio.c   — ncurses UI and application main loop
├── radio.c     — V4L2 hardware interface and scan/seek threads
├── radio.h     — Radio struct and public function declarations
├── rds.c       — RDS block decoder (PS name, Radio Text)
├── rds.h       — RdsDecoder struct and rds_feed() declaration
├── audio.c     — audio pipe (capture → playback thread) + device autodetect
│                 PipeWire build: both streams via pw_stream + spa_ringbuffer
│                 ALSA build:    capture + playback via libasound
├── audio.h     — Audio struct and public function declarations
├── record.c    — MP3 encoder wrapper (libmp3lame); compiled only with HAVE_LAME
├── record.h    — Record struct and public function declarations
├── config.c    — ~/.ncradio.conf read/write
├── config.h    — Config struct and function declarations
├── Makefile
└── configure   — build-time feature detection (libpipewire-0.3, libasound, libudev, libmp3lame)
```

## Module responsibilities

### `rds.c` / `rds.h`

A self-contained V4L2 RDS block decoder with no dependencies beyond libc.

**`RdsDecoder` struct:**

```
pending_b/c/d   — last received block words (assembled into a group on block D)
b_ok/c_ok/d_ok  — cleared if block had an uncorrectable error
ps[]            — 8-char Program Service name (station name), null-terminated
rt[]            — 64-char Radio Text, null-terminated
ps_ready        — 1 when all 4 PS segments have been received
rt_ready        — 1 when a contiguous run of RT segments has been received
ps_seg_seen     — 4-bit mask: which of the four PS segments (0-3) arrived
rt_seg_seen     — 16-bit mask: which of the 16 RT segments arrived
rt_ab           — current RT A/B flag; toggle signals a new RT message
```

**`rds_feed(d, lsb, msb, block_desc)`** processes one raw `v4l2_rds_data` element:
- `block_desc & 0x80` = uncorrectable error → discard
- `block_desc & 0x07` = block type (0=A, 1=B, 2=C, 3=D, 4=C')
- On block D, `process_group()` decodes the assembled group

**`process_group()`** dispatches on block B's group type:
- **Group 0A/0B** (bits 15-12 = 0): segment = bits 1-0; two PS chars from block D.
  When all four segments seen, `ps_ready` is set and the string cleaned.
- **Group 2A** (bits 15-12 = 2, bit 11 = 0): A/B flag + 4-bit segment; four RT chars
  from blocks C and D. A/B change resets the RT buffer. End-of-text marker (0x0D)
  and contiguous-segment check both trigger `rt_ready`.

`rds_init()` resets all fields; called whenever the tuner changes frequency.

### `audio.c` / `audio.h`

Runs the audio pipe in a background thread and provides device enumeration and
autodetect. The implementation is split into two fully separate backend paths
selected at compile time by `HAVE_PIPEWIRE`.

**`Audio` struct (shared by both backends):**

```
device[]      — capture device / PipeWire source node name
play_device[] — playback device / PipeWire sink node name; empty = default
buffer_frames — period size hint (ALSA backend); ignored by PipeWire backend
running       — volatile; cleared by thread on exit or by caller to stop
started       — 1 if pthread_create was called (thread needs joining)
thread        — pthread handle
rate          — detected/fixed sample rate, set by thread at startup
channels      — detected/fixed channel count, set by thread at startup
errmsg[]      — last error string; empty while running normally
rec_lock      — mutex protecting rec_fn and rec_ctx
rec_fn        — recording callback; called after each captured period
rec_ctx       — opaque pointer passed to rec_fn
```

---

#### PipeWire backend (`HAVE_PIPEWIRE`)

`audio_fn` creates two `pw_stream` objects on a single `pw_thread_loop`:

| Stream | Direction | Target |
|--------|-----------|--------|
| `ncradio-cap` | `PW_DIRECTION_INPUT` | `Audio/Source` node (radio card) |
| `ncradio` | `PW_DIRECTION_OUTPUT` | `Audio/Sink` node (speakers) |

The capture stream proposes S16\_LE with an open rate range
(`SPA_POD_CHOICE_RANGE_Int(48000, 8000, 384000)`) so PipeWire selects the
source's native sample rate — no resample on the capture side. The playback
stream is connected from `pw_cap_param_changed` once the capture format is
negotiated, using the same rate, limiting total resampling to at most one hop
(source native → sink native). `a->rate` and `a->channels` are written in
`pw_cap_param_changed` when the capture format is first negotiated.

**Data path:**

```
[radio card] ─PipeWire graph─▶ pw_cap_process callback
                                    │
                                    ├─▶ spa_ringbuffer (2 MiB, lock-free)
                                    │        │
                                    │        ▼
                                    │   pw_play_process callback ──▶ [speakers]
                                    │
                                    └─▶ rec_fn(rec_ctx, pcm, frames, channels)
                                              (inside rec_lock)
```

**Ring buffer usage** — `spa_ringbuffer_get_write_index` / `spa_ringbuffer_get_read_index`
return the fill level as `int32_t` and place the absolute index in `*index`.
The `*index` value is used as the offset for `_write_data` / `_read_data`
(masked to `& (PW_RING_BYTES - 1)`) and as the base for `_write_update` /
`_read_update`.

**`param_changed` callbacks** — `pw_cap_param_changed` parses the negotiated
`spa_audio_info_raw` (rate, channels), updates `stride`, `a->rate`,
`a->channels`, calls `pw_stream_update_params` with `SPA_PARAM_Buffers`, and on
its first invocation connects the playback stream with the negotiated rate.
`pw_play_param_changed` only calls `pw_stream_update_params`. Both updates move
the stream from `PAUSED` to `STREAMING`, causing the `process` callback to start
firing.

**`audio_fn` parking loop** — after setup the thread sits in a 10 ms
`nanosleep` loop until `a->running` is cleared by `audio_stop`. The PipeWire
loop thread handles all callbacks independently.

**`audio_enum_devices`** — enumerates PipeWire `Audio/Source` nodes via a
short-lived `pw_main_loop` run, using `pw_core_sync` to signal completion.

**`audio_enum_play_devices`** — same pattern for `Audio/Sink` nodes; index 0
is always `""` / `"(default)"`.

**`audio_autodetect`** — locates the radio card via sysfs/udev (same code as
ALSA backend, no ALSA library calls), extracts the ALSA card ID, then
enumerates PipeWire `Audio/Source` nodes matching by `api.alsa.card.id`
property. Falls back to a substring match on `node.name` if the property is
absent.

---

#### ALSA backend (`!HAVE_PIPEWIRE`)

**`audio_fn` thread — startup sequence:**

```
1. snd_pcm_open(capture, device, CAPTURE)
2. snd_pcm_hw_params_any → probe rates with snd_pcm_hw_params_test_rate
     candidate list: {96000, 48000, 44100, 32000, 22050, 16000}
     first match (highest) is selected
3. Configure capture: S16_LE, stereo (fallback mono), detected rate,
     period ≈ 4096 frames
4. snd_pcm_open(playback, play_device or "default", PLAYBACK)
5. Configure playback: same format/channels/rate
6. a->rate and a->channels written (read by UI for display)
```

**`audio_fn` transfer loop:**

```
while a->running:
    snd_pcm_readi(cap, buf, period)   — blocks ~100 ms per period
    snd_pcm_writei(play, buf, n)
    snd_pcm_recover on xrun in either direction
    lock(rec_lock)
    if rec_fn: rec_fn(rec_ctx, buf, n, channels)
    unlock(rec_lock)
```

**`audio_enum_devices`** — iterates ALSA cards via `snd_card_next` /
`snd_ctl_pcm_next_device` and emits only physical capture endpoints
(`hw:CARD=<id>,DEV=N`).

**`audio_enum_play_devices`** — same iteration for playback endpoints; index 0
is always `""` / `"(default)"`.

**`audio_alsa_version()`** — returns `snd_asoundlib_version()`.

---

#### Sysfs/udev detection (shared)

The helper functions `detect_sysfs` and `detect_udev` use only POSIX
filesystem calls and the libudev API — no ALSA library calls. They are
compiled regardless of backend.

**`detect_udev`** (compiled only with `HAVE_UDEV`): opens the radio character
device via `stat` + `udev_device_new_from_devnum`, walks up to the USB device
node, then enumerates all `sound` subsystem devices and matches those sharing
the same USB ancestor. Returns `hw:CARD=<id>,DEV=0`.

**`detect_sysfs`**: resolves `/sys/class/video4linux/<radioN>` to its real path
using `chdir` + `getcwd`, strips the `video4linux/<name>` tail, then walks up
to three parent directories looking for a `sound/card*` entry.

Both helpers return a `hw:CARD=<id>,DEV=0` (or `hw:N,0`) string, which is
used directly in the ALSA backend and used to find the matching PipeWire source
node in the PipeWire backend.

---

**`audio_start`** calls `audio_stop` first (making it safe to call
repeatedly), then spawns the thread. **`audio_stop`** is idempotent and safe
to call when no thread is running.

**Thread safety** — `a->rate`, `a->channels`, and `a->errmsg` are written once
by the audio thread (in `pw_cap_param_changed` for the PipeWire backend; during
startup for the ALSA backend), then only read by the UI thread for display
(benign race on int-sized fields). `rec_fn` and `rec_ctx` are protected by
`rec_lock` (a `pthread_mutex_t` initialized in `audio_start`).

### `record.c` / `record.h`

A thin libmp3lame wrapper, compiled only when `HAVE_LAME` is defined.

**`Record` struct:**

```
lame        — lame_global_flags encoder context
fp          — open FILE* for the output .mp3
mp3buf[]    — 16 KiB staging buffer for lame output
in_channels — channel count of the PCM being fed (1 or 2)
```

**`record_open(path, in_rate, in_channels, out_rate, out_channels, bitrate, errmsg, errmsg_size)`**

Initialises lame with:
- `lame_set_in_samplerate` / `lame_set_num_channels` — from the audio thread's
  detected hardware parameters
- `lame_set_out_samplerate` — configurable; 0 or same-as-input means no
  resampling; otherwise lame resamples internally
- `lame_set_mode` — `MONO` if either `in_channels` or `out_channels` is 1,
  else `JOINT_STEREO`
- `lame_set_brate` — configurable kbps
- `lame_set_quality(5)` — good quality, low CPU overhead

Opens the output file for writing and returns a `Record *`, or NULL with an
error message in `errmsg`.

**`record_feed(r, pcm, frames)`** — called from the audio thread (inside
`rec_lock`) after each capture period:
- Stereo input: `lame_encode_buffer_interleaved`
- Mono input: `lame_encode_buffer` with the same pointer for both channels
- Writes encoded bytes directly to `r->fp`

**`record_close(r)`** — calls `lame_encode_flush` to drain the encoder's
internal lookahead buffer, writes the remaining bytes, closes the file, and
frees `r`.

### `config.c` / `config.h`

Handles `~/.ncradio.conf`. The `Config` struct holds:

```c
uint32_t freqs[MAX_PRESETS]                  — station frequencies in Hz
char     names[MAX_PRESETS][NAME_MAX_LEN+1]  — optional display name per preset
int      count                               — number of presets
uint32_t scan_step_hz                        — scan/step/seek increment (Hz)
int      signal_threshold_pct                — minimum signal strength (0-100%)
int      rds_names                           — 1 = collect RDS names during scan
int      volume                              — tuner volume 0-100 (0 = not yet saved)
uint32_t last_freq_hz                        — last tuned frequency in Hz (0 = not yet saved)
int      audio_enabled                       — 1 = start audio pipe at launch
char     audio_device[64]                    — capture device / PipeWire source node
char     audio_play_device[64]               — playback device / PipeWire sink node; empty = default
int      audio_mute_scan                     — 1 = stop audio pipe during band scan
int      audio_mute_seek                     — 1 = stop audio pipe while seeking
int      record_bitrate                      — MP3 bitrate kbps (64/96/128/192/256/320)
int      record_stereo                       — 1 = stereo output, 0 = mono
int      record_samplerate                   — output sample rate Hz (22050/44100/48000)
```

**File format:**

```
# ncradio configuration
scan_step=100000         ← integer key=value
signal_threshold=30
rds_names=1
volume=80
last_freq=98500000
audio_enabled=1
audio_device=alsa_input.hw:CARD=Si4713,DEV=0.0.analog-stereo  ← PipeWire node name
audio_mute_scan=1
audio_mute_seek=1
record_bitrate=128
record_stereo=1
record_samplerate=44100
# stations
87.90 BBC Radio 1        ← float [optional name]
98.50
```

Lines are categorised at load time:
- `#` or blank → skip
- contains `=` and first char is not a digit → settings `key=value`
  (parsed with `%39[^=]=%79[^\n]`; `atoi` applied for integer fields)
- parseable as a float in 87.5–108.0 → station (remainder of line is the name)

Defaults are seeded before reading so the struct is always valid even when the
file is absent or has no settings lines. After parsing, `settings_defaults()`
clamps numeric values to their valid ranges.

`config_save()` always writes settings before stations. The format is
backward-compatible: old ncradio versions skip `key=value` lines because
`sscanf("%lf", "scan_step=…")` fails to match.

### `radio.c` / `radio.h`

Owns the V4L2 file descriptor and the scan/seek background threads.

**`Radio` struct — key fields:**

```
fd              — open file descriptor for /dev/radioN
cap_low         — 1 if V4L2 freq unit is 62.5 Hz, else 62.5 kHz
rds_capable     — hardware advertises V4L2_TUNER_CAP_RDS
stereo          — updated from rxsubchans on every radio_get_signal() call
freq_min_hz     — device-reported lower bound of tunable range
freq_max_hz     — device-reported upper bound of tunable range
freq_hz         — current tuned frequency in Hz (unchanged while scan/seek runs)
rds             — RdsDecoder for the live (non-scan) stream

scan_step_hz    — frequency step for scan thread (copied from config before start)
scan_threshold  — raw V4L2 signal threshold 0-65535 (from config before start)
scan_rds_names  — 1 = collect RDS names (from config before start)

seeking         — volatile stop flag for seek thread
seek_started    — 1 if seek thread needs joining
seek_result_hz  — found frequency set by seek thread, or 0 if not found
seek_step_hz    — step used by seek thread
seek_threshold  — signal threshold used by seek thread
seek_fwd        — 1 = forward seek, 0 = backward

scanning        — volatile stop flag for scan thread
scan_started    — 1 if scan thread needs joining
mutex           — protects found_freqs, found_names, found_count, scan_pos_hz
```

**V4L2 frequency units** — `VIDIOC_G_TUNER` reveals whether the tuner uses
62.5 Hz units (`V4L2_TUNER_CAP_LOW`) or 62.5 kHz units. The static helpers
`hz_to_v4l2` / `v4l2_to_hz` convert transparently. `VIDIOC_G_TUNER` also
reports the device's tunable range (`tuner.rangelow` / `tuner.rangehigh`),
which is stored in `freq_min_hz` / `freq_max_hz` and used to validate manual
tune input and to clamp the restored `last_freq` on startup.

**Stereo detection** — `radio_get_signal()` calls `VIDIOC_G_TUNER` and extracts
both `tuner.signal` (→ 0-100%) and `tuner.rxsubchans & V4L2_TUNER_SUB_STEREO`
(→ `r->stereo`) in one ioctl.

**RDS streaming** — `radio_read_rds()` does a zero-timeout `poll()` + `read()`
to drain pending RDS blocks into `r->rds`. Called each 500 ms tick outside
scan/seek mode.

**`radio_set_freq()` and `radio_seek()`** call `rds_init(&r->rds)` so the live
RDS state clears when the frequency changes.

**Scan thread (`scan_fn`)** — steps every `scan_step_hz` Hz across the FM
band, dwells 120 ms at each step to let the tuner settle, reads signal, and
adds stations above threshold to `found_freqs[]`. Optionally runs
`collect_rds()` for up to 1.5 s per found station.

**Seek thread (`seek_fn`)** — same step/threshold parameters as scan, but
steps in one direction and exits on the first matching frequency. Uses 80 ms
settle time (faster than scan since only one station needs to be found). Wraps
around at the band edge and terminates after at most one full sweep.
`r->seek_result_hz` carries the result back to the main thread (0 = not found).

Both threads follow the same lifecycle: `r->scanning` / `r->seeking` is
`volatile` and serves as both a stop flag (written by main thread) and a
completion signal (written by thread on natural exit). `scan_started` /
`seek_started` tracks whether `pthread_join` is needed.

### `ncradio.c`

Contains all ncurses code, application state, and the main event loop.

**Application modes (`Mode` enum):**

```
M_NORMAL      — browsing presets, listening
M_TUNING      — user typing a frequency after 't'
M_SCANNING    — scan thread running
M_EDITING     — user renaming a preset after 'e'
M_SETTINGS    — settings panel open after 'o'
M_SEEKING     — seek thread running
M_RECORD_NAME — user typing a recording filename after 'r' (HAVE_LAME only)
M_RECORDING   — MP3 recording in progress (HAVE_LAME only)
```

**Screen layout — fixed rows on `stdscr`:**

```
Row 0          title bar
Row 1          horizontal separator
Row 2          frequency  [ST/MO]  signal bar
Row 3          volume bar  [MUTED]  [A]/[A!]  RDS PS name (right-aligned)
Row 4          mode info line:
                 M_NORMAL/M_SETTINGS  — timed status message, then RDS RT
                 M_TUNING             — "Tune (MHz): ___"
                 M_EDITING            — "Name: ___"
                 M_SCANNING           — scan progress bar
                 M_SEEKING            — "Seeking forward >" / "Seeking backward <"
                 M_RECORD_NAME        — "Record file: ___"
                 M_RECORDING          — "- REC 0:00  filename" (red, elapsed time)
Row 5          horizontal separator
Rows 6…LINES-4 list area:
                 M_NORMAL/M_TUNING/M_EDITING/M_SEEKING — preset grid
                 M_SCANNING                            — found-stations list
                 M_SETTINGS                            — settings panel
LINES-3        horizontal separator
LINES-2        help line (context-sensitive)
LINES-1        second help line (M_NORMAL only)
```

**Audio indicator** (row 3):
- `[A]` green — audio pipe is running
- `[A!]` red — pipe stopped with an error (error text visible in settings panel)
- absent — audio disabled in config

**Settings panel** (`draw_settings()`) — seven rows when only audio is compiled
in; ten rows when both audio and lame are present:

```
Row +2   Scan step:            0.10 MHz         <- -> to cycle
Row +3   Signal threshold:     30%              <- -> to adjust (5% steps)
Row +4   Save RDS names:       Yes              <- -> or Enter to toggle
Row +5   Audio output:         On (48000Hz 2ch) <- -> or Enter to toggle
Row +6   Audio device:         <node name>      <- -> to cycle  <description>
Row +7   Mute while scanning:  Yes              <- -> or Enter to toggle
Row +8   Mute while seeking:   Yes              <- -> or Enter to toggle
Row +9   Record bitrate:       128 kbps         <- -> to cycle        ← HAVE_LAME
Row +10  Record channels:      Stereo           <- -> to toggle       ← HAVE_LAME
Row +11  Record sample rate:   44100 Hz         <- -> to cycle        ← HAVE_LAME
```

**`audio_apply()`** — called on startup and whenever audio settings change:

1. If `audio_enabled` is off → `audio_stop` and return.
2. If `audio_device` is empty → call `audio_autodetect(radio_dev_path, …)`.
   On success, save the detected device to config. On failure, return without
   starting (audio stays off).
3. Call `audio_start(audio_device)`.

**Startup sequence** (`main()`):

1. `radio_open` — opens the V4L2 device, reads tunable range and capabilities.
2. `config_load` — reads `~/.ncradio.conf`; applies defaults for missing keys.
3. Restore volume: if `config.volume > 0`, call `radio_set_volume`.
4. Restore frequency: if `config.last_freq_hz` is within the device's reported
   range, call `radio_set_freq`.
5. Enumerate audio capture and playback devices for the settings panel.
6. Auto-enable audio: if `audio_enabled == 0` and `audio_device` is empty
   (i.e. no explicit prior choice), call `audio_autodetect`. If a device is
   found, set `audio_enabled = 1`, save both fields, and proceed.
7. `audio_apply` — start the pipe if enabled.

**Recording** (`M_RECORD_NAME` / `M_RECORDING`, HAVE_LAME only):

- `r` in `M_NORMAL`: if `audio.running`, enter `M_RECORD_NAME`; otherwise show
  "Enable audio first".
- `M_RECORD_NAME`: text entry for the output path (`.mp3` auto-appended). On
  Enter, `record_open` is called with `audio.rate` / `audio.channels` as input
  parameters and `config.record_*` as output parameters. On success, the
  `Record *` is installed as `audio.rec_ctx` and `recording_cb` as `audio.rec_fn`
  inside `audio.rec_lock`, then mode transitions to `M_RECORDING`.
- `M_RECORDING`: all keys ignored except `s`/`Esc` (stop) and `q` (stop + quit).
  `recording_stop` locks `rec_lock`, grabs the `Record *`, clears both fields,
  unlocks, then calls `record_close` — guaranteeing the audio thread is not
  mid-callback when `record_close` frees the encoder.

**`-v` / `--version`** — `print_version()` is called before `radio_open` if
either flag appears in `argv`. It prints the version string, build timestamp,
and one line per optional component:
- PipeWire build: `audio_pipewire_version()` (runtime `pw_get_library_version()`)
- ALSA build: `audio_alsa_version()` (runtime `snd_asoundlib_version()`)
- udev: `LIBUDEV_VERSION` macro embedded at compile time by `configure`
- lame: `get_lame_version()` (runtime)

**Exit sequence** (`main()` after the event loop):

1. If still in scan mode, `finish_scan`.
2. `radio_mute(1)` — silence the tuner before teardown.
3. Save `config.volume = radio.volume` and `config.last_freq_hz = radio.freq_hz`.
4. `config_save`.
5. `endwin`, `audio_stop`, `radio_close`.

**Preset grid layout** (`preset_layout()` in `draw_presets()`):

Each cell is `ENTRY_BASE` (13) chars:
```
marker(1) + index(2) + dot(1) + space(1) + freq-6chars(6) + space(1) + cur-mark(1)
```
Column count and name-display width are computed from terminal width and the
longest name in the list. Column-major ordering: item `i` is at visual row
`i % rows_per_col`, column `i / rows_per_col`. `preset_rows_per_col` is
written by `draw_presets()` and used by `handle_key()` for `←`/`→` column
jumps.

**Signal and RDS polling** — every 500 ms outside scan/seek mode,
`radio_get_signal()` and `radio_read_rds()` update `signal_pct`, `radio.stereo`,
and `radio.rds`.

**Thread safety** — neither the scan, seek, nor audio threads call ncurses.
`radio.scanning`, `radio.seeking`, and `audio.running` are `volatile` and read
without locks (single-word). Multi-field scan state is protected by
`radio.mutex`. Audio thread writes `audio.rate`, `audio.channels`, and
`audio.errmsg` once during startup; the UI reads them for display only (benign
race).

## Data flow

```
 keyboard
    │
    ▼
 handle_key()
    │
    ├─ radio_set_freq(freq ± scan_step_hz)     ← , / . keys
    │       └─ ioctl(VIDIOC_S_FREQUENCY) + rds_init()
    │
    ├─ radio_start_seek(fwd, step, threshold)  ← < / > keys
    │       └─ pthread_create → seek_fn
    │                   └─ VIDIOC_S_FREQUENCY + VIDIOC_G_TUNER per step
    │                      → seek_result_hz
    │
    ├─ radio_start_scan                        ← s key
    │       └─ pthread_create → scan_fn
    │                   ├─ VIDIOC_S_FREQUENCY + VIDIOC_G_TUNER per step
    │                   └─ collect_rds → read(fd) → rds_feed → found_names[]
    │
    ├─ audio_apply / audio_start / audio_stop  ← settings o key
    │       └─ pthread_create → audio_fn
    │
    │  [PipeWire build]
    │       audio_fn ──▶ pw_thread_loop (separate thread)
    │                        ├─ pw_stream (capture) ←── radio Audio/Source node
    │                        │        └─ pw_cap_process: PCM → ring buffer + rec_fn
    │                        └─ pw_stream (playback) ───▶ speaker Audio/Sink node
    │                                 └─ pw_play_process: ring buffer → PCM
    │
    │  [ALSA build]
    │       audio_fn loop:
    │                   ├─ snd_pcm_open(hw:CARD=x,DEV=0, CAPTURE)
    │                   ├─ probe rate, configure S16_LE
    │                   ├─ snd_pcm_open("default", PLAYBACK)
    │                   └─ snd_pcm_readi → snd_pcm_writei → rec_fn(rec_ctx) loop
    │
    ├─ record_open → audio.rec_fn/rec_ctx set ← r key (HAVE_LAME)
    │       └─ lame_init → lame_init_params → fopen(path)
    │          [audio thread calls record_feed each period]
    │
    ├─ recording_stop                         ← s/Esc/q in M_RECORDING
    │       └─ lock rec_lock, clear rec_fn/ctx, unlock → record_close
    │                   └─ lame_encode_flush → fclose
    │
    ├─ config_add / config_del / config_save   → ~/.ncradio.conf
    │
    └─ radio_set_freq / radio_mute / radio_set_volume
            └─ ioctl(fd, VIDIOC_…) → /dev/radioN

 main loop tick (250 ms)
    ├─ radio_get_signal  → VIDIOC_G_TUNER  → signal_pct, radio.stereo
    ├─ radio_read_rds    → poll+read(fd)   → rds_feed → radio.rds
    ├─ seek completion   → radio_stop_seek + radio_set_freq(result)
    ├─ scan completion   → finish_scan
    └─ draw_all          → ncurses / terminal
```

## Build

`configure` probes for `libpipewire-0.3` first (preferred backend), then falls
back to `libasound` if PipeWire is absent or `--disable-pipewire` is passed. It
also probes for `libudev` (device autodetection) and `libmp3lame` (recording)
independently of the audio backend. It writes `config.mk` with `AUDIO_CFLAGS`,
`AUDIO_SRCS`, `AUDIO_LIBS`, and `RECORD_SRCS`.

| Flag | Effect |
|------|--------|
| `--disable-pipewire` | Skip PipeWire probe; use ALSA |
| `--disable-udev` | Skip libudev probe; sysfs-only autodetection |
| `--disable-lame` | Skip libmp3lame probe; no recording support |

With PipeWire: `HAVE_PIPEWIRE` is defined, `-lpipewire-0.3` is linked, no
`-lasound`.

With ALSA: `HAVE_AUDIO` is defined (without `HAVE_PIPEWIRE`), `-lasound` is
linked.

When lame is found, `HAVE_LAME` is added to `AUDIO_CFLAGS`, `-lmp3lame` to
`AUDIO_LIBS`, and `record.c` to `RECORD_SRCS`.

When udev is found, `configure` runs `pkg-config --modversion libudev` and, if
successful, appends `-DLIBUDEV_VERSION=\"<ver>\"` to `AUDIO_CFLAGS` (the only
component with no runtime version query API).

Compiled with `-Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L`. Linked against
`-lncurses -lpthread` plus the detected audio and optional libraries.
