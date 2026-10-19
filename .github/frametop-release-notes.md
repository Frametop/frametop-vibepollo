Vibepollo 2.0.0 with Frametop's changes, for streaming a computer's displays to Frametop's remote displays on a Steam Frame (https://github.com/Frametop/frametop).

- Windows: `sunshine.exe` replaces the one in a Vibepollo 2.0.0 install. Frametop's Windows host kit installs it: it checks its hash, keeps the original, and puts it back with `-Undo`. Steps: https://github.com/Frametop/frametop/blob/experimental/host/windows/README.md
- Arch Linux and CachyOS: `vibepollo-*.pkg.tar.zst`, installed with Vibepollo's installer (`sudo bash scripts/linux_install.sh --package ./vibepollo-*.pkg.tar.zst`). Not tested with Frametop yet. Steps: https://github.com/Frametop/frametop/blob/experimental/host/linux/README.md

The changes:

- Windows: a Remote Monitor is released before the display topology is recomposed, and the host's own monitor layout is never reset.
- A Frametop display role streams one of the host's existing displays, on Windows and Linux.
- The HTTPS thread never blocks waiting for a client's TLS close.
- Windows: a Frametop display stream that asks for SDR turns the display's HDR off while it captures it, and back on when the stream ends (with `dd_hdr_option` automatic, the default). An HDR desktop in an SDR stream looked washed out.
- A Frametop client can send its microphone, and apps on the host record it: "Microphone (Steam Streaming Microphone)" on Windows, "Frametop Microphone" on Linux. Only on the encrypted control stream, and only from clients allowed keyboard input.
- Windows: a Frametop display stream switches the display to the stream's resolution (or the closest mode the display lists with the same shape) while it streams, and back when it ends, instead of scaling the display's own resolution into the stream (with `dd_resolution_option` automatic, the default). HDR goes off before the resolution changes.

Each file has its SHA-256 next to it (`.sha256`). The source is this tag, under GPL-3.0 like Vibepollo.
