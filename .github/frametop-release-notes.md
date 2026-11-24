Vibepollo with Frametop's changes, for streaming a computer's displays to a Steam Frame with Frametop (https://github.com/Frametop/frametop). Built on upstream Vibepollo 2.0.2. It's numbered 2.0.6 so update checks see it as newer than 2.0.4 (2.0.5 wasn't published).

## Install

- Windows, the quick way: run `FrametopHostSetup.exe`. It runs Frametop's Windows host kit from Frametop's `main` branch, which installs Vibepollo 2.0.3 if needed and the build Frametop pins, checking every file's SHA-256. It isn't signed: Windows may say "Windows protected your PC" (More info, then Run anyway).
- Windows: `sunshine.exe`, with `frametop_clipboard.exe` and `sunshine_wgc_capture.exe` for its `tools` folder, go into a Vibepollo 2.0.3 install (Nonary's `VibepolloSetup-v2.0.3.exe`). Frametop's Windows host kit installs them: it checks each hash, keeps the originals, and puts them back with `-Undo`. Steps: https://github.com/Frametop/frametop/blob/experimental/host/windows/README.md
- Arch Linux and CachyOS: `vibepollo-*.pkg.tar.zst`, installed with Vibepollo's installer (`sudo bash scripts/linux_install.sh --package ./vibepollo-*.pkg.tar.zst`). Steps: https://github.com/Frametop/frametop/blob/experimental/host/linux/README.md

## New in 2.0.6

- **Monitor plans (Windows, Hyprland, KDE Plasma):** Frametop can turn physical monitors on or off while you stream (including turning off monitors without a panel), switch a monitor's resolution to match its panel's stream size, and pick a per-monitor scale on Hyprland and KDE Plasma. Monitors go back to their previous state and places 15 seconds after the last display stream ends or on clean shutdown (keeping any manual changes made in between), and changes are journaled so a crash restores them on restart. If a monitor refuses a requested size and falls back, it is restored to its own mode, remembered for the run, and reported to Frametop.
- **`FrametopHostSetup.exe` (Windows):** Windows host setup you can double-click (see Install).
- **App windows (Hyprland):**
  - Per-window scaling, set at open or changed live (scale-only changes keep the pixel size so the stream keeps running, and the pointer follows).
  - Shell bars recreated after a resize or scale change move back under the fullscreen app window right away instead of sitting on top of it.
  - Web app windows (such as Omarchy web apps) are matched across any Chromium-based browser (Chrome, Brave, etc.), not only Chrome's window class.
- **Hyprland pointer accuracy:** the Frame's pointer lands where you point, including when displays don't start at the top left and as displays or window outputs come and go. Captures follow the live output layout, restart when it changes, and idle outputs notice layout changes too.
- **Frametop Together (Linux / Hyprland):** send this PC's open app windows as panels (3 to 6) into your headset's Together room via `ft-pcsend`, encoded in HEVC only while watched, adapting bitrate and frame rate on the fly. (`ft-pcsend` is built separately from Frametop's `together/pcsend`.)
- **Shared clipboard (`ext-data-control-v1`, Linux):** `vibepollo-clipboard` now supports the standard Wayland `ext-data-control-v1` protocol (with `wlr-data-control` fallback), keeping clipboard sharing working on KDE Plasma 6.6+ and newer Wayland compositors that dropped `wlr-data-control`.
- **OpenSSL 4 compatibility:** updated certificate generation and signing APIs for OpenSSL 4.

## Heads up: host input is on by default

Using this computer's keyboard and mouse from the Frame used to be off until you turned it on. It's now on for new installs and for the first upgrade from a build that shipped it off. A Frame still has to be paired, and it only borrows the input when you ask in the headset. Both Shift keys + Esc takes it back at once.

- Linux: turn it off with `sudo systemctl disable --now vibepollo-host-input.socket`. If you disable or mask it, later upgrades leave it off.
- Windows: set `frametop_host_input = disabled` in the config.

## New in 2.0.4: both

- App windows: put a single window of this computer on its own panel in the Frame, or open an installed app straight onto a panel, sized to the panel. Released, it goes back where it came from, and so does a window nobody has streamed for a minute. Steps: https://github.com/Frametop/frametop-vibepollo/blob/frametop/hyprland/docs/frametop-app-windows.md
- Shared clipboard: text and PNG images go both ways between the Frame and this computer when Frametop's "Share the clipboard" is on for the host.
- Keyboard and mouse borrowing: you can lend just the keyboards or just the mice.

## New in 2.0.4: Windows

- Host input: the Frame can now borrow this computer's keyboard and mouse on Windows too.
- App windows include Store apps and windows on other virtual desktops. Windows out on panels move to a virtual display of their own, which goes away 15 s after the last one is back. Without the virtual display driver they stay on their monitors. The tray's **Bring windows back** puts every window back.
- A window's popups and menus are drawn into its panel.

## New in 2.0.4: Linux (Arch / CachyOS)

- Hyprland: HDR (PQ) outputs are tone mapped to SDR on the GPU before encoding, with NVENC or VAAPI. Set `hdr_sdr_white_nits` to the output's SDR brightness (`sdrMaxLuminance`).
- Hyprland: NVENC can encode wlr captures.
- The Frame's mic works with the system service, with less delay and faster recovery.
- App windows on Hyprland come from a second Vibepollo in your Hyprland session. The package installs it and leaves it off: turn it on with `vibepollo-frametop-windows setup` in a terminal in that session.

## Known limits

- Windows: the Frame's mic needs Steam installed (it provides the Steam Streaming Microphone).
- Windows: app windows show no app icons yet.
- Windows: a monitor on a different GPU than the encoder is refused, even when both GPUs are the same brand (for example two NVIDIA cards). Point the encoder and adapter at that GPU.
- Windows: the Web UI still offers Nonary's updates. Don't install them, they replace this build.
- App windows: KDE Plasma and other Linux desktops don't have them in Vibepollo yet. Frametop 0.3.0-exp.25 can show a KDE PC's windows through KWin Window Stream instead.
- Hyprland: displays must already be on, no virtual displays, touch isn't mapped, rotated displays aren't placed. HLG outputs, and HDR outputs under the software or Vulkan encoders, are refused.

Each file has its SHA-256 next to it (`.sha256`). The source is this tag, under GPL-3.0 like Vibepollo.
