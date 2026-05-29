# Architecture

## Source layout

```
ncradio/
├── ncradio.c   — ncurses UI and application main loop
├── radio.c     — V4L2 hardware interface
├── radio.h     — Radio struct and function declarations
├── config.c    — ~/.ncradio.conf read/write
├── config.h    — Config struct and function declarations
└── Makefile
```

## Module responsibilities

### `radio.c` / `radio.h`

Owns everything below the UI: the open file descriptor to `/dev/radioN`, all
V4L2 ioctl calls, and the scan background thread.

**`Radio` struct** (defined in `radio.h`):

```
Radio {
    fd          — open file descriptor for /dev/radioN
    cap_low     — 1 if V4L2 freq unit is 62.5 Hz, 0 if 62.5 kHz
    freq_hz     — current tuned frequency in Hz
                  (unchanged while scan thread runs, so it can be
                   restored afterwards)
    volume      — 0-100 mirror of what was sent to hardware
    muted       — mute state mirror

    scanning    — volatile flag: 1 while scan thread is alive;
                  main thread writes 0 to request abort;
                  thread writes 0 on natural completion
    scan_started — 1 if pthread_create was called (needs join)
    scan_thread — pthread handle
    mutex       — protects found_freqs, found_count, scan_pos_hz
    found_freqs — array of detected station frequencies (Hz)
    found_count — number of entries in found_freqs
    scan_pos_hz — frequency currently being probed (for UI progress)
}
```

**V4L2 frequency units** — the kernel represents FM frequencies in units of
either 62.5 Hz (`V4L2_TUNER_CAP_LOW` set) or 62.5 kHz (cap_low clear).
`radio_open` detects this via `VIDIOC_G_TUNER` and stores it in `cap_low`. All
public functions accept and return plain Hz; the two static helpers
`hz_to_v4l2` / `v4l2_to_hz` do the conversion.

**Scan thread** (`scan_fn`) — steps every 100 kHz from 87.50 to 108.00 MHz.
At each step it:
1. Writes the frequency via `VIDIOC_S_FREQUENCY`.
2. Sleeps 120 ms for the tuner to settle.
3. Reads `tuner.signal` via `VIDIOC_G_TUNER`.
4. If signal ≥ `SCAN_SIGNAL_THRESH` (20000/65535 ≈ 30%), appends the
   frequency to `found_freqs` (mutex-protected) and dwells 200 ms.

The thread sets `scanning = 0` when it finishes or is asked to stop. The main
thread calls `radio_stop_scan` (which does `pthread_join`) to collect the
result safely.

### `config.c` / `config.h`

Plain text file handling for `~/.ncradio.conf`. The format is one MHz value
per line (e.g. `98.50`), with `#` comments ignored. All functions operate on
the `Config` struct which holds up to 64 frequencies as `uint32_t` Hz values.

`config_add` inserts in sorted order and is a no-op if the frequency is
already present. `config_save` rewrites the entire file atomically (standard
`fopen`/`fclose`).

### `ncradio.c`

Contains all ncurses code and the application state machine.

**Application state** — three modes defined as `Mode`:

```
M_NORMAL   — browsing presets, listening
M_TUNING   — user is typing a frequency after pressing 't'
M_SCANNING — scan thread is running
```

**Screen layout** — fixed row assignments on `stdscr`:

```
Row 0   title bar
Row 1   horizontal separator
Row 2   frequency + signal bar
Row 3   volume bar + MUTED indicator
Row 4   mode-specific info line:
          M_NORMAL   — timed status message (3 s) or blank
          M_TUNING   — "Tune (MHz): ___"
          M_SCANNING — "Scanning XX.XX MHz  Found: N  [====] XX%"
Row 5   horizontal separator
Row 6…LINES-4  preset list / scan results area
LINES-3 horizontal separator
LINES-2 help line (context-sensitive)
LINES-1 second help line (M_NORMAL only)
```

The layout requires a minimum terminal height of ~14 rows; standard 24×80 gives
13 visible preset rows.

**Redraw loop** — `draw_all()` calls `erase()` then redraws every widget on
`stdscr` before calling `refresh()`. ncurses performs differential updates so
only changed cells are written to the terminal. The main loop ticks every
250 ms via `timeout(250)` on `getch()`.

**Signal strength polling** — updated every 500 ms in `M_NORMAL` mode via
`gettimeofday`. Not polled during scanning (the tuner is being swept) or
tuning.

**Scan completion** — the main loop checks `!radio.scanning && radio.scan_started`
each tick. When that condition becomes true, `finish_scan(0)` is called:

1. `radio_stop_scan` joins the thread.
2. `found_freqs` is copied into `config.freqs` under the mutex.
3. `config_save` writes `~/.ncradio.conf`.
4. `radio_set_freq` restores `radio.freq_hz` (the pre-scan frequency) to the
   hardware.
5. Mode returns to `M_NORMAL`.

**Thread safety** — the scan thread never calls ncurses. The main thread reads
`radio.scanning` (volatile, no lock needed for a single-byte check) and reads
`found_freqs`/`found_count`/`scan_pos_hz` only while holding `radio.mutex`.

## Data flow

```
 keyboard
    │
    ▼
 handle_key()
    │
    ├─ radio_set_freq / radio_seek / radio_mute / radio_set_volume
    │       │
    │       └─ ioctl(fd, VIDIOC_…)  ──►  /dev/radioN
    │
    ├─ config_add / config_del / config_save  ──►  ~/.ncradio.conf
    │
    └─ radio_start_scan
            │
            └─ pthread_create ──► scan_fn (background)
                                       │
                                       ├─ ioctl VIDIOC_S_FREQUENCY (each step)
                                       ├─ ioctl VIDIOC_G_TUNER (signal check)
                                       └─ radio.found_freqs[] (mutex-protected)

 main loop tick (250 ms)
    ├─ radio_get_signal ──► ioctl VIDIOC_G_TUNER
    ├─ finish_scan (on completion detection)
    └─ draw_all ──► ncurses / terminal
```

## Build

A single `Makefile` with explicit per-object dependencies. Compiled with
`-Wall -Wextra -O2`. Linked against `-lncurses -lpthread`. No external
libraries beyond the standard C library, ncurses, and pthreads.
