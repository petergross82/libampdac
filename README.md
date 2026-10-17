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
sudo install -m 755 volumio/ampdac/ampdac /usr/bin/ampdac
sudo install -m 755 build/libasound_module_pcm_ampdac.so \
    /usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/alsa-lib/
sudo install -m 644 alsa/50-ampdac.conf /usr/share/alsa/alsa.conf.d/50-ampdac.conf
sudo ln -sfn /usr/share/alsa/alsa.conf.d/50-ampdac.conf /etc/alsa/conf.d/50-ampdac.conf
sudo install -m 644 systemd/ampdac.service systemd/ampdac.socket /etc/systemd/system/
sudo install -m 644 wireplumber/50-ampdac.conf \
    /usr/share/wireplumber/wireplumber.conf.d/50-ampdac.conf
sudo install -m 644 udev/99-ampdac.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=usb
sudo systemctl daemon-reload
sudo systemctl enable --now ampdac.socket
sudo usermod -aG audio "$USER"   # then log in again
```

The same install is the Debian package `dist/ampdac_0.3.18-1_amd64.deb`:

```bash
sudo apt install ./dist/ampdac_0.3.18-1_amd64.deb
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

Volumio (Raspberry Pi, 32- or 64-bit) does not need a compiler. Build the
plugin zip on a PC with `scripts/build-volumio-plugin.sh`, copy
`dist/ampdac-volumio.zip` to the Pi, open `http://volumio.local/dev` and
upload that zip. Enable the plugin, open Playback Options and pick
**ampdac USB DAC** next to the kernel card. MPD still opens the PCM
`volumio`; while that entry is selected, `/etc/asound.conf` points it at
the `ampdac` plugin. The kernel card stays in the list for the driver path.

`./build/ipc_smoke ./build/ampdacd` checks the socket without a DAC.

## Utilities

`/usr/bin/ampdac` is the same program on Debian and on Volumio. Both
commands print English. `show transport` needs `python3`. The caller must
be in group `audio`, because `/run/ampdac.sock` is mode `0660`.

`ampdac show status` says which output is actually open. It lists kernel
cards, whether `ampdacd` is running, and the name of the player holding the
socket. On Volumio it also prints the web label, the `volumio` PCM route,
and the MPD outputs, because those three can disagree. On a normal Debian
install those Volumio blocks are omitted. If `/etc/mpd.conf` exists, its
outputs are still listed.

`ampdac show transport` asks `ampdacd` for one screen: the DAC name, USB
id, clocking, channel count, PCM rates and bit depths, whether native DSD
and DoP are advertised, then the live stream, the feedback clock, the ring
and the loss count. Nothing playing is `playback: idle`. On Volumio, if the
plugin route is not `ampdac`, it adds `plugin: inactive` and names the
kernel card when that card is the one playing.

`ampdac-cli` is a separate player, not one of those two commands. `list`
and `probe` describe the attached UAC2 DAC. `play` and `tone` send audio
through `libampdac` directly. `play` needs `ffmpeg` on `PATH`.

## Hardware

The transport opens one class-compliant **USB Audio Class 2** DAC and
sends isochronous audio itself. It is built for the asynchronous case:
the DAC crystal is clock master and the device provides an explicit
feedback endpoint. PCM keeps the file rate. 16-bit and 24-bit samples
are placed in the top of a 4-byte slot; the lower bits stay zero at
unity gain. If the DAC publishes both a 24-bit and a 32-bit 4-byte
alternate, 24-bit PCM uses the 24-bit one, because the 32-bit alternate
on some DACs plays the padding byte. A packed 3-byte slot is used only
when that rate and channel count have no 4-byte PCM alternate. Native
DSD is used when a RAW / Type IV alternate is advertised. Otherwise
DSD64, DSD128 and DSD256 go out as DoP on a 4-byte PCM alternate.
Stereo only. One client owns the DAC.

Brought up on a FiiO K7, both as the kernel card and through this
transport, on Volumio and on Debian.

It will not play:

- A USB Audio Class 1 device. Interfaces whose protocol is not UAC2
  (`0x20`) are skipped.
- Anything that is not a USB DAC: I2S HATs, HDMI, analogue jacks,
  S/PDIF-only converters.
- A PCM DAC whose only subslot is 2 bytes. The formatter accepts a
  4-byte slot, and a 3-byte slot only as the fallback above.
- DSD512 or any DSD rate other than DSD64, DSD128 and DSD256.
- DoP into a DAC that has no 4-byte PCM alternate. The DoP marker lives
  in the top byte of that slot, so the packed 3-byte fallback cannot
  carry it.
- More than two channels. DSD is rejected unless it is stereo, and the
  ALSA device `ampdac` accepts two channels only.
- A second player while the first still holds the DAC.
- Windows or macOS. The daemon is Linux, libusb and systemd.

An adaptive or synchronous UAC2 endpoint can still be opened, but it is
not the clock-master path. The bit-perfect claim (DAC crystal, explicit
feedback, file rate unchanged) is for asynchronous feedback devices.

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

`ampdac_write` blocks while the ring is full. The whole call gives up after
about 2 s and returns `AMPDAC_ERR_TIMEOUT`. It does not return a short
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

- Class-compliant **UAC2**, stereo. Normal PCM is a 4-byte Type I slot; a 3-byte slot is only the fallback described above
- Native DSD when a RAW/TYPE_IV alt exists, else DoP, DSD64/128/256
- One DAC at a time, exclusive libusb
- No resample, unity volume on the USB wire

## License

[MIT](LICENSE) — same as upstream. Keep the copyright notices.
