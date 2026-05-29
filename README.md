# ncradio

A small ncurses FM radio controller for Linux, built for V4L2-compatible tuner
cards and USB radio sticks accessible as `/dev/radio0`.

```
              ncradio v0.1
──────────────────────────────────────────────────────────────────
 Freq:  98.50 MHz  [ST]    Signal:[||||||||||....] 71%
 Vol:   [||||||||||||||] 80%                      Capital FM
 Coldplay - The Scientist                                        ← RDS radio text
──────────────────────────────────────────────────────────────────
 Presets (5):
    1.   87.90 MHz  BBC Radio 1
    2.   91.30 MHz
  > 3.   98.50 MHz  Capital FM       <
    4.  103.60 MHz  LBC
    5.  107.30 MHz
──────────────────────────────────────────────────────────────────
 s:scan  ,:seek<  .:seek>  t:tune  m:mute  +/-:vol  a:add  d:del  e:rename  o:settings
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
| `e` | Rename the highlighted preset |
| `o` | Open settings panel |
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

### Rename mode (after pressing `e`)

| Key | Action |
|-----|--------|
| Printable chars | Append to name (max 32 characters) |
| `Backspace` | Delete last character |
| `Enter` | Save name |
| `Esc` | Cancel without saving |

### Scan mode

| Key | Action |
|-----|--------|
| `s` or `Esc` | Stop scan early, save results, return to normal |
| `q` | Stop scan, save results, quit |

### Settings panel (after pressing `o`)

| Key | Action |
|-----|--------|
| `↑` / `↓` | Select setting |
| `←` / `→` | Adjust value |
| `Enter` | Toggle (Save RDS names only) |
| `Esc` or `o` | Close settings |

## Status display

### Stereo / mono

`[ST]` (green) is shown next to the frequency when the tuner detects a stereo
pilot tone. `[MO]` (dim) is shown when the signal is monophonic or too weak
for stereo decoding.

### RDS

If the tuner hardware supports RDS (`V4L2_TUNER_CAP_RDS`), ncradio decodes
incoming RDS data automatically:

| Element | Location | Description |
|---------|----------|-------------|
| PS name | Right of volume bar | 8-character station name (e.g. `Capital FM`) appears in green once all 4 segments are received (typically within 1–2 s) |
| Radio Text | Info row | Up to 64-character "now playing" text; shown when no status message is pending |

RDS data is cleared whenever the frequency changes (tune, seek, or preset
selection). On hardware without RDS support, these areas stay blank.

## Scanning

Pressing `s` starts a full band sweep from 87.50 to 108.00 MHz. The scan runs
in a background thread so the UI stays responsive throughout. A progress bar
and a live list of found stations are shown during the sweep.

**RDS name collection** — if `Save RDS names` is enabled (default: Yes), the
scanner dwells on each found station for up to 1.5 s to collect the RDS PS
name. It exits early if the name is received sooner. Stations with no RDS
broadcast are saved without a name.

When the scan finishes (or is stopped), all found stations **replace** the
current preset list and are saved to `~/.ncradio.conf`. The tuner returns to
the frequency it was on before the scan.

## Settings

Press `o` to open the settings panel. Changes take effect immediately and are
written to `~/.ncradio.conf` on every adjustment.

| Setting | Default | Range | Description |
|---------|---------|-------|-------------|
| Scan step | 0.10 MHz | 0.025 / 0.05 / 0.10 / 0.20 MHz | Frequency increment during scan. Smaller steps are slower but find more stations |
| Signal threshold | 30% | 5% – 95% (5% steps) | Minimum signal strength for a frequency to be recorded as a station |
| Save RDS names | Yes | Yes / No | Whether to pause on each found station to collect its RDS PS name |

## Configuration file

Settings and presets are stored together in `~/.ncradio.conf`:

```
# ncradio configuration
scan_step=100000
signal_threshold=30
rds_names=1
# stations
87.90 BBC Radio 1
91.30
98.50 Capital FM
103.60 LBC
```

**Settings lines** are `key=value` pairs (written before the station list).
**Station lines** are a frequency in MHz followed by an optional name. Lines
starting with `#` are comments.

The file is rewritten in full every time a setting changes, a preset is
added/deleted/renamed, or a scan completes. You can also edit it by hand;
ncradio reads it at startup.

### Backward compatibility

Old config files (frequency lines only, no settings) are read correctly —
ncradio uses defaults for any settings not found in the file. If an old version
of ncradio reads a new config, it silently ignores the `key=value` lines and
reads the station frequencies normally.

## Hardware notes

ncradio uses the following V4L2 ioctls:

| ioctl | Purpose |
|-------|---------|
| `VIDIOC_G_TUNER` | Detect frequency unit, RDS capability, signal strength, stereo status |
| `VIDIOC_G_FREQUENCY` / `VIDIOC_S_FREQUENCY` | Get / set tuner frequency |
| `VIDIOC_S_HW_FREQ_SEEK` | Hardware-assisted station seek |
| `VIDIOC_S_CTRL` | Volume (`V4L2_CID_AUDIO_VOLUME`), mute (`V4L2_CID_AUDIO_MUTE`), RDS reception (`V4L2_CID_RDS_RECEPTION`) |

RDS data is obtained by calling `read()` on the radio device file descriptor,
which returns a stream of `struct v4l2_rds_data` blocks (3 bytes each).

Seek (`VIDIOC_S_HW_FREQ_SEEK`) is a blocking call; it may take up to a second
or two. If the tuner does not support hardware seek the ioctl returns an error
and the frequency is unchanged.
