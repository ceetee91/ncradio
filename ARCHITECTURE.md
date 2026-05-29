# Architecture

## Source layout

```
ncradio/
├── ncradio.c   — ncurses UI and application main loop
├── radio.c     — V4L2 hardware interface and scan thread
├── radio.h     — Radio struct and public function declarations
├── rds.c       — RDS block decoder (PS name, Radio Text)
├── rds.h       — RdsDecoder struct and rds_feed() declaration
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
b_ok/c_ok/d_ok  — clear if block had an uncorrectable error
ps[]            — 8-char Program Service name (station name), null-terminated
rt[]            — 64-char Radio Text, null-terminated
ps_ready        — 1 when all 4 PS segments have been received
rt_ready        — 1 when a contiguous run of RT segments has been received
ps_seg_seen     — 4-bit mask: which of the four PS segments (0-3) arrived
rt_seg_seen     — 16-bit mask: which of the 16 RT segments arrived
rt_ab           — current RT A/B flag; A/B toggle signals a new RT message
```

**`rds_feed(d, lsb, msb, block_desc)`** — processes one raw `v4l2_rds_data` element:
- `block_desc & 0x80` = uncorrectable error → silently discard
- `block_desc & 0x07` = block type (0=A, 1=B, 2=C, 3=D, 4=C')
- Words are assembled as `(msb<<8)|lsb`
- On block D, `process_group()` is called

**`process_group()`** — dispatches on the group type encoded in block B:
- **Group 0A/0B** (bits 15-12 = 0): segment = bits 1-0 of B; two PS chars from block D.
  When all four segments are seen, `ps_ready` is set and the string is cleaned
  (non-printable → space, trailing spaces removed).
- **Group 2A** (bits 15-12 = 2, bit 11 = 0): A/B flag + 4-bit segment; four RT chars
  from blocks C and D. A/B flag change resets the RT buffer. The end-of-text
  marker (0x0D) and a contiguous-segment check both trigger `rt_ready`.

`rds_init()` resets all fields and must be called when the tuner changes frequency.

### `config.c` / `config.h`

Handles `~/.ncradio.conf`. The `Config` struct holds:

```c
uint32_t freqs[MAX_PRESETS]              — station frequencies in Hz
char     names[MAX_PRESETS][NAME_MAX_LEN+1] — optional display name per preset
int      count                           — number of presets
uint32_t scan_step_hz                    — scan frequency increment (Hz)
int      signal_threshold_pct            — minimum signal strength (0-100%)
int      rds_names                       — 1 = collect RDS names during scan
```

**File format:**

```
# ncradio configuration
scan_step=100000         ← key=value settings (first char not a digit)
signal_threshold=30
rds_names=1
# stations
87.90 BBC Radio 1        ← float [optional name]
98.50
```

Lines are categorised at load time:
- `#` or blank → skip
- contains `=` and first char is not a digit → settings key=value
- parseable as a float in 87.5–108.0 → station (remainder of line = name)

Defaults (`DEFAULT_SCAN_STEP_HZ`, `DEFAULT_SIGNAL_THRESH_PCT`, `DEFAULT_RDS_NAMES`)
are seeded before reading so the struct is always valid even if the file is
absent or has no settings lines. After parsing, `settings_defaults()` clamps
values to their valid ranges.

`config_save()` always writes settings before stations. The format is
backward-compatible: old ncradio versions skip `key=value` lines because
`sscanf("%lf", ...)` fails on them.

### `radio.c` / `radio.h`

Owns the V4L2 file descriptor and the scan background thread.

**`Radio` struct — key fields:**

```
fd              — open file descriptor for /dev/radioN
cap_low         — 1 if V4L2 freq unit is 62.5 Hz, else 62.5 kHz
rds_capable     — hardware advertises V4L2_TUNER_CAP_RDS
stereo          — updated from rxsubchans on every radio_get_signal() call
freq_hz         — current tuned frequency in Hz (unchanged while scan runs)
rds             — RdsDecoder for the live (non-scan) stream

scan_step_hz    — frequency step for the scan thread (set from config before start)
scan_threshold  — raw V4L2 signal threshold for the scan thread (0-65535)
scan_rds_names  — 1 = collect RDS names during scan

scanning        — volatile; cleared by thread on completion or by caller to abort
scan_started    — 1 if pthread_create was called (thread needs joining)
mutex           — protects found_freqs, found_names, found_count, scan_pos_hz
```

**V4L2 frequency units** — `VIDIOC_G_TUNER` reveals whether the tuner uses
62.5 Hz units (`V4L2_TUNER_CAP_LOW`) or 62.5 kHz units. The two static helpers
`hz_to_v4l2` / `v4l2_to_hz` do the conversion transparently.

**Stereo detection** — `radio_get_signal()` calls `VIDIOC_G_TUNER` and extracts
both `tuner.signal` (→ 0-100%) and `tuner.rxsubchans & V4L2_TUNER_SUB_STEREO`
(→ `r->stereo`) in one ioctl.

**RDS streaming** — `radio_read_rds()` does a zero-timeout `poll()` followed by
`read()` to drain any pending RDS blocks into `r->rds`. Called from the main
loop every 500 ms tick. The static `drain_rds()` helper is also reused inside
`collect_rds()` for scan-time RDS collection.

**`radio_set_freq()` and `radio_seek()`** both call `rds_init(&r->rds)` after a
successful ioctl so the live RDS state is cleared when the frequency changes.

**Scan thread (`scan_fn`):**

```
1. Resolve effective step/threshold/rds_names from scan_* fields
   (fall back to FREQ_STEP_HZ / SCAN_SIGNAL_THRESH / 1 if zero)
2. For each frequency from FREQ_MIN_HZ to FREQ_MAX_HZ in step increments:
   a. Set frequency via VIDIOC_S_FREQUENCY
   b. Sleep 120 ms (tuner settling time)
   c. Read signal via VIDIOC_G_TUNER
   d. If signal < threshold: continue
   e. Lock mutex, append to found_freqs[] with empty name, unlock
   f. If rds_capable && scan_rds_names:
        collect_rds() for up to 1500 ms (exits early on ps_ready)
        Lock mutex, store PS name in found_names[], unlock
      Else: msleep(200)
3. Set r->scanning = 0 to signal natural completion
```

`collect_rds()` polls the fd in 50 ms increments, checking `r->scanning` each
time to support early abort.

`radio_stop_scan()` sets `r->scanning = 0` and calls `pthread_join`. Safe to
call even after the thread finished naturally (join returns immediately).

### `ncradio.c`

Contains all ncurses code, application state, and the main event loop.

**Application modes (`Mode` enum):**

```
M_NORMAL    — browsing presets, listening
M_TUNING    — user typing a frequency after 't'
M_SCANNING  — scan thread running
M_EDITING   — user renaming a preset after 'e'
M_SETTINGS  — settings panel open after 'o'
```

**Screen layout — fixed rows on `stdscr`:**

```
Row 0          title bar
Row 1          horizontal separator
Row 2          frequency  [ST/MO]  signal bar
Row 3          volume bar  [MUTED]  RDS PS name (right-aligned)
Row 4          mode info line:
                 M_NORMAL/M_SETTINGS  — status message (3 s) or RDS RT
                 M_TUNING             — "Tune (MHz): ___"
                 M_EDITING            — "Name: ___"
                 M_SCANNING           — "Scanning XX.XX MHz  Found: N  [====] XX%"
Row 5          horizontal separator
Rows 6…LINES-4 list area:
                 M_NORMAL/M_TUNING/M_EDITING — preset list
                 M_SCANNING                  — found-stations list
                 M_SETTINGS                  — settings panel
LINES-3        horizontal separator
LINES-2        help line (context-sensitive)
LINES-1        second help line (M_NORMAL only)
```

Requires a minimum terminal height of ~14 rows; standard 24×80 gives 13
visible preset rows or settings rows.

**Settings panel** — displayed when mode is M_SETTINGS, replacing the preset
list in the list area. Three rows, one per setting:

```
> Scan step:            0.10 MHz     <- -> to cycle
  Signal threshold:     30%          <- -> to adjust (5% steps)
  Save RDS names:       Yes          <- -> or Enter to toggle
```

Scan step cycles through fixed options `{25000, 50000, 100000, 200000}` Hz.
Signal threshold steps ±5 percentage points, clamped to 5-95%. RDS names
toggles between 0 and 1. Every keystroke calls `config_save()` immediately.

**Scan parameters** — when `s` is pressed, the current config values are
copied into the Radio struct before `radio_start_scan()` is called:

```c
radio.scan_step_hz   = config.scan_step_hz;
radio.scan_threshold = (config.signal_threshold_pct * 65535) / 100;
radio.scan_rds_names = config.rds_names;
```

**Redraw loop** — `draw_all()` calls `erase()` then redraws every widget before
`refresh()`. ncurses performs differential updates. The loop ticks every 250 ms
via `timeout(250)` on `getch()`.

**Signal and RDS polling** — every 500 ms (outside scan mode), the main loop
calls `radio_get_signal()` (updates `signal_pct` and `radio.stereo`) and
`radio_read_rds()` (drains pending RDS blocks into `radio.rds`).

**Thread safety** — the scan thread never calls ncurses. `radio.scanning` is
`volatile` and checked without a lock (single-byte read). All multi-field scan
state (`found_freqs`, `found_names`, `found_count`, `scan_pos_hz`) is read
under `radio.mutex`.

## Data flow

```
 keyboard
    │
    ▼
 handle_key()
    │
    ├─ radio_set_freq / radio_seek / radio_mute / radio_set_volume
    │       └─ ioctl(fd, VIDIOC_…)  ──►  /dev/radioN
    │
    ├─ config_add / config_del / config_save  ──►  ~/.ncradio.conf
    │
    ├─ handle_settings_key → config fields + config_save
    │
    └─ radio_start_scan (after copying scan params into Radio)
            └─ pthread_create → scan_fn
                    ├─ VIDIOC_S_FREQUENCY + VIDIOC_G_TUNER (per step)
                    └─ collect_rds → read(fd) → rds_feed → found_names[]

 main loop tick (250 ms)
    ├─ radio_get_signal  → VIDIOC_G_TUNER  →  signal_pct, radio.stereo
    ├─ radio_read_rds    → poll + read(fd) →  rds_feed → radio.rds
    ├─ finish_scan (on natural completion detection)
    └─ draw_all → ncurses / terminal
```

## Build

A single `Makefile` with explicit per-object dependencies. Compiled with
`-Wall -Wextra -O2`. Linked against `-lncurses -lpthread`. No external
libraries beyond the standard C library, ncurses, and pthreads.
