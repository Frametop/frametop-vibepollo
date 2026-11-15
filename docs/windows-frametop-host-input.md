# Frametop host input on Windows

Frametop's build can lend this computer's keyboard and mouse to one Frametop
client while its headset is worn (Frametop's host input). The protocol, the
lease and the escape hotkey are the same as on Linux (see
[the Hyprland guide](linux/hyprland.md)); only the way the input is taken
differs.

## Turning it off

It is on by default. This computer's owner turns it off in `sunshine.conf`:

```ini
frametop_host_input = disabled
```

While it is off, a client asking is told `off`. When it is turned off, a lease
already held lapses within 1.5 s.

## What it does

The client renews a 1.5 s lease on the encrypted control stream (packet
`0x3005`) while its headset is worn. Only a client allowed both keyboard and
mouse input gets one, and one client at a time; another is told it's busy.
While a lease holds:

- Low-level keyboard and mouse hooks (`WH_KEYBOARD_LL`, `WH_MOUSE_LL`) take
  every hardware key, button, wheel and pointer event, so Windows and its apps
  see none of them. They go to the client as Linux evdev events (`0x3004`).
  Keys are positional (set 1 scan codes mapped to `KEY_*`), so the client's
  keyboard layout decides what they type.
- The mouse's motion comes from Raw Input (`RIDEV_INPUTSINK` on a hidden
  message-only window): its counts, before Windows' pointer acceleration.
  A device that gives no relative Raw Input counts (a precision touchpad's
  pointer, a tablet or a remote desktop's absolute pointer) moves the client by
  the pointer delta the hook sees instead, which is accelerated and stops at the
  edge of the screen the cursor is on.
- Buttons and the wheel come from the hook. The wheel goes in 120ths of a notch
  (`REL_WHEEL_HI_RES`, `REL_HWHEEL_HI_RES`) with whole notches (`REL_WHEEL`,
  `REL_HWHEEL`).
- The cursor here stays where it was.

These stay here:

- Events Sunshine injects itself (the client's own clicks and keys on this
  desktop) and any other software's injected input: they carry the
  `LLKHF_INJECTED` and `LLMHF_INJECTED` flags, and their Raw Input has no device.
- Volume and media keys.
- Pen and touch input.
- Keys and buttons already held down when the lease starts, until they come up.
- Ctrl+Alt+Del, which no hook can take. Win+L locks the session as usual.

## Giving it back

Both Shifts and Escape take the keyboard and mouse back at once. Then no lease
takes them until the client releases its own: its headset comes off or its
switch goes off. A paused Frametop or a dropped stream keeps the escape;
restarting Vibepollo forgets it. A lapsed lease, the stream ending or the
client losing its permission also gives them back.

Keys and buttons that were down on the client come up there. Ones still held
here stay taken until they come up here (at most 2 s), so a held key doesn't
start repeating on this computer.

When the secure desktop has the input (UAC prompts, Ctrl+Alt+Del) or the
session is locked, the input stays here and the client is told `secure`. It
gets the input again on its next renewal once the desktop is back.

## Security and permissions

- **Who can take the input.** Only a paired client on an encrypted control
  stream with both keyboard and mouse permission. That client can already type
  and click on this desktop through Sunshine, so lending it the local keyboard
  and mouse grants nothing new on this computer. What it adds is that this
  computer's own keyboard and mouse stop working here while the lease holds.
- **On by default.** `frametop_host_input = disabled` turns it off.
- **No driver.** Hooks and Raw Input are standard user-mode APIs. Nothing is
  installed (no Interception or filter driver), and the hooks go away with the
  lease, or with the process.
- **Where it runs.** In `sunshine.exe`, on its own thread. Sunshine's service
  starts it as SYSTEM in the console session; the thread moves to the input
  desktop, as Sunshine's injection does. A low-level hook only sees the desktop
  it was set on, so the secure desktop's and the lock screen's input is never
  seen, and never taken.
- **Escape.** Both Shifts and Escape on this computer always win over the
  client. The escape is for a well-behaved client: one allowed keyboard and
  mouse input could lift it itself by releasing and engaging again, and
  already drives this computer's desktop.
- **What is logged.** Engaging, releasing and why. Key codes and motion are
  never logged.
- **Windows' own limits.** Windows skips a low-level hook that doesn't answer
  in time (`LowLevelHooksTimeout`) and may remove it. The hooks only queue
  events and return; the capture thread runs at high priority.
