# Frametop app windows

Frametop's build can show single app windows of this computer as panels in a
Frametop headset, next to its whole displays. Frametop lists the windows and
installed apps in its launcher, and each one opened there streams on its own.
Windows and Hyprland (Linux) have it, with the same API, so Frametop treats
both alike. KDE Plasma and other Linux desktops don't have it yet.

| | Windows | Linux (Hyprland) |
| --- | --- | --- |
| Who shares the windows | The Vibepollo service | A second Vibepollo in the desktop session |
| HTTP port (Web UI one above) | 47989 | 48989 |
| Web UI login | The service's | Its own, made by `vibepollo-frametop-windows setup` |
| A window's output | `window:0x<HWND>`, captured with Windows Graphics Capture | A headless output of its own, `FT-W1`, `FT-W2`, ... |
| Turning it on | Nothing to do | `vibepollo-frametop-windows setup` |
| Putting every window back | Tray, **Bring windows back** | Tray, **Bring windows back**, or `vibepollo-frametop-windows back` |

## In the headset

In Frametop Settings, Remote displays, on this computer's card, turn on **Show
its apps and windows in the launcher** and sign in with the Web UI login from
the table. Frametop looks for the windows on port 48989 first, then 47989, and
keeps a token that can only pair its own streams and use the window routes
below. The password isn't kept.

## Windows

The service shares the windows of whoever is signed in. Nothing is turned on:
the routes answer on the service's own ports. With nobody signed in they answer
503, "No one is signed in to this PC's desktop". A window keeps its place on
its monitor, is sized to the panel, and goes back as it was when released.

## Linux (Hyprland)

A window gets a headless output of its own, which only a Vibepollo inside the
desktop session can make and capture. The machine service runs outside it and
keeps streaming the real monitors, so a second Vibepollo runs as the desktop
user, with its own settings, Web UI login and certificate, in
`~/.config/vibepollo-frametop-windows`. The package installs it and leaves it
off. In a terminal in the Hyprland session:

```bash
vibepollo-frametop-windows setup
```

It asks for a Web UI user name and password, writes the settings, and starts
`vibepollo-frametop-windows.service` with the desktop session from then on
(`graphical-session.target`, which a Hyprland started through UWSM has). In a
session without one it prints the `exec-once` line to add to Hyprland's
settings instead. It never asks for root.

| Command | What it does |
| --- | --- |
| `vibepollo-frametop-windows setup` | Makes the settings and the login, and starts it with the session |
| `vibepollo-frametop-windows login` | Changes the Web UI login |
| `vibepollo-frametop-windows status` | Whether it runs, its ports and its files |
| `vibepollo-frametop-windows back` | Puts every window back where it came from |
| `vibepollo-frametop-windows off` | Stops it, and doesn't start it with the session |

Its ports are TCP 48984, 48989, 48990 and 49010, and UDP 48998 to 49000 and
49010. The package carries a firewall profile for them and opens nothing by
itself:

```bash
sudo ufw allow 'Vibepollo Frametop windows'
# or
sudo firewall-cmd --permanent --add-service=vibepollo-frametop-windows && sudo firewall-cmd --reload
```

The machine service answers the window routes with 503 and names the command
above, so a Frametop that only reaches the machine service says what's missing.

An opened window moves to its output fullscreen (the compositor's fullscreen
only: the app isn't told). Released, it goes back to the workspace it came
from, and its output is removed. Stopping or restarting the session's
Vibepollo puts every window back, which is what `back` and the tray entry do;
their streams end. After a crash the next start puts back what was left out.

Known limits:

- The encoder must run on the GPU Hyprland renders with. On a computer where
  the encoder is on another GPU, a window's stream shows no picture (the log
  says `[wayland] Frame capture failed`). With Hyprland on an NVIDIA GPU, set
  `encoder = nvenc` and that GPU's render node as `adapter_name`.
- On Omarchy, a window taken from a workspace outside its monitor's numbered
  block (workspace 77 on a monitor that owns 1 to 10) comes back on a
  workspace Omarchy's workspace script picks, as that script renumbers such
  workspaces whenever an output comes or goes.
- While the session is locked, window outputs stream black.

## API

All routes are on the Web UI port, with the Web UI login or a token whose
scopes list them.

| Route | Answer |
| --- | --- |
| `GET /api/frametop/windows` | `windows`, the `outputs` open now, and whether the session is `locked` |
| `GET /api/frametop/windows/apps` | Installed apps a launch can name |
| `POST /api/frametop/windows/open` | Opens a window (`address`) or launches an app (`app`), at `width` x `height`: its `output` |
| `POST /api/frametop/windows/resize` | Resizes a window output |
| `POST /api/frametop/windows/at` | Which window is at a point |
| `POST /api/frametop/windows/release` | Puts a window back, named by its `output` or its `address` |

A Frametop display stream of the output is the window. An output no stream
captures for a minute is released, and so is one whose window closed.
