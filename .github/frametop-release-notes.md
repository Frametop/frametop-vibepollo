Vibepollo 2.0.0 with Frametop's changes, for streaming a computer's displays to Frametop's remote displays on a Steam Frame (https://github.com/Frametop/frametop).

- Windows: `sunshine.exe` replaces the one in a Vibepollo 2.0.0 install. Frametop's Windows host kit installs it: it checks its hash, keeps the original, and puts it back with `-Undo`. Steps: https://github.com/Frametop/frametop/blob/experimental/host/windows/README.md
- Arch Linux and CachyOS: `vibepollo-*.pkg.tar.zst`, installed with Vibepollo's installer (`sudo bash scripts/linux_install.sh --package ./vibepollo-*.pkg.tar.zst`). Not tested with Frametop yet. Steps: https://github.com/Frametop/frametop/blob/experimental/host/linux/README.md

The changes:

- Windows: a Remote Monitor is released before the display topology is recomposed, and the host's own monitor layout is never reset.
- A Frametop display role streams one of the host's existing displays, on Windows and Linux.
- Update checks, release notes and the Linux tray use Frametop's releases, never Nonary's, whose install would replace this build. On Windows only `sunshine.exe` is replaced, so the stock Web UI still checks Nonary's releases: don't install an update from it.
- Linux on Hyprland: Vibepollo's experimental Hyprland session (`desktop_backend=hyprland-kms`, a UWSM-managed Hyprland started by SDDM) can stream its SDR physical displays to Frametop with the display role. The display must already be on, and its position and scale aren't known. Virtual displays and HDR aren't supported there. On a hybrid laptop the host picks an encoder on the GPU the displays are wired to, and refuses a display its encoder can't capture instead of streaming no video. Steps: `docs/linux/hyprland.md`.
- The HTTPS thread never blocks waiting for a client's TLS close.
- Windows: a Frametop display stream that asks for SDR turns the display's HDR off while it captures it, and back on when the stream ends (with `dd_hdr_option` automatic, the default). An HDR desktop in an SDR stream looked washed out.
- A Frametop client can send its microphone, and apps on the host record it: "Microphone (Steam Streaming Microphone)". Windows only for now: Vibepollo's Linux service can't add a microphone to the desktop yet. Only on the encrypted control stream, and only from clients allowed keyboard input.
- Windows: a Frametop display stream switches the display to the stream's resolution (or the closest mode the display lists with the same shape) while it streams, and back when it ends, instead of scaling the display's own resolution into the stream (with `dd_resolution_option` automatic, the default). HDR goes off before the resolution changes.

Each file has its SHA-256 next to it (`.sha256`). The source is this tag, under GPL-3.0 like Vibepollo.
