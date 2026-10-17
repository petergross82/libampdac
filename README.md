# libampdac

Userspace **USB Audio Class 2** output for Linux. **ampdacd** holds the DAC
and sends bit-perfect PCM, native DSD, and DSD-over-PCM (DoP) over libusb
isochronous transfers. Players do not open the USB device. They open the
ALSA device `ampdac`, which talks to `/run/ampdac.sock`.

When the DAC publishes explicit feedback, its crystal is the clock master
and the file rate is not changed. Unity gain leaves 16-bit and 24-bit
samples in the top of the USB slot.

This is not a fork of the Android player. The USB engine in [`native/`](native/)
comes from that project (see [NOTICE](NOTICE)). Android, JNI, and FFmpeg stay
there. Decode here is the player's job.

**Upstream (original work, MIT):**  
[androidexpert35/Audiophile-Music-Player](https://github.com/androidexpert35/Audiophile-Music-Player)

Packages: [v0.3.18](https://github.com/petergross82/libampdac/releases/tag/v0.3.18).

## Install

### Debian (amd64)

```bash
sudo apt install ./ampdac_0.3.18-1_amd64.deb
sudo usermod -aG audio "$USER"
```

Log in again so the `audio` group applies. The package enables
`ampdac.socket`. The daemon starts when something connects to the socket.
`aplay -L` lists `ampdac`.

The desktop socket is mode `0660` and group `audio`. `ampdac show transport`
also needs `python3`, which the package depends on.

### Volumio 3

A factory image has no `unzip` and no zip upload on the developer page.
Use the tar archive. On the Pi:

```bash
tar -xzf ampdac-volumio.tar.gz
cd ampdac
volumio plugin install
```

Answer `y` to the prompt about an unsafe plugin. The plugin ships prebuilt
binaries for 32-bit and 64-bit Pi. Nothing is compiled on the device.

The archive on the v0.3.18 release is plugin **0.3.17**. It does not yet
contain the packed 3-byte PCM fallback. That fallback is in the Debian
package.

Enable the plugin, open Playback Options, and pick **ampdac USB DAC** next
to the kernel card. MPD still opens the PCM device `volumio`. While that
entry is selected, `/etc/asound.conf` points `volumio` at `ampdac`. The
kernel card stays in the list. Disabling the plugin restores the card that
was selected before. The Volumio socket is mode `0666`, because MPD's user
is not in group `audio`.

## Play

In Audacious, Strawberry, or MPD select the ALSA device `ampdac` and turn
resampling off. One client owns the DAC. A second open gets "busy".

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

Moode rewrites `mpd.conf`. Put the block back after it does.

Volume on this path stays at unity. Use the DAC's own control. A format
change (44.1 to 96, or DSD to PCM) closes the USB session and opens another.

## Utilities

`/usr/bin/ampdac` is the same program on Debian and on Volumio. Both
commands print English.

`ampdac show status` says which output is open. It lists kernel cards,
whether `ampdacd` is running, and the player holding the socket. On Volumio
it also prints the web label, the `volumio` PCM route, and the MPD outputs,
because those three can disagree. On Debian those Volumio blocks are
omitted. If `/etc/mpd.conf` exists, its outputs are still listed.

`ampdac show transport` asks the daemon for one screen: DAC name, USB id,
clocking, channel count, PCM rates and bit depths, native DSD and DoP, then
the live stream, the feedback clock, the ring, and losses. Nothing playing
is `playback: idle`. On Volumio, if the route is not `ampdac`, it adds
`plugin: inactive` and names the kernel card when that card is playing.

`ampdac-cli` is a separate program. `list` and `probe` describe the DAC.
`play` and `tone` open the USB device themselves, not through `ampdacd`.
Do not run them while a player is using `ampdac`. `play` needs `ffmpeg` on
`PATH`.

```bash
ampdac-cli probe
ampdac-cli tone --rate 48000 --seconds 10
ampdac-cli play /path/to/cd.flac
ampdac-cli play --dop /path/to/track.dsf
```

## Hardware

The transport is built for one class-compliant **USB Audio Class 2** DAC
in the asynchronous case: the DAC crystal is clock master and the device
provides an explicit feedback endpoint. PCM keeps the file rate. 16-bit and
24-bit samples sit in the top of a 4-byte slot. The lower bits stay zero
at unity gain. If the DAC publishes both a 24-bit and a 32-bit 4-byte
alternate, 24-bit PCM uses the 24-bit one. A packed 3-byte slot is used
only when that rate and channel count have no 4-byte PCM alternate. Native
DSD is used when a RAW / Type IV alternate is advertised. Otherwise DSD64,
DSD128, and DSD256 go out as DoP on a 4-byte PCM alternate. Stereo only.

Brought up on a FiiO K7, as the kernel card and through this transport, on
Volumio and on Debian.

It will not play:

- A USB Audio Class 1 device. Interfaces whose protocol is not UAC2
  (`0x20`) are skipped.
- Anything that is not a USB DAC: I2S HATs, HDMI, analogue jacks,
  S/PDIF-only converters.
- A PCM DAC whose only subslot is 2 bytes. The formatter accepts a 4-byte
  slot, and a 3-byte slot only as the fallback above.
- DSD512, or any DSD rate other than DSD64, DSD128, and DSD256.
- DoP into a DAC that has no 4-byte PCM alternate. The DoP marker lives in
  the top byte of that slot.
- More than two channels. DSD must be stereo, and the ALSA device `ampdac`
  accepts two channels only.
- A second player while the first still holds the DAC.
- Windows or macOS.

An adaptive or synchronous UAC2 endpoint can still be opened. That is not
the clock-master path. The bit-perfect claim is for asynchronous feedback
devices.

## Build

Only needed to compile. The packages above are already built.

```bash
sudo apt install build-essential libusb-1.0-0-dev ffmpeg
make
```

Or `cmake -S . -B build && cmake --build build`.

Artifacts: `build/libampdac.so`, `build/ampdacd`, `build/ampdac-cli`,
`build/libasound_module_pcm_ampdac.so`, `build/ddb_out_ampdac.so`.
`./build/ipc_smoke ./build/ampdacd` checks the socket without a DAC.
`scripts/build-deb.sh` rebuilds the Debian package.
`scripts/build-volumio-plugin.sh` cross-builds the Pi plugin. It needs a
Zig toolchain and libusb sources. The Pi does not compile.

Deadbeef 1.10 connects to `ampdacd`. It forces FFmpeg DoP so DSF/DFF are
not converted to float, then asks the daemon for native DSD when the DAC
has it. DSP off. Start the daemon first.

```bash
make plugin
make install-deadbeef DEADBEEF_DIR=$HOME/deadbeef-1.10.3
```

## Troubleshooting

- **`ampdac show transport` cannot reach the socket on Debian.** The user
  is not in group `audio`, or the login is from before `usermod`. Log in
  again.
- **The DAC stays on the kernel card.** On Volumio the plugin is off or
  Playback Options still name the kernel card. `ampdac show status` prints
  both the web label and the route.
- **claim returns BUSY.** PipeWire, Pulse, or the kernel driver still holds
  the card. Stop that playback. On a desktop,
  `systemctl --user stop pipewire pipewire-pulse wireplumber`.
- **PCM right after DSD is silent.** The DAC's clock has to settle after
  the DSD session. The next open retries the clock set. Wait for that open
  to finish instead of starting a second player.
- **Dropouts while the Pi is busy.** The daemon asks for `SCHED_FIFO`. The
  systemd unit grants `CAP_SYS_NICE`. A hand-built binary needs the same
  capability, or it runs on the normal scheduler.

## License

[MIT](LICENSE). Same as upstream. Keep the copyright notices.
