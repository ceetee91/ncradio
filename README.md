# ncradio

A small ncurses FM radio controller for Linux, built for V4L2-compatible tuner
cards and USB radio sticks accessible as `/dev/radio0`.

```
              ncradio v0.1
──────────────────────────────────────────────────────────────
 Freq:  98.50 MHz    Signal:[||||||||||....] 71%
 Vol:   [||||||||||||||] 80%
 Scan done — found 8 stations, saved to ~/.ncradio.conf
──────────────────────────────────────────────────────────────
 Presets (8):
    1.   87.90 MHz
    2.   91.30 MHz
  > 3.   98.50 MHz  <
    4.  101.00 MHz
    5.  103.60 MHz
──────────────────────────────────────────────────────────────
 s:scan  ,:seek<  .:seek>  t:tune  m:mute  +/-:vol  a:add  d:del
 Up/Dn:select preset   Enter:tune to preset   q:quit
```

## Requirements

- Linux kernel with V4L2 radio support (`/dev/radio0`)
- `ncurses` development headers (`ncurses-devel` / `libncurses-dev`)
- GCC and GNU make

## Build

```sh
make
```

Optional install to `/usr/local/bin`:

```sh
sudo make install
```

## Usage

```sh
./ncradio              # uses /dev/radio0
./ncradio /dev/radio1  # alternate device
```

The tuner device must be readable and writable by the current user. On most
distributions, add yourself to the `video` group:

```sh
sudo usermod -aG video $USER
```

## Key Bindings

### Normal mode

| Key | Action |
|-----|--------|
| `s` | Full band scan (87.50 – 108.00 MHz) |
| `,` or `←` | Seek to previous station (hardware seek) |
| `.` or `→` | Seek to next station (hardware seek) |
| `t` | Manual tune — type a frequency in MHz, then Enter |
| `+` or `=` | Volume up (5% step) |
| `-` | Volume down (5% step) |
| `m` | Toggle mute |
| `a` | Add current frequency to presets |
| `d` | Delete the highlighted preset |
| `↑` / `↓` | Move selection up / down in preset list |
| `PgUp` / `PgDn` | Scroll preset list by one page |
| `Enter` | Tune to the highlighted preset |
| `q` | Quit |

### Tuning mode (after pressing `t`)

| Key | Action |
|-----|--------|
| `0`–`9`, `.` | Enter frequency digits |
| `Backspace` | Delete last digit |
| `Enter` | Confirm and tune |
| `Esc` | Cancel |

### Scan mode

| Key | Action |
|-----|--------|
| `s` or `Esc` | Stop scan early, save results, return to normal |
| `q` | Stop scan, save results, quit |

## Scanning

Pressing `s` starts a full band sweep from 87.50 to 108.00 MHz in 100 kHz
steps. The scan runs in a background thread so the UI stays responsive. A
progress bar and a live list of found stations are shown during the sweep.

When the scan finishes (or is stopped), all found stations **replace** the
current preset list and are saved to `~/.ncradio.conf`. The tuner is then
restored to the frequency it was on before the scan.

A full sweep takes roughly 25–30 seconds depending on hardware settling time.

## Configuration file

Presets are stored in `~/.ncradio.conf` — one frequency per line in MHz:

```
# ncradio stations
87.90
91.30
98.50
101.00
103.60
```

Lines starting with `#` are ignored. The file is rewritten in sorted order
every time a preset is added, deleted, or a scan completes. You can also edit
it by hand; ncradio reads it at startup.

## Hardware notes

ncradio uses the following V4L2 ioctls:

| ioctl | Purpose |
|-------|---------|
| `VIDIOC_G_TUNER` | Detect frequency unit (`cap_low`) and read signal strength |
| `VIDIOC_G_FREQUENCY` / `VIDIOC_S_FREQUENCY` | Get / set tuner frequency |
| `VIDIOC_S_HW_FREQ_SEEK` | Hardware-assisted station seek |
| `VIDIOC_S_CTRL` | Set volume (`V4L2_CID_AUDIO_VOLUME`) and mute (`V4L2_CID_AUDIO_MUTE`) |

Seek (`VIDIOC_S_HW_FREQ_SEEK`) is a blocking call; it may take up to a second
or two on some hardware. If the tuner does not support hardware seek the ioctl
returns an error and the frequency is unchanged.
