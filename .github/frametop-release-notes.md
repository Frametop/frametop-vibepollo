Vibepollo with Frametop's changes, for streaming a computer's displays to a Steam Frame with Frametop (https://github.com/Frametop/frametop). Built on upstream Vibepollo 2.0.2. It's numbered 2.0.3 so update checks see it as newer than the 2.0.2 builds.

## Install

- Windows: `sunshine.exe` replaces the one in a Vibepollo 2.0.1 install. Frametop's Windows host kit installs it: it checks the hash, keeps the original, and puts it back with `-Undo`. Steps: https://github.com/Frametop/frametop/blob/experimental/host/windows/README.md
- Arch Linux and CachyOS: `vibepollo-*.pkg.tar.zst`, installed with Vibepollo's installer (`sudo bash scripts/linux_install.sh --package ./vibepollo-*.pkg.tar.zst`). Steps: https://github.com/Frametop/frametop/blob/experimental/host/linux/README.md

## Windows

- Your Frame's mic shows up on the PC as "Microphone (Steam Streaming Microphone)", so games and voice chat can use it.
- If a monitor is already streaming to another device, you're told so instead of getting a blank stream. Renamed monitors are followed.
- A monitor plugged into a different GPU than the one encoding now fails right away with a clear message, instead of connecting to a black screen.
- Still here: the monitor switches to the stream's resolution, and HDR turns off for an SDR stream, until you stop.

## Linux (Arch / CachyOS)

- Hyprland: stream your existing physical displays to the Frame.
- Hybrid-GPU laptops: the host picks the GPU that drives your screens, and checks again when a display moves (dock, lid, external monitor).
- The mouse lands where you point across multi-monitor layouts and scaling.
- New: use this computer's keyboard, mouse and touchpad from the Frame. Turn it on with `sudo systemctl enable --now vibepollo-host-input.socket`. Both Shift keys + Esc takes them back at once.
- Borrowed input is smooth (no periodic mouse hitching), and touchpads get tap-to-click and two-finger scrolling.
- Updates and the tray follow Frametop's releases.

## Updates

From this release on, the update check reads Frametop's `frametop-<version>` release tags, so later releases are offered in the tray and the Linux Web UI. Builds from before this release don't read these tags, so update to this one by hand.

## Known limits

- Windows: a monitor on a different GPU than the encoder is refused, even when both GPUs are the same brand (for example two NVIDIA cards). Point the encoder and adapter at that GPU.
- Windows: the Web UI still offers Nonary's updates. Don't install them, they replace this build.
- Linux: the Frame's mic doesn't work with the system service yet.
- Hyprland: displays must already be on, SDR only, no virtual displays, touch isn't mapped, rotated displays aren't placed.
- Keyboard and mouse borrowing is Linux only for now.

Each file has its SHA-256 next to it (`.sha256`). The source is this tag, under GPL-3.0 like Vibepollo.
