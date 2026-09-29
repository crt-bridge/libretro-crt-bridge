# crt-bridge client

A libretro core that receives the video, audio and gamepad stream of a crt-bridge emitter over
the local network and plays it on this machine; this machine's gamepad goes back to the
emitter. Two computers are enough.

## Works with

`crt-bridge emitter`: https://github.com/crt-bridge/RetroArch/releases/tag/v1.22.2-crtbridge.1

The emitter is pointed at this machine's address. Compatibility with other Groovy senders is
not claimed. Tested against that emitter release: on Windows, picture, sound and gamepad work
end to end; on Linux and on macOS (Apple Silicon), the core installs, loads and shows the
picture, but the tests ran over Wi-Fi, where most of the sound and, on Linux, part of the picture were lost (see below).

## Install

See `INSTALL.md` for step-by-step instructions for Windows, macOS and Linux. Requires
RetroArch 1.9.8 or later.

## Security

The protocol has no authentication and no encryption. Use it on a local network, or through a
tunnel (WireGuard, Tailscale); never expose its UDP ports to the internet.

## Known limitations

- Black screens that the emitter's re-announcement does not recover: no automatic recourse
  yet, restart the core or the emitter.
- A core started after the emitter can take about 2 seconds to show its first image: this is
  expected, not a fault.
- A mode change lost over a Wi-Fi link is not repaired until the next one: restart the core if
  the image looks wrong after a mode change.
- The frame-rate-collapse message is hard to read in windowed mode: switch to fullscreen, or
  check the run report file instead.
- Metal puts the core's loop to sleep on macOS: set RetroArch's video driver to `glcore`, not
  Metal.
- The macOS x86_64 (Intel) slice of the universal binary is built but not tested on real
  hardware.
- Windows binaries are unsigned; macOS binaries are signed ad hoc, not notarized: expect a
  security warning on first launch, see `INSTALL.md`.
- Running the emitter and this client on the same Windows PC: set RetroArch's audio driver to
  `xaudio`, not the default `wasapi`, or the emitter can lose its own sound and run far faster
  than its content's native rate.
- A Wi-Fi connection can lose most of this machine's sound while the picture keeps arriving:
  use a wired connection instead when you can; a future emitter release is expected to fix this.
- Restarting this client while the emitter's content keeps running does not always bring back
  sound and the gamepad on their own: restart the content on the emitter too if they do not
  return.

## Build from source

Requires a C compiler and GNU Make.

- **Windows**: MSYS2 MINGW64
- **macOS**: Xcode command line tools (GNU Make 3.81 is enough)
- **Linux**: build-essential

```
make
```

Builds the core and its `.info` file into `build/`. `make test` runs the unit tests;
`make test-frozen-run` runs the frozen-run smoke test; `make print-version` prints the core's
version.

## What this repository is

A public copy of the client as released; development happens elsewhere. This repository
receives one commit per synchronisation, never a history rewrite.

## Credits and license

Based on the Groovy protocol by psakhis: https://github.com/psakhis/Groovy_MiSTer

Not affiliated with, or endorsed by, the libretro project.

Licensed under GPL-3.0-or-later (see `LICENSE`). Third-party components are listed in
`NOTICE.txt`.
