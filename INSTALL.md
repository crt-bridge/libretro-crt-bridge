# Installing the crt-bridge client

The crt-bridge client is a RetroArch core, `crt-bridge client` (`crt_bridge_libretro`), that
receives the video and audio stream sent by an emitter PC on the same local network, displays
and plays it here, and sends this machine's gamepad back to that PC. All you need is RetroArch
and the core file - nothing else. This document is written to be followed step by step, from
nothing, by someone who has never seen this repository, on Windows, macOS, or Linux.

**What this is not:** a game, or a copy of whatever the emitter is running. It only receives.

## 1. What this is

Two computers on the same local network are enough: one runs the emitter, this one runs the
client. The emitter is `crt-bridge emitter`, a RetroArch build published here:

https://github.com/crt-bridge/RetroArch/releases/tag/v1.22.2-crtbridge.1

Once both are installed and pointed at each other (step 5 below), this document is not needed
again.

## 2. What you need

| Requirement | Where |
|---|---|
| RetroArch 1.9.8 or later | Windows, macOS, or Linux - your normal RetroArch install |
| The client archive for your system | https://github.com/crt-bridge/libretro-crt-bridge/releases |

1.9.8 is where RetroArch learned the core-options interface this core uses; on anything older
the core runs but shows no settings at all. Check your version from
`Main Menu > Information > System Information`, or by running RetroArch with `--version` from a
terminal.

The archive names are `crt-bridge-client-v0.1.0-windows-x64.zip`,
`crt-bridge-client-v0.1.0-linux-x64.tar.gz`, and `crt-bridge-client-v0.1.0-macos-universal.zip`.
The macOS archive is a universal binary: the Apple Silicon (arm64) slice is proven on real
hardware; the Intel (x86_64) slice is built the same way but has not been tested on real Intel
hardware. Both slices target macOS 11 or later.

## 3. Install the core

Every archive contains six files: `crt_bridge_libretro.<ext>` (the core itself),
`crt_bridge_libretro.info` (tells RetroArch the core's name and what it needs), and four text
files - `LICENSE`, `NOTICE.txt`, `README.md`, and this guide (`INSTALL.md`). Only the first two
are installed into RetroArch; the four text files are for you to read and can be left in the
archive, or deleted, once you have. Both installed files go into folders that RetroArch tells you
about - do not guess a path, read it from RetroArch's own settings:

- the core file goes where `Settings > Directory > Cores` points;
- the `.info` file goes where `Settings > Directory > Core Info` points.

If the `.info` file is missing from that second folder, the core still loads, but it appears in
RetroArch's core list under its file name (`crt_bridge_libretro`) instead of
`crt-bridge client` - that is the symptom to look for, not a separate failure; see section 8.

### Windows

1. Extract `crt_bridge_libretro.dll` and `crt_bridge_libretro.info` from the zip archive.
2. Copy `crt_bridge_libretro.dll` into the folder shown under `Settings > Directory > Cores`
   (typically `<RetroArch install>\cores\`).
3. Copy `crt_bridge_libretro.info` into the folder shown under
   `Settings > Directory > Core Info` (typically `<RetroArch install>\info\`).
4. **Check:** both files are present in the folders you just read from RetroArch's own settings.

### macOS

1. Extract `crt_bridge_libretro.dylib` and `crt_bridge_libretro.info` from the zip archive.
2. Copy `crt_bridge_libretro.dylib` into the folder shown under
   `Settings > Directory > Cores` (typically
   `~/Library/Application Support/RetroArch/cores/`).
3. Copy `crt_bridge_libretro.info` into the folder shown under
   `Settings > Directory > Core Info` (typically
   `~/Library/Application Support/RetroArch/info/`).
4. **Check:** `file ~/Library/Application\ Support/RetroArch/cores/crt_bridge_libretro.dylib`
   names an architecture (`arm64`, `x86_64`, or both, for the universal binary).
5. Clear the quarantine flag, if the file came from a browser or AirDrop:
   ```
   xattr -d com.apple.quarantine ~/Library/Application\ Support/RetroArch/cores/crt_bridge_libretro.dylib
   ```
   **Check:** if the file never came through a browser or AirDrop, this prints `No such xattr` -
   that is normal, not an error.

### Linux

1. Extract `crt_bridge_libretro.so` and `crt_bridge_libretro.info` from the tar.gz archive.
2. Copy `crt_bridge_libretro.so` into the folder shown under `Settings > Directory > Cores`
   (typically `~/.config/retroarch/cores/`).
3. Copy `crt_bridge_libretro.info` into the folder shown under
   `Settings > Directory > Core Info` (typically `~/.config/retroarch/cores/` or
   `~/.config/retroarch/info/`, depending on your distribution's RetroArch package).
4. **Check:** `ls ~/.config/retroarch/cores/crt_bridge_libretro.so` prints the path.

## 4. Two RetroArch settings that no core option can set

| Setting | Value | Why | What you see when it is wrong |
|---|---|---|---|
| `Settings > User Interface > Pause Content When Not Active` (`pause_nonactive`) | **Off** | RetroArch's default is On: clicking on another window pauses the core **while the stream keeps arriving**. | The picture freezes as soon as you leave the window, and resumes where it stopped instead of catching up. |
| macOS only: `Settings > Drivers > Video` (`video_driver`) | **`glcore`** | macOS defaults to `metal` or `vulkan`, never `glcore`. A change of video driver needs a RetroArch restart. | With `metal`: smooth only while you move the mouse, less than one frame per second otherwise. With `vulkan`: it works, but it is the slowest of the three drivers tried. |

**Check:** on macOS, after restarting, `Settings > Drivers > Video` reads `glcore`.

## 5. Firewall

The core listens on UDP port 32100 (video and audio) and UDP port 32101 (gamepad, always
`video port + 1`) for the stream coming from the emitter.

- **Windows:** the first time RetroArch opens this core, Windows asks whether to allow it
  network access - allow it on private networks. If you missed that prompt or use a
  third-party firewall, open UDP 32100 and 32101 for inbound connections.
- **macOS:** if the built-in firewall is on (`System Settings > Network > Firewall`), accept
  incoming connections for RetroArch when asked, or add it to the allowed list.
- **Linux:** if a firewall is active, open UDP 32100 and 32101 for inbound connections; for
  example, with `ufw`:
  ```
  sudo ufw allow 32100/udp
  sudo ufw allow 32101/udp
  ```

## 6. Start the core

`Main Menu > Load Core > crt-bridge client`, then `Main Menu > Start Core`.

**Check:** with the core running, look for the UDP port it bound:

- macOS or Linux: `lsof -nP -iUDP:32100` prints a line for RetroArch.
- Windows, from a command prompt: `netstat -an | findstr 32100` prints a `UDP` line.

That means the core has bound its port. Nothing has to be arriving yet for this check to pass,
and it needs no log and no emitter. If you changed `UDP listen port` in section 7's settings
table, use that number instead.

**You can start this client and the emitter in either order.** The picture appears by itself:
the emitter repeats the video mode until this client has it. Wait up to 10 seconds, without
touching anything. If there is still no picture after 10 seconds, see "No image at all" in
section 9.

Once the core is running, point the emitter at this machine's address on your local network -
the emitter's own documentation says how.

**If you would rather check the log than the port, you have to turn logging on first.** A fresh
RetroArch configuration writes no log at all: `Logging Verbosity` and `Log to File` are both off
by default, and the `--log-file` flag on its own does NOT turn them on - it is ignored unless
verbosity is enabled too. In `Settings > Logging`, turn `Logging Verbosity` on, set
`Core Logging Level` to `1 (Info)`, and turn `Log to File` on. The log then lands in the folder
shown under `Settings > Directory > System Event Logs`, and it holds a line containing
`[crt-bridge-client] listening UDP 32100` (RetroArch itself prefixes the line, e.g. `[libretro INFO]`).

## 7. The core's settings

| Setting | Default | What it does |
|---|---|---|
| Frame buffer depth (`groovy_depth`) | 1 | How many frames the client holds back before showing them. Higher values ride out network jitter but add that many frames of latency. Lower values respond faster but stutter when the network hiccups. Raise this only if the image stutters. |
| UDP listen port (`groovy_port`) | 32100 | The port this core listens on for the video stream from the emitter PC. The gamepad channel uses this port plus one. Change it only if the default is already taken on this machine. |
| Send gamepad to emitter (`groovy_input`) | enabled | Forwards player 1's gamepad back to the emitter, so a pad plugged into this machine drives the game running on the emitter PC. Turn it off to play with the pad attached to the emitter instead. |

Each of these can be overridden by an environment variable, and the variable wins over the menu.
When that happens the core says so on screen, naming the variable - see the "Something is
wrong" table below.

## 8. The run report

The core always writes `gmc-core.json`, unconditionally and with no setting to turn it off, into
RetroArch's Saves folder. By default RetroArch sorts saves into one folder per core, so the file
lands in `<Saves folder>/crt-bridge client/gmc-core.json`; if you have turned
`Settings > Saving > Save File: Sort into Folders by Core Name` off, the file sits directly in
that folder instead. RetroArch shows you the Saves folder itself under
`Settings > Directory > Save Files` - on macOS this was proven on
`~/Documents/RetroArch/saves/crt-bridge client/gmc-core.json`; on Windows and Linux, read the
same setting rather than assuming a path. The name is fixed and the file is overwritten every
session. **This is the file to send when something goes wrong.**

Honest limit: it is written when you quit, and also once at the moment a frame-rate collapse is
detected - but a crash without a collapse leaves nothing behind.

> The report separates what the core *saw* from what it *concluded*. `stalls` counts every gap
> where the core was not called for more than 300 ms, whatever the reason. `driver_stalls`
> counts the ones RetroArch was awake for - those are the ones worth reporting to us.
> `pauses` counts the ones RetroArch told the core it had paused itself, which is normal and
> yours: clicking on another window does it. A gap in neither bucket usually just means the
> RetroArch menu was open.

## 9. Something is wrong

This table is indexed by what you see, not by which setting you got wrong - if you are calling
for help, you describe a symptom, not a setting. The rows that begin with `On screen:` quote the
core's own messages word for word, so if you read one on screen you can find it here unchanged.

| Symptom | What it means | What to do |
| --- | --- | --- |
| No image at all: a plain black or grey screen, and no on-screen message | Either nothing has arrived yet, or nothing ever will. The core is running fine; it has simply never been given a picture. | First, wait up to 10 seconds, doing nothing: the emitter repeats the video mode by itself until this client has it. If there is still nothing after 10 seconds: quit RetroArch (the run report is written when you quit), then open `gmc-core.json` - section 8 says where it is. `client.gmclient.wire.datagrams` at 0 means nothing at all reached this machine: check that the emitter is running, that it is aimed at this machine's current IP address, and that the network path and firewall (section 5) are open. `client.gmclient.wire.datagrams` above 0 with `client.gmclient.wire.switchres` at 0 means the stream IS reaching you but the video mode never did: tell the person running the emitter. |
| The core appears in RetroArch's core list under its file name (`crt_bridge_libretro`) instead of `crt-bridge client` | The core loaded, but RetroArch never found `crt_bridge_libretro.info` for it. | Copy `crt_bridge_libretro.info` into the folder shown under `Settings > Directory > Core Info` - see section 3. The core still works either way; only its displayed name is affected. |
| Windows asks whether to allow RetroArch network access, the first time you start this core | Normal: Windows always asks the first time a program opens a network port. | Allow it on private networks; see section 5. |
| On screen: `Frame rate collapsed: RetroArch is not iterating this core. Set video_driver to glcore in Settings > Drivers > Video, then restart RetroArch.` (macOS) | RetroArch's video driver is not driving the run loop. On macOS the default is `metal` or `vulkan`, never `glcore`; with `metal` the event loop only wakes up on input. | Settings > Drivers > Video, set `glcore`, quit and restart RetroArch. |
| On screen: `Frame rate collapsed: RetroArch is not iterating this core. Check the video driver and vsync settings.` (Windows or Linux) | RetroArch's video driver is not driving the run loop. | Check `Settings > Drivers > Video` and your vsync settings; if the picture is otherwise fine, this can be reported with the run report attached (section 8). |
| On screen: `Packets are being lost between the emitter and this machine. If the picture stutters, ask the person running the emitter to set audio=off for this machine in GROOVY_FOLLOWERS.` | The network link to this machine loses packets. Sound travels in the same bursts as the picture and breaks first; the picture keeps going. On a lossy link, removing the sound shortens every burst, which may be enough to stop the picture from stuttering. | This cannot be fixed on this machine. Tell the person running the emitter: on THEIR machine, in the `GROOVY_FOLLOWERS` entry for this machine, add `audio=off` (for example `<this machine's address>:32100,audio=off`), then restart the emitter. You lose the sound, not the picture. On a home network, a wired connection instead of Wi-Fi removes the loss; over the Internet it may not. |
| On screen: `Packets are being lost between the emitter and this machine, and no sound is being sent to it: the network link itself is the problem.` | The network link to this machine loses packets, and the emitter sends no sound here - so the losses come from the link alone, not from sound lengthening the bursts. | This cannot be fixed on this machine, and `audio=off` would change nothing: there is already no sound to remove. Tell the person running the emitter that the link loses packets. On a home network, a wired connection instead of Wi-Fi removes the loss; over the Internet it may not. |
| The picture stutters or jerks on a Wi-Fi or remote connection | The same cause as the two rows above, seen from outside - if on-screen notifications are on, one of those two messages appears too. | If the message mentions `audio=off`: same fix, on the emitter's machine - `audio=off` in this machine's `GROOVY_FOLLOWERS` entry. If it says no sound is being sent: only a better link helps. |
| On screen: `Core was paused while the stream kept arriving. Turn off Settings > User Interface > Pause Content When Not Active (pause_nonactive = false).` | RetroArch paused the core when its window lost focus, while the emitter kept sending. The core sees the gap on its first wake-up and says so once per session. | Settings > User Interface, turn `Pause Content When Not Active` off. |
| On screen: `<something>: forced to <value> by <VARIABLE>, menu value ignored` | An environment variable outranks the menu, and one is set in your shell or in a launcher script. This is deliberate: it is how a run is made reproducible. | Either unset the variable named in the message and restart, or accept the forced value. The menu will keep showing the value you picked, and it will keep being ignored. |
| Smooth only while you move the mouse; almost frozen otherwise (macOS) | The same failure as the `glcore` row above, seen from outside. | Same fix: `video_driver` = `glcore`. |
| Freezes when you click on another window, and resumes where it stopped instead of catching up | The same failure as the `pause_nonactive` row above, seen from outside. | Same fix: turn `Pause Content When Not Active` off. |
| It works, but it feels laggy (macOS) | `vulkan` runs at full rate, so the core does not warn about it - but it measured the worst latency of the three drivers tried: p95 41.5 ms against 25.2 ms for `glcore`. | Prefer `glcore` on Mac. |
| RetroArch says it failed to load the core | On Windows or Linux, check that the downloaded file matches your system (`.dll` for Windows, `.so` for Linux). On an Intel Mac, this core's Intel slice is built but has not been tested on real hardware (section 2). | Re-check the archive you downloaded against section 2. |
| The core file seems to be there but macOS will not open it | Quarantine attribute, set when a file arrives through a browser or AirDrop. | `xattr -d com.apple.quarantine <path to the core>` - see section 3. |
| No core settings appear in the menu at all | Your RetroArch predates the core-options interface this core uses. | Check your version (section 2). Update to 1.9.8 or later. |
| The picture is wrong but no message ever appears on screen | On-screen notifications are turned off in RetroArch, so this core's messages are hidden too. | Settings > On-Screen Display > Notifications, or turn logging on and read the log - see section 6 for how. |
| You want the client to be silent | This client has no mute option of its own, on purpose: RetroArch already has one, and two places to look would be one too many. | `Settings > Audio > Mute`. There is also an `Audio Mute` hotkey under `Settings > Input > Hotkeys`, but it has no key assigned by default - assign one if you want it. |
| Nothing above matches | | Send `gmc-core.json` - section 8 says where it is. It records what the core actually did: its effective settings, which of them were forced by the environment, its frame rate, its pauses, and whether a collapse was detected. |

The run report is written when you quit and once when a collapse is detected - a crash without a
collapse leaves nothing behind. That is accepted: a periodic snapshot would not reliably explain
a random crash, and it would cost a recurring stutter on the latency path.

## 10. Removing it

Delete the core file from the folder you copied it into in section 3 (for example,
`crt_bridge_libretro.dylib` on macOS, `crt_bridge_libretro.dll` on Windows,
`crt_bridge_libretro.so` on Linux), and its `.info` file if you want it gone completely.

Two more things this core wrote, both inside RetroArch's own folders, not somewhere of its own
choosing: the `crt-bridge client` folder under RetroArch's Saves directory (section 8's
`gmc-core.json` - or, on a RetroArch install with no Saves directory configured at all, the run
report instead lands in your system's temporary-files folder), and RetroArch's own core options
file for this core (created the first time you change a setting in section 7's table). Delete
either if you want a completely clean uninstall; leaving them behind changes nothing the next
time you install this core again.
