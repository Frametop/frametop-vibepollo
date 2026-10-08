Vibepollo 2.0.0 with Frametop's changes, for streaming a computer's displays to Frametop's remote displays on a Steam Frame (https://github.com/DeeJanuz/frametop).

- Windows: `sunshine.exe` replaces the one in a Vibepollo 2.0.0 install. Frametop's host setup (`host/windows/Setup Frametop host.cmd` in Frametop) checks its hash, keeps the original, and puts it back with `-Undo`.
- Arch Linux: `vibepollo-*.pkg.tar.zst`, installed with `pacman -U`.

The changes:

- Windows: a Remote Monitor is released before the display topology is recomposed, and the host's own monitor layout is never reset.
- A Frametop display role streams one of the host's existing displays.
- The HTTPS thread never blocks waiting for a client's TLS close.

Each file has its SHA-256 next to it (`.sha256`). The source is this tag, under GPL-3.0 like Vibepollo.
