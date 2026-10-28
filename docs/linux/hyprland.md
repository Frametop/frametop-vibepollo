# Experimental Hyprland physical-display host

The native machine package has an administrator opt-in for a limited Hyprland
session. KDE/Plasma remains the default. This backend has component coverage;
live Hyprland/NVIDIA capture, application, login and suspend validation is still
required before claiming this platform works on a particular machine.

Use a Hyprland session started by SDDM through **Hyprland (uwsm-managed)**,
using UWSM's `hyprland.desktop` entry. The controller still requires the chosen
local desktop account, active Wayland logind session and seat0. UWSM must publish
its current `WAYLAND_DISPLAY` to the user manager and finish starting
`wayland-wm@hyprland.desktop.service`,
`wayland-session@hyprland.desktop.target` and `graphical-session.target`.
The controller observes these units and probes the validated user-owned socket.
Its peer UID must match the selected user, and its process must belong to the
fixed UWSM unit's cgroup, including the packaged start-hyprland watchdog;
it does not start a compositor or import arbitrary environment files. Each UWSM
compositor invocation, peer PID and process start time are bound to the trusted machine session record, so a
watchdog child restart or unit restart replaces the host generation even if the socket name repeats.
An observed stopped unit or changed invocation revokes the previous generation
even when its replacement socket is unavailable. Failed metadata queries alone
retain suspend-transition retry behavior and never admit an unverified replacement.
See [Hyprland's systemd startup guide](https://wiki.hypr.land/Useful-Utilities/Systemd-start/).

Install the native package manually following [the Linux installation guide](install.md#install-manually).
The recommended installer script retains its Plasma requirements. After package
configuration selects the intended desktop account, an administrator can add
this setting to the existing root-owned mode 0600 `/etc/vibepollo/machine.conf`:

```ini
desktop_backend=hyprland-kms
```

Keep the existing `desktop_user` line. The package's configure/upgrade path
preserves this selection; omitting the setting selects `kwin`. A policy or
session change stops the host and its applications before binding a new
generation, interrupting active streams. The Plasma Login greeter continues to
use the KWin backend, independent of the desktop policy. SDDM greeter streaming
is not provided.

The supported slice captures an already enabled **physical SDR output through
KMS**, with audio/input and administrator-authorized native Wayland application
commands. Set `output_name` to an exact physical connector name from display
enumeration before streaming; empty and numeric output aliases are rejected.
Capture checks that selected connector's current DRM `HDR_OUTPUT_METADATA`
state at initialization and before delivering frames. A zero blob or valid
static metadata with SDR EOTF is required; missing/unreadable metadata,
unsupported metadata types and every HDR EOTF fail closed. Display enumeration
alone does not certify SDR state. The user must disable HDR in Hyprland on that
connector; the host does not change its color pipeline or perform HDR-to-SDR
conversion. Connectors without a readable standard HDR property are excluded
from this experimental capture policy. The backend forces KMS and disables the global
private-display default for that host launch. Saved Plasma output topology is
not applied. Existing managed kernel outputs and configured reserved outputs
are excluded from capture selection.

These operations are explicitly unsupported: wlr capture in the machine-service
account, private/Remote Monitor creation, display modes/layout/scale/HDR changes,
DPMS wake/power control, touchscreen mapping and KWin application focus. Physical
outputs must be enabled by the desktop user. HDR or private-output stream requests
fail with an explanation. The backend advertises no independent-output capability.
KScreen and managed-connector operations are fenced even if called outside the
normal backend route.

Frametop's build adds its display role to this slice. A `frametopDisplay`
launch can stream one of these physical outputs, named by the connector that
display enumeration lists. Enumeration gives each output with an active scanout
its KMS size and refresh rate; its layout position, scale and primary display
aren't listed. The machine service has no Wayland connection, so for absolute
mouse input the session broker's read-only `display-layout` request reports
each output's logical position and size (xdg-output), matched to its KMS output
by exact connector name. The pointer then lands on the right spot with any
arrangement or fractional scale. If the layout can't be read, the host logs a
warning and treats each display as if it sat at the top left, unscaled. Rotated
outputs aren't placed. Touch devices aren't mapped to an output here. The
display role takes no display power guard here, so the display must already be
on: if Hyprland's idle handling turns it off, capture fails. HDR outputs fail
closed as above.

Frametop's build can also lend this computer's keyboards and mice to one
Frametop client (Frametop's host input). It is off until this computer's owner
turns it on:

```bash
sudo systemctl enable --now vibepollo-host-input.socket
```

Until then a client asking is told `off`. Once on, the client renews a 1.5 s
lease on the encrypted control stream (packet `0x3005`) while its headset is
worn, and only a client allowed both keyboard and mouse input gets one. While a
lease holds, `vibepollo-host-input.service` grabs the keyboards and mice
(`EVIOCGRAB`), so the desktop here sees nothing from them, and sends their key,
button, motion and wheel events to the client (`0x3004`). Bluetooth keyboards
and mice (uhid) are taken too. Touchpads' multitouch nodes, touchscreens,
gamepads and uinput devices (the host's own passthrough devices among them) are
never taken. A device is grabbed only once no key is held on it (a key pressed and released within the few microseconds of the grab itself could still stay down here; no human press is that short). Volume, media
and brightness keys stay here: they are typed again on a uinput device of the
service's own.

Both Shifts and Escape take the keyboard and mouse back at once (the Shifts may be on different keyboards). Then no lease
takes them until the client releases its own: its headset comes off or its
switch goes off. A paused Frametop or a dropped stream keeps the escape, and so does the capture service restarting (restarting Vibepollo forgets it). The escape is for a well-behaved client: one allowed keyboard and mouse input could lift it itself, and already drives this computer's desktop. A
lapsed lease, the stream ending, the client's permission going, its control
stream falling behind, the kernel dropping a device's events, or the machine host stopping also gives them back.
Keys and buttons that were down on the client come up there. One client at a
time holds them; another is told it's busy.

The service is socket-activated (`vibepollo-host-input.socket`,
`/run/vibepollo/host-input.sock`, group `vibepollo`) and serves only the
machine host's uid. It runs as a dynamic user in the `input` and
`vibepollo-uinput` groups, with no capabilities, no network and access only to
input devices and `/dev/uinput`. Key codes are never logged.

On a hybrid laptop, the encoder must run on the GPU the displays are wired to.
KMS capture for NVENC skips a GPU it can't import framebuffers from, so an
Intel- or AMD-wired display would stream no video. With no `encoder` set, the
host tries NVENC after VA-API (but still before software encoding) when NVENC
can capture fewer physical displays than VA-API can. Each encoder probes on a
physical display it can capture. For Intel this needs `intel-media-driver`.
If a display moving between GPUs (at a dock, say) changes that comparison, the
host probes again. A tie keeps NVENC first. A `frametopDisplay` launch for a
display the chosen encoder can't capture fails with status 503 and names the
display. To choose the GPU yourself, set `encoder = vaapi` and `adapter_name`
to that GPU's render node (see `/dev/dri/by-path`).

Hyprland's Xwayland runs without an Xauthority file, and the controller's
private `/tmp` cannot see `/tmp/.X11-unix`. The controller treats UWSM's
published `DISPLAY` as a candidate only: it connects to that socket through the
bound compositor's `/proc/PID/root` and records `xdisplay=:N` only when the
listening peer is the compositor PID and start time already bound to the
session. `XAUTHORITY` is never imported, and inherited user-manager values are
removed from launched services; only the recorded display is set. A retryable
probe failure keeps a display already bound to the same compositor, so it does
not restart the host. Without a bound display the session stays Wayland-only.

With a bound display, Steam/Big Picture, Steam-direct and Lutris launches use the
same broker operations as KDE; Proton games then run on Hyprland's Xwayland. HDR
compatibility launches and the external Proton stream policy remain
unsupported.

Before promoting support, test physical KMS/NVENC or VAAPI capture, audio/input,
a native Wayland application, logout/login and greeter transitions, compositor
restart with a reused socket, suspend/resume and upgrade cleanup. Verify that
private/HDR requests fail without changing outputs, that Steam/Lutris launches
are refused without a bound Xwayland display, that old compositor
applications are cancelled, and that switching the policy back to `kwin`
preserves the existing Plasma behavior. No live compositor test is implied by
the component fixtures.
