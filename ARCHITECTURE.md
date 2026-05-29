# Architecture

## Source layout

```
ncradio/
├── ncradio.c   — ncurses UI and application main loop
├── radio.c     — V4L2 hardware interface and scan/seek threads
├── radio.h     — Radio struct and public function declarations
├── rds.c       — RDS block decoder (PS name, Radio Text)
├── rds.h       — RdsDecoder struct and rds_feed() declaration
├── audio.c     — ALSA audio pipe (capture → playback thread)
├── audio.h     — Audio struct and public function declarations
├── config.c    — ~/.ncradio.conf read/write
├── config.h    — Config struct and function declarations
└── Makefile
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

An ALSA-based audio pipe that runs in a background thread. It is entirely
independent of the V4L2 radio module; the two interact only through
`config.audio_device` and `config.audio_enabled`.

**`Audio` struct:**

```
device[]    — ALSA capture device name (e.g. "hw:2,0")
running     — volatile; cleared by thread on exit or by caller to stop
started     — 1 if pthread_create was called (thread needs joining)
thread      — pthread handle
rate        — detected sample rate, set by thread after hardware probe
channels    — detected channel count (1 or 2), set by thread
errmsg[]    — last error string; empty while running normally
```

**`audio_fn` thread — startup sequence:**

```
1. snd_pcm_open(capture, device, CAPTURE)
2. snd_pcm_hw_params_any → probe rates with snd_pcm_hw_params_test_rate
     candidate list: {96000, 48000, 44100, 32000, 22050, 16000}
     first match (highest) is selected
3. Configure capture: S16_LE, stereo (fallback mono), detected rate,
     period ≈ 4096 frames
4. snd_pcm_open(playback, "default", PLAYBACK)
5. Configure playback: same format/channels/rate
     → "default" routes to PulseAudio/PipeWire/ALSA hw as configured
6. a->rate and a->channels written (read by UI for display)
```

**`audio_fn` transfer loop:**

```
while a->running:
    snd_pcm_wait(cap, 100ms)   — yields CPU; allows stop-flag check on timeout
    snd_pcm_readi(cap, buf, period)
    snd_pcm_writei(play, buf, n)
    snd_pcm_recover on xrun in either direction
```

The 100 ms `snd_pcm_wait` timeout means `audio_stop()` (which sets
`a->running = 0` then `pthread_join`) returns within ~100 ms of being called,
regardless of the hardware period size.

**`audio_enum_devices`** calls `snd_device_name_hint(-1, "pcm", ...)` and
filters to entries where `IOID` is `"Input"` (or absent, meaning both
directions) **and** the device name starts with `"hw:"`. This excludes virtual
ALSA plugins (`default`, `dmix`, `null`, `plug*`, etc.) and gives only
physical capture endpoints.

**`audio_start`** calls `audio_stop` first (making it safe to call repeatedly),
then spawns the thread. **`audio_stop`** is idempotent and safe to call when no
thread is running.

**Thread safety** — `a->rate` and `a->channels` are written once by the thread
during startup, then only read by the UI thread for display. The benign data
race on these `int`-sized fields is acceptable; they are informational only.
`a->errmsg` follows the same pattern.

### `config.c` / `config.h`

Handles `~/.ncradio.conf`. The `Config` struct holds:

```c
uint32_t freqs[MAX_PRESETS]                  — station frequencies in Hz
char     names[MAX_PRESETS][NAME_MAX_LEN+1]  — optional display name per preset
int      count                               — number of presets
uint32_t scan_step_hz                        — scan/step/seek increment (Hz)
int      signal_threshold_pct                — minimum signal strength (0-100%)
int      rds_names                           — 1 = collect RDS names during scan
int      audio_enabled                       — 1 = start audio pipe at launch
char     audio_device[64]                    — ALSA capture device (e.g. "hw:2,0")
```

**File format:**

```
# ncradio configuration
scan_step=100000         ← integer key=value
signal_threshold=30
rds_names=1
audio_enabled=1
audio_device=hw:2,0      ← string key=value
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
`hz_to_v4l2` / `v4l2_to_hz` convert transparently.

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
M_NORMAL    — browsing presets, listening
M_TUNING    — user typing a frequency after 't'
M_SCANNING  — scan thread running
M_EDITING   — user renaming a preset after 'e'
M_SETTINGS  — settings panel open after 'o'
M_SEEKING   — seek thread running
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

**Settings panel** (`draw_settings()`) — five rows in the list area:

```
Row +2   Scan step:          0.10 MHz    <- -> to cycle
Row +3   Signal threshold:   30%         <- -> to adjust (5% steps)
Row +4   Save RDS names:     Yes         <- -> or Enter to toggle
Row +5   Audio output:       On (48000Hz 2ch)  <- -> or Enter to toggle
Row +6   Audio device:       hw:2,0      <- -> to cycle  <description>
```

Changing **Audio output** calls `audio_apply()` which starts or stops the
`Audio` thread immediately. Changing **Audio device** while audio is on calls
`audio_start()` directly (which calls `audio_stop()` first), so the pipe
restarts on the new device without any manual toggle.

`audio_apply()` also handles the first-run case: if audio is enabled but no
device is configured, it auto-selects `audio_dev_names[0]` and saves it.

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
    │                   ├─ snd_pcm_open(hw:X,Y, CAPTURE)
    │                   ├─ probe rate, configure S16_LE
    │                   ├─ snd_pcm_open("default", PLAYBACK)
    │                   └─ snd_pcm_wait → snd_pcm_readi → snd_pcm_writei loop
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

A single `Makefile` with explicit per-object dependencies. Compiled with
`-Wall -Wextra -O2`. Linked against `-lncurses -lpthread -lasound`.
