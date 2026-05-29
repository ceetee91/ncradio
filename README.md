# ncradio

A small ncurses FM radio controller for Linux, built for V4L2-compatible tuner
cards and USB radio sticks accessible as `/dev/radio0`. Includes a live audio
pipe that routes the tuner's capture output to the system's default playback
device.

```
              ncradio v0.1
──────────────────────────────────────────────────────────────────
 Freq:  98.50 MHz  [ST]    Signal:[||||||||||....] 71%
 Vol:   [||||||||||||||] 80%  [MUTED] [A]          Capital FM
 Coldplay - The Scientist                               ← RDS radio text
──────────────────────────────────────────────────────────────────
 Presets (9):
 > 1.  87.90 BBC R1      4.  98.50 Capital  <  7. 105.40
   2.  91.30 Classic     5. 101.00              8. 107.30
   3.  94.50 Radio 3     6. 103.60 LBC          9. 107.90
──────────────────────────────────────────────────────────────────
 s:scan  ,:step<  .:step>  <:seek<  >:seek>  t:tune  m:mute  +/-:vol
 a:add  d:del  e:rename  o:settings  arrows:navigate  Enter:tune  q:quit
```

## Requirements

**Runtime:**
- Linux kernel with V4L2 radio support (`/dev/radio0`)
- ALSA (`libasound`) for audio output

**Build:**
- `ncurses` development headers (`ncurses-devel` / `libncurses-dev`)
- `alsa-lib` development headers (`alsa-lib-devel` / `libasound2-dev`)
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
| `,` | Step frequency down by the configured scan step |
| `.` | Step frequency up by the configured scan step |
| `<` | Seek backward — find previous station at or above signal threshold |
| `>` | Seek forward — find next station at or above signal threshold |
| `t` | Manual tune — type a frequency in MHz, then Enter |
| `+` or `=` | Volume up (5% step) |
| `-` | Volume down (5% step) |
| `m` | Toggle mute |
| `a` | Add current frequency to presets |
| `d` | Delete the highlighted preset |
| `e` | Rename the highlighted preset |
| `o` | Open settings panel |
| `↑` / `↓` | Move selection down/up within the current preset column |
| `←` / `→` | Move selection one column left/right in the preset grid |
| `PgUp` / `PgDn` | Scroll preset list by one visible window of rows |
| `Enter` | Tune to the highlighted preset |
| `q` | Quit |

### Tuning mode (after pressing `t`)

| Key | Action |
|-----|--------|
| `0`–`9` | Enter digit |
| `.` or `,` | Decimal point (both accepted) |
| `Backspace` | Delete last character |
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

### Seek mode (after pressing `<` or `>`)

| Key | Action |
|-----|--------|
| Any key | Cancel seek and restore previous frequency |

### Settings panel (after pressing `o`)

| Key | Action |
|-----|--------|
| `↑` / `↓` | Select setting |
| `←` / `→` | Adjust value |
| `Enter` | Toggle boolean settings (RDS names, Audio output) |
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
| PS name | Right of volume bar | 8-character station name (e.g. `Capital FM`) appears in green once all 4 RDS segments arrive — typically 1–2 s |
| Radio Text | Info row | Up to 64-character "now playing" text; shown when no status message is pending |

RDS data is cleared whenever the frequency changes (tune, step, seek, or
preset selection). On hardware without RDS support these areas stay blank.

### Audio indicator

`[A]` (green) appears on the volume row while the audio pipe is running.
`[A!]` (red) appears if the pipe stopped due to an error; the error text
is shown in the settings panel next to the **Audio output** row.

## Stepping, scanning, and seeking

### Manual step (`,` / `.`)

Step the tuner by the configured **Scan step** in either direction. The same
step size applies to both manual browsing and the automatic band scan.

### Automatic scan (`s`)

A full band sweep from 87.50 to 108.00 MHz runs in a background thread so the
UI stays responsive. A progress bar and a live list of found stations are shown
during the sweep; the list auto-scrolls to always show the most recently found
station.

**RDS name collection** — if `Save RDS names` is enabled (default: Yes), the
scanner dwells on each found station for up to 1.5 s to collect its RDS PS
name, exiting early once the name is received.

When the scan finishes (or is stopped), all found stations **replace** the
current preset list and are saved to `~/.ncradio.conf`. The tuner returns to
the frequency it was on before the scan.

### Software seek (`<` / `>`)

Seek steps through frequencies in the configured direction using the same scan
step and signal threshold as the automatic scan, stopping at the first
frequency that meets or exceeds the threshold. Seek runs in a background thread
— the UI remains responsive and any keypress cancels it. If no qualifying
station is found after a full sweep of the band, the tuner is restored to its
pre-seek frequency and a "No station found" message is shown.

## Audio output

ncradio can pipe the tuner's audio directly to the system's default playback
device (PulseAudio, PipeWire, or ALSA "default"). This is functionally
equivalent to running:

```sh
arecord -D hw:2,0 -r 96000 -f S16_LE -c 2 | aplay -
```

but handled entirely in-process via the ALSA library.

### Enabling audio

1. Press `o` to open settings.
2. Navigate to **Audio output** and press `Enter` or `←`/`→` to switch to **On**.
3. If no device is configured yet, ncradio auto-selects the first detected
   capture device.
4. Navigate to **Audio device** and use `←`/`→` to cycle through all detected
   ALSA capture devices. The device description (card and PCM name) is shown
   as a hint on that row.

Changes take effect immediately — switching the device or toggling audio
restarts the pipe on the fly.

### Rate and channel detection

ncradio probes the capture device for the highest supported sample rate from
the list `{96000, 48000, 44100, 32000, 22050, 16000}` Hz. It tries stereo
first and falls back to mono if the device does not support two channels. The
detected rate and channel count are displayed in the **Audio output** row of
the settings panel while the pipe is running (e.g. `On (48000Hz 2ch)`).

The playback side opens `"default"` (PulseAudio/PipeWire/ALSA) and matches the
same format, letting the system's audio stack handle any further resampling.

### Audio device list

Only physical `hw:X,Y` ALSA PCM capture devices appear in the device list —
virtual plugins such as `default`, `dmix`, and `null` are excluded. To find
your tuner's device name outside of ncradio:

```sh
arecord -l          # list capture devices
cat /proc/asound/cards
```

## Preset list

Presets are displayed in a multi-column grid that fills the terminal width
automatically:

- Frequencies are shown as `XX.XX` (no "MHz" label).
- Presets are arranged **column-major**: the list fills downward within a
  column before spilling into the next, like `ls` output. Preset 2 is below
  preset 1, not next to it.
- The number of columns is derived from the terminal width and the length of
  the longest preset name. More columns are used when names are absent or
  short; fewer when names are longer.
- The currently tuned preset is marked with `<`.
- The selected (highlighted) preset is marked with `>`.
- `↑`/`↓` move within a column; `←`/`→` jump one column; `PgUp`/`PgDn`
  scroll by one visible window of rows.

## Settings

Press `o` to open the settings panel. Changes take effect immediately and are
written to `~/.ncradio.conf` on every adjustment.

| Setting | Default | Range / values | Description |
|---------|---------|----------------|-------------|
| Scan step | 0.10 MHz | 0.025 / 0.05 / 0.10 / 0.20 MHz | Frequency increment for scan, manual step, and seek |
| Signal threshold | 30% | 5% – 95% (5% steps) | Minimum signal strength to record a station during scan/seek |
| Save RDS names | Yes | Yes / No | Whether to pause on each found station to collect its RDS PS name during scan |
| Audio output | Off | Off / On | Enable or disable the audio pipe |
| Audio device | (auto) | any detected `hw:X,Y` | ALSA capture device to read audio from |

## Configuration file

Settings and presets are stored together in `~/.ncradio.conf`:

```
# ncradio configuration
scan_step=100000
signal_threshold=30
rds_names=1
audio_enabled=0
audio_device=hw:2,0
# stations
87.90 BBC Radio 1
91.30
98.50 Capital FM
103.60 LBC
```

**Settings lines** — `key=value` pairs written before the station list:

| Key | Value | Meaning |
|-----|-------|---------|
| `scan_step` | Hz (e.g. `100000`) | Frequency step for scan, step, and seek |
| `signal_threshold` | percentage (e.g. `30`) | Minimum signal to record a station |
| `rds_names` | `0` or `1` | Whether to collect RDS names during scan |
| `audio_enabled` | `0` or `1` | Whether to start the audio pipe at launch |
| `audio_device` | ALSA device (e.g. `hw:2,0`) | Capture device for the audio pipe |

**Station lines** — frequency in MHz, optional name after a space. Lines
starting with `#` are comments.

The file is rewritten in full every time a setting changes, a preset is
added/deleted/renamed, or a scan completes. You can also edit it by hand;
ncradio reads it at startup.

### Backward compatibility

Old config files (frequency lines only, no settings) are read correctly —
ncradio uses defaults for any settings not found in the file. Old ncradio
versions reading a new config silently ignore the `key=value` lines (the
`%lf` scan for a float fails on `scan_step=…` and the line is skipped).

## Hardware notes

### V4L2 radio ioctls

| ioctl | Purpose |
|-------|---------|
| `VIDIOC_G_TUNER` | Detect frequency unit, RDS capability, signal strength, stereo status |
| `VIDIOC_G_FREQUENCY` / `VIDIOC_S_FREQUENCY` | Get / set tuner frequency |
| `VIDIOC_S_HW_FREQ_SEEK` | Hardware-assisted station seek (available in `radio.c`, not currently bound to a key) |
| `VIDIOC_S_CTRL` | Volume (`V4L2_CID_AUDIO_VOLUME`), mute (`V4L2_CID_AUDIO_MUTE`), RDS reception (`V4L2_CID_RDS_RECEPTION`) |

RDS data is obtained by calling `read()` on the radio device file descriptor,
which returns a stream of `struct v4l2_rds_data` blocks (3 bytes each).

### ALSA audio

The audio pipe uses the following ALSA API calls:

| Call | Purpose |
|------|---------|
| `snd_device_name_hint` | Enumerate PCM devices for the settings panel |
| `snd_pcm_open` | Open capture (`hw:X,Y`) and playback (`default`) PCMs |
| `snd_pcm_hw_params_test_rate` | Probe supported sample rates without modifying device state |
| `snd_pcm_hw_params_*` | Configure format (S16\_LE), channels, rate, period size |
| `snd_pcm_wait` | Wait for capture data with a 100 ms timeout (allows stop-flag check) |
| `snd_pcm_readi` / `snd_pcm_writei` | Interleaved read/write of sample frames |
| `snd_pcm_recover` | Recover from buffer overruns and underruns |
