# libampdac

Userspace USB Audio Class 2 transport for Linux. **ampdacd** opens the DAC
with libusb and writes isochronous OUT packets. A player never opens the USB
device. It opens the ALSA device `ampdac`, and that device talks to
`/run/ampdac.sock`.

The engine in [`native/`](native/) comes from
[androidexpert35/Audiophile-Music-Player](https://github.com/androidexpert35/Audiophile-Music-Player)
(see [NOTICE](NOTICE)). This tree is not that player. Decode stays in the
player. Android, JNI, and FFmpeg are not part of the transport.

## What it does

On a UAC2 DAC that publishes explicit feedback, the DAC crystal is the clock
master. ampdacd counts the feedback endpoint and submits the next packets to
match it. The file rate is not resampled. Gain on this path is unity.

PCM uses the narrowest alternate whose advertised width can hold the file.
The DAC declares that width as `bBitResolution` on each PCM alternate.

| Advertised PCM alternates | 16-bit file | 24-bit file | 32-bit file |
| --- | --- | --- | --- |
| 32, 24 and 16 | 16 | 24 | 32 |
| 32 and 16, as on FiiO KA17 | 16 | 32 | 32 |
| 32 and 24, as on SMSL with Thesycon firmware | 24 | 24 | 32 |
| one alternate | that alternate | that alternate | that alternate |

When the chosen slot is wider than the file, the sample stays in the top of
the slot and the padding bits stay zero. A 16-bit file on the 16-bit
alternate fills that slot; it is not padded out to 32. A packed 3-byte slot
is used only when no 4-byte alternate of that width exists. Native DSD is
used when the DAC advertises a raw DSD alternate. Otherwise DSD64, DSD128,
and DSD256 go out as DoP on a 4-byte PCM alternate. One client owns the DAC.

`ampdac show transport` prints the DAC descriptor, every advertised alternate,
which alternate is open, the live stream, the feedback clock, the ring, and
losses. `ampdac show status` prints the same alternate list.

~~~
$ ampdac show transport
DAC: FIIO KA17
...
stream: PCM 44100 Hz, 16 bit, stereo
alt: interface 1, alt 2, 16 valid bits, 2-byte slot
~~~

## What we measured

These figures are from captures on the machines and DACs below. They are
examples, not a specification.

The kernel driver `snd-usb-audio` and libampdac followed the same feedback.
The host did not use a smoothed or free-running clock in either case.

| Stream | Feedback, both paths |
| --- | --- |
| 44.1 kHz | 44099.61 Hz |
| 96 kHz | 95999.02 Hz |
| DSD128 | 176397.46 Hz |

The packet rhythm is not the same. The kernel submits 6 or 7 packets per
URB and completions landed around 0.8 ms apart. libampdac submits 8 packets
and completions landed around 1 ms apart. On a laptop with an Ice Lake
controller the libampdac capture had no `ECOMM`. On a Raspberry Pi, ECOMM
appeared in the first 1–2 seconds after the interface was claimed. On a
laptop with an Ice Lake controller, the same capture had none. This points
to the Pi's xHCI at stream open, not to the transport.

Where a recording sounded different, the USB alternate was the split we
could see. The clock trace was not.

| File | Kernel on that DAC | libampdac |
| --- | --- | --- |
| 16-bit PCM, FiiO KA17 | 16-bit alternate | 16-bit alternate. Release 0.3.20 opened the 32-bit alternate and put the sample in the top of the slot |
| 24-bit PCM, SMSL with Thesycon firmware | 32-bit alternate. ALSA has no 24-bit format on that card, so the padding byte is part of the slot | 24-bit alternate, when the DAC advertises one |
| Same file on moOde, kernel entry | On the kernel path, moOde's MPD configuration resamples to 96 kHz by default. libampdac bypasses that: the file rate reaches the DAC unchanged | (no change; format stays the file width) |

Whether that alternate difference is audible depends on the DAC firmware.
On SMSL with Thesycon firmware, the 32-bit alternate plays the padding byte
as audio; the 24-bit alternate does not. On FiiO KA17, which has no 24-bit
alternate, this difference does not exist.

On moOde, a separate difference applies: the kernel path resamples to 96 kHz
by default. libampdac does not, so the file rate reaches the DAC unchanged.

## DACs this was run on

| DAC | Class | What played |
| --- | --- | --- |
| FiiO K7 | UAC2, asynchronous, explicit feedback | PCM up to 384 kHz / 32-bit, DSD up to DSD256. DSD512 and 768 kHz were left out: they were noisy, and the rate check does not accept DSD512 |
| FiiO KA17 | UAC2, asynchronous, explicit feedback | PCM 44.1–768 kHz. Alternates: 32-bit, 16-bit, native DSD. DoP uses the 32-bit PCM alternate |
| SMSL with Thesycon firmware (DS20, and the unit 152a:85dd) | UAC2, asynchronous, explicit feedback | 32-bit PCM, 24-bit PCM, native DSD |
| Texas Instruments PCM2706C | UAC1 | Not opened. An interface whose protocol is not `0x20` is skipped |

## What it does not do

- It does not drive USB Audio Class 1.
- It does not drive an I2S HAT, HDMI, an analogue jack, or a converter that
  has no UAC2 interface.
- It does not play DSD512, or any DSD rate other than DSD64, DSD128, and
  DSD256.
- It does not play more than two channels. The ALSA device accepts two.
- It does not share the DAC. A second open gets "busy".
- It does not run on Windows or macOS.
- An adaptive or synchronous UAC2 endpoint can still be opened. That is not
  the clock-master path, and the notes above are not about it.
- Volume on this path stays at unity. Use the control on the DAC.
- DeaDBeeF sees libampdac as a normal ALSA device. Select it under
  Preferences → Sound and turn DSP off. No plugin is needed; the transport
  is below ALSA, not above it.

A format change closes the USB session and opens another. PCM right after
DSD waits while the DAC clock settles. Wait for that open. Do not start a
second player on top of it.

## Packages

[v0.3.21](https://github.com/petergross82/libampdac/releases/tag/v0.3.21).

| Package | Where |
| --- | --- |
| `ampdac_0.3.21-1_amd64.deb` | Debian amd64. ALSA device `ampdac` |
| `ampdac-volumio_0.3.21-2_armhf.deb` | Volumio 3, 32-bit Pi |
| `ampdac-volumio_0.3.21-2_arm64.deb` | Volumio 3, 64-bit Pi |
| `ampdac-moode_0.3.21-1_arm64.deb` | moOde 10.3.6, 64-bit only |

The Volumio package was installed with `dpkg` on a 32-bit Volumio 3 image.
The moOde package runs the install script that was used on moOde 10.3.6.
Both images are Raspberry Pi OS. The moOde image we used is 64-bit; there
is no 32-bit moOde build.

### Debian

~~~
sudo apt install ./ampdac_0.3.21-1_amd64.deb
sudo usermod -aG audio "$USER"
~~~

Log in again so the `audio` group applies. The package enables
`ampdac.socket`. The daemon starts when something connects. `aplay -L`
lists `ampdac`. The socket is mode `0660`, group `audio`.

In Audacious, Strawberry, DeaDBeeF, or MPD select the ALSA device `ampdac`
and turn resampling off.

~~~
audio_output {
    type            "alsa"
    name            "ampdac"
    device          "ampdac"
    mixer_type      "none"
    auto_resample   "no"
    auto_format     "no"
    dop             "yes"
    use_mmap        "no"
}
~~~

Remove it with:

~~~
sudo dpkg -r ampdac
~~~

### Volumio 3

Use the armhf package on a 32-bit image and the arm64 package on a 64-bit
image. Installing restarts Volumio once, so the plugin process loads.

~~~
sudo apt install ./ampdac-volumio_0.3.21-2_armhf.deb
~~~

Enable **ampdac** under Plugins → Installed plugins if it is off. Playback
Options then lists the kernel card and **libampdac ·** the DAC name. Changing
the output applies immediately. MPD is not restarted: the output is closed
and opened again, and the queue stays. A gap of about two seconds is the
USB handoff. On the 32-bit image, Unplugged went to the kernel card and back
with the position kept.

The Info button on the ampdac row runs `ampdac show transport`.

The kernel entry is Volumio's normal plug → hardware path. The libampdac
entry is the socket. While libampdac is selected, a USB rebind does not
move the output back to HDMI. Volume for that entry is the DAC knob.
`ampdac.service` is not enabled at boot. The socket starts the daemon.

Remove it with:

~~~
sudo dpkg -r ampdac-volumio
~~~

That restores the kernel route, removes the Info button, and removes the
units.

### moOde

64-bit only. Tested path: moOde 10.3.6.

~~~
sudo dpkg -i ampdac-moode_0.3.21-1_arm64.deb
~~~

Configure → Audio → Output device lists the kernel DAC and
**libampdac ·** the DAC name. Changing the list applies immediately, including
during playback. The gap is the USB handoff. Clicks or a short pause are
expected. MPD is not restarted and the queue stays. CamillaDSP and the moOde
equaliser are not in the chain while libampdac is selected. The kernel entry
is moOde's normal route, with resampling and automatic format conversion as
moOde sets them. The daemon is socket-activated and is not enabled at boot.

Remove it with:

~~~
sudo dpkg -r ampdac-moode
~~~

That puts the output back on the kernel card and removes the extra list entry.

## Utilities

`/usr/bin/ampdac` is the same program on Debian, Volumio, and moOde.

`ampdac show status` says which output is open, whether `ampdacd` is
running, and which player holds the socket. On Volumio it also prints the
web label, the `volumio` PCM route, and the MPD outputs.

`ampdac show transport` prints one screen for the DAC and the live stream.
Nothing playing is `playback: idle`.

`ampdac-cli` is a separate program. `play` and `tone` open the USB device
themselves, not through `ampdacd`. Do not run them while a player holds
`ampdac`. `play` needs `ffmpeg` on `PATH`.

~~~
ampdac-cli probe
ampdac-cli tone --rate 48000 --seconds 10
ampdac-cli play /path/to/cd.flac
ampdac-cli play --dop /path/to/track.dsf
~~~

## Build

Only needed to compile. The packages above are already built.

~~~
sudo apt install build-essential libusb-1.0-0-dev ffmpeg
make
~~~

Or `cmake -S . -B build && cmake --build build`.

Artifacts: `build/libampdac.so`, `build/ampdacd`, `build/ampdac-cli`,
`build/libasound_module_pcm_ampdac.so`.
`./build/ipc_smoke ./build/ampdacd` checks the socket without a DAC.
`scripts/build-deb.sh` rebuilds the Debian package.
`scripts/build-player-debs.sh` packs the Volumio and moOde packages from
`volumio/ampdac/bin` and `moode/ampdac/bin`. It does not cross-compile.
`scripts/build-volumio-plugin.sh` is the cross build. It needs a Zig
toolchain and libusb sources.

## Troubleshooting

- **`ampdac show transport` cannot reach the socket on Debian.** The user
  is not in group `audio`, or the login is from before `usermod`. Log in
  again.
- **The DAC stays on the kernel card.** On Volumio the plugin is off, or
  Playback Options still names the kernel card. On moOde the Output device
  list still names the kernel card. `ampdac show status` prints the route.
- **claim returns BUSY.** PipeWire, Pulse, or the kernel driver still holds
  the card. Stop that playback. On a desktop,
  `systemctl --user stop pipewire pipewire-pulse wireplumber`.
- **PCM right after DSD is silent.** The DAC clock has to settle after the
  DSD session. The next open retries the clock set.
- **Dropouts while the Pi is busy.** The daemon asks for `SCHED_FIFO`. The
  systemd unit grants `CAP_SYS_NICE`. A hand-built binary needs the same
  capability, or it runs on the normal scheduler.
## License

[MIT](LICENSE). Same as upstream. Keep the copyright notices.
