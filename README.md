# libampdac

Userspace **USB Audio Class 2** output for Linux: bit-perfect PCM, native DSD,
and DSD-over-PCM (DoP) over libusb isochronous transfers. The DAC crystal is
clock master when explicit feedback exists.

This is **not** a fork of the Android player. It is a small library extracted
from the same USB engine, with a CLI and a Deadbeef plugin.

**Upstream (original work, MIT):**  
[androidexpert35/Audiophile-Music-Player](https://github.com/androidexpert35/Audiophile-Music-Player)

Shared engine sources are in [`native/`](native/) (see [NOTICE](NOTICE)).
Android, JNI, and FFmpeg stay in that project. Decode here is the host’s job
(`ffmpeg` for the CLI, Deadbeef for the plugin).

## Build

```bash
sudo apt install build-essential libusb-1.0-0-dev ffmpeg
git clone https://github.com/<you>/libampdac.git
cd libampdac
make
```

Or CMake:

```bash
cmake -S . -B build && cmake --build build
```

Artifacts: `build/libampdac.so`, `build/ampdac-cli`, `build/ddb_out_ampdac.so`.

## USB permission

Linux binds class-compliant DACs to `snd-usb-audio`. This library detaches that
driver for exclusive ISO access.

```bash
sudo ./grant-usb-dac.sh
```

Stop PipeWire playback on that card if `claim` is still BUSY.

## CLI

Plug **one** UAC2 DAC.

```bash
./build/ampdac-cli probe
./build/ampdac-cli tone --rate 48000 --seconds 10
./build/ampdac-cli play /path/to/cd.flac
./build/ampdac-cli play /path/to/concert/*.flac   # gapless, same format
./build/ampdac-cli play /path/to/track.dsf        # native DSD if the DAC has RAW
./build/ampdac-cli play --dop /path/to/track.dsf  # force DoP
```

Need `ffmpeg` / `ffprobe` on `PATH` for `play`.

## Deadbeef 1.10

```bash
make plugin
make install-deadbeef DEADBEEF_DIR=$HOME/deadbeef-1.10.3
```

Preferences → Sound → **ampdac USB DAC**. DSP off. Restart the player.
The plugin enables FFmpeg DoP so DSF/DFF are not converted to float.

## Test checklist

| Check | Command | Expect |
|---|---|---|
| Probe | `./build/ampdac-cli probe` | vid:pid, async + feedback, `native=` / `dop=` |
| Tone | `tone --rate 48000 --seconds 10` | underruns=0, DAC is clock master |
| PCM 16/44.1 | `play file.flac` | USB clock = file rate |
| PCM 24/96 | `play file-96k.flac` | no resample |
| Gapless PCM | `play concert/*.flac` | one USB session |
| DSD native | `play file.dsf` | clock = DSD/32 |
| DSD DoP | `play --dop file.dsf` | clock = DSD/16 |
| Deadbeef DSD | ampdac output, DSP off | not `1411200 Hz float` |

Format changes (44.1 → 96, DSD → FLAC) retune the PLL; that is a new session.

## Scope

- Class-compliant **UAC2** High-Speed, stereo, 4-byte Type-I PCM
- Native DSD when a RAW/TYPE_IV alt exists, else DoP
- One DAC at a time, exclusive libusb
- No resample, unity volume on the USB wire

## License

[MIT](LICENSE) — same as upstream. Keep the copyright notices.
