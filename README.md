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
git clone https://github.com/petergross82/libampdac.git
cd libampdac
make
```

Or CMake:

```bash
cmake -S . -B build && cmake --build build
```

Artifacts: `build/libampdac.so`, `build/ampdacd`, `build/ampdac-cli`,
`build/libasound_module_pcm_ampdac.so`, `build/ddb_out_ampdac.so`.

## Players

The DAC is opened by **ampdacd**, not by the player. The daemon holds
`CAP_SYS_NICE` and `CAP_SYS_ADMIN` (SCHED_FIFO and unbinding `snd-usb-audio`).
Players talk to `/run/ampdac.sock`, which is group `audio`.

```bash
sudo install -m 755 build/libampdac.so /usr/lib/libampdac.so
sudo install -m 755 build/ampdacd /usr/bin/ampdacd
sudo install -m 755 build/libasound_module_pcm_ampdac.so \
    /usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/alsa-lib/
sudo install -m 644 alsa/50-ampdac.conf /usr/share/alsa/alsa.conf.d/50-ampdac.conf
sudo install -m 644 systemd/ampdac.service systemd/ampdac.socket /etc/systemd/system/
sudo install -m 644 wireplumber/50-ampdac.conf \
    /usr/share/wireplumber/wireplumber.conf.d/50-ampdac.conf
sudo install -m 644 udev/99-ampdac.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=usb
sudo systemctl daemon-reload
sudo systemctl enable --now ampdac.socket
sudo usermod -aG audio "$USER"   # then log in again
```

`aplay -L` lists `ampdac`. In Audacious, Strawberry, or MPD select that ALSA
device and turn resampling off. MPD:

```
audio_output {
    type            "alsa"
    name            "ampdac USB DAC"
    device          "ampdac"
    mixer_type      "none"
    auto_resample   "no"
    auto_format     "no"
    dop             "yes"
    use_mmap        "no"
}
```

Moode rewrites `mpd.conf`; put the same block back after it does.

Volumio: install this package first, then the plugin in `volumio/ampdac`
(Miscellanea). It enables the socket and appends that output to MPD and to
Volumio's template. Volume on this path is unity.

`./build/ipc_smoke ./build/ampdacd` checks the socket without a DAC.

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

The output plugin does not link libusb. It connects to ampdacd, still forces
FFmpeg DoP, and still unwraps markers before native DSD. Start the daemon
before playback.

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

`ampdac_write` blocks while the ring is full, up to about 2 s since the last
successful push, then returns `AMPDAC_ERR_TIMEOUT`. It does not return a short
count for back-pressure. A short non-negative count means only that the tail
was not a whole sample frame; pass that tail again with the next buffer.

## Troubleshooting

- **claim returns BUSY.** PipeWire or Pulse still holds the ALSA node, so
  `USBDEVFS_DISCONNECT` fails. Stop playback on that card, or
  `systemctl --user stop pipewire pipewire-pulse wireplumber`. The sysfs
  `unbind` fallback needs write access to the driver node (root or a udev ACL).
- **PCM right after DSD is silent or clocks fail.** `kDsdPllSettleUs` (50 ms)
  runs only inside `ampdac_configure_dsd`, after `SET_CUR` and before the
  streaming alt. The next PCM session waits 20 ms on alt 0 before `SET_CUR`
  (`idle_then_clock` in `src/ampdac.cpp`). If the XMOS PLL is still unwinding,
  that 20 ms is the knob; a failed `SET_CUR` already resets the device and
  retries after 100 ms.
- **XRUN while the machine is busy (apt, network) on a Pi.** The libusb event
  thread asks for `SCHED_FIFO` and continues on CFS if that returns `EPERM`.
  Grant it with `sudo setcap cap_sys_nice+ep ./build/ampdac-cli`, or
  `AmbientCapabilities=CAP_SYS_NICE` on the systemd unit.

## Scope

- Class-compliant **UAC2** High-Speed, stereo, 4-byte Type-I PCM
- Native DSD when a RAW/TYPE_IV alt exists, else DoP
- One DAC at a time, exclusive libusb
- No resample, unity volume on the USB wire

## License

[MIT](LICENSE) — same as upstream. Keep the copyright notices.
