#!/usr/bin/bash
# The Frametop app windows launcher against a stand-in Vibepollo and systemctl, and what
# packages it.
set -euo pipefail
root=$1
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
fail_test() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# A Vibepollo that records how it was run, and saves a login the way --creds does.
cat >"$work/host" <<EOF
#!/usr/bin/bash
printf '%s\n' "\$@" >"$work/argv"
env >"$work/env"
if [[ \${2:-} == --creds ]]; then
  cat >"$work/input"
  printf '{"username": "%s"}\n' "\$3" >"$work/config/vibepollo-frametop-windows/credentials.json"
fi
EOF
chmod +x "$work/host"
mkdir -p "$work/run"
export XDG_CONFIG_HOME=$work/config XDG_RUNTIME_DIR=$work/run
export VIBEPOLLO_FRAMETOP_WINDOWS_HOST=$work/host VIBEPOLLO_MACHINE_HOST=1
unset HYPRLAND_INSTANCE_SIGNATURE
# shellcheck source=/dev/null
source "$root/packaging/linux/vibepollo-frametop-windows"

conf=$work/config/vibepollo-frametop-windows/sunshine.conf
ran() { [[ -e $work/argv ]]; }

# Nothing runs before setup made the settings and a login.
if (run) 2>"$work/err"; then fail_test 'ran without settings'; fi
grep -q 'vibepollo-frametop-windows setup' "$work/err" || fail_test 'no settings: setup not named'

write_settings
[[ $(stat -c %a "$conf") == 600 && $(stat -c %a "$(dirname "$conf")") == 700 ]] || fail_test 'settings readable by others'
for line in 'port = 48989' 'capture = wlr' 'virtual_display_mode = disabled' \
  "credentials_file = $work/config/vibepollo-frametop-windows/credentials.json" \
  "pkey = $work/config/vibepollo-frametop-windows/credentials/cakey.pem"; do
  grep -qxF -- "$line" "$conf" || fail_test "settings lack: $line"
done
if grep -q '^output_name' "$conf"; then fail_test 'settings pin one output'; fi
[[ $(settings_port) == 48989 ]] || fail_test 'port not read from the settings'
printf 'port = 49989\n' >>"$conf"
write_settings
grep -qx 'port = 49989' "$conf" || fail_test "the owner's settings were overwritten"
[[ $(settings_port) == 49989 ]] || fail_test "the owner's port not read"

if (run) 2>"$work/err"; then fail_test 'ran without a login'; fi
grep -q 'no Web UI login' "$work/err" || fail_test 'no login: not said'
if ran; then fail_test 'Vibepollo started without a login'; fi

# The password reaches Vibepollo on its input, never as an argument.
if (save_login 'bad name' secret) 2>/dev/null; then fail_test 'a user name with a space taken'; fi
if (save_login alice '') 2>/dev/null; then fail_test 'an empty password taken'; fi
if ran; then fail_test 'Vibepollo called for a refused login'; fi
save_login alice 'pass word $x'
[[ $(<"$work/argv") == "$conf"$'\n'--creds$'\n'alice$'\n'- ]] || fail_test "login arguments: $(tr '\n' ' ' <"$work/argv")"
[[ $(<"$work/input") == 'pass word $x' ]] || fail_test 'the password did not arrive on the input'
has_login || fail_test 'saved login not seen'
rm -f -- "$work/argv"

# Outside Hyprland it ends quietly, so its unit doesn't restart it for nothing.
(run) >/dev/null || fail_test 'no Hyprland session: failed'
if ran; then fail_test 'Vibepollo started without Hyprland'; fi

# Hyprland's socket is found by its signature, or as the one there is.
python3 - "$work/run/hypr/abc" <<'EOF'
import os, socket, sys
os.makedirs(sys.argv[1])
socket.socket(socket.AF_UNIX).bind(sys.argv[1] + "/.socket.sock")
EOF
hyprland_runs || fail_test "Hyprland's socket not found"
if (HYPRLAND_INSTANCE_SIGNATURE=other; hyprland_runs); then fail_test "another session's Hyprland taken"; fi
(HYPRLAND_INSTANCE_SIGNATURE=abc; hyprland_runs) || fail_test 'Hyprland not found by its signature'

# In the session it becomes Vibepollo, as the session's own.
(run)
[[ $(<"$work/argv") == "$conf" ]] || fail_test "run arguments: $(tr '\n' ' ' <"$work/argv")"
grep -qx 'VIBEPOLLO_SESSION_BACKEND=hyprland-wlr' "$work/env" || fail_test 'not started as the Hyprland session backend'
if grep -q '^VIBEPOLLO_MACHINE_HOST=' "$work/env"; then fail_test 'started as the machine host'; fi

# "back" restarts its unit, which is what puts the windows back.
user_systemd() { return 0; }
fixture_active=1
systemctl() {
  printf '%s\n' "$*" >>"$work/systemctl"
  case "$*" in *is-active*) return "$fixture_active" ;; esac
}
if (back) 2>/dev/null; then fail_test 'put windows back with nothing running'; fi
fixture_active=0
back >/dev/null
grep -qx -- '--user restart vibepollo-frametop-windows.service' "$work/systemctl" || fail_test 'back did not restart its unit'

# The package carries it, and opens nothing on its own.
linux=$root/packaging/linux
grep -qx 'ExecStart=@CMAKE_INSTALL_FULL_BINDIR@/vibepollo-frametop-windows run' "$linux/vibepollo-frametop-windows.service.in" ||
  fail_test "the unit doesn't run the launcher"
grep -qx 'WantedBy=graphical-session.target' "$linux/vibepollo-frametop-windows.service.in" || fail_test 'the unit is not for the desktop session'
grep -qx 'ports=48984,48989,48990,49010/tcp|48998:49000,49010/udp' "$linux/ufw/vibepollo-frametop-windows" || fail_test 'ufw profile ports'
for open_port in 'protocol="tcp" port="48989"' 'protocol="tcp" port="48990"' 'protocol="udp" port="48998-49000"'; do
  grep -qF -- "$open_port" "$linux/firewalld/vibepollo-frametop-windows.xml" || fail_test "firewalld lacks $open_port"
done
grep -qF 'linux_frametop_windows.cmake' "$root/cmake/packaging/linux.cmake" || fail_test 'not in the Linux package'
for packaged in '%{_bindir}/vibepollo-frametop-windows' '%{_userunitdir}/vibepollo-frametop-windows.service' \
  '%{_prefix}/lib/firewalld/services/vibepollo-frametop-windows.xml' '%{_sysconfdir}/ufw/applications.d/vibepollo-frametop-windows'; do
  grep -qxF -- "$packaged" "$linux/copr/Sunshine.spec" || fail_test "RPM lacks $packaged"
done
if grep -q 'frametop-windows' "$linux/vibepollo-postinst.in" "$linux/Arch/vibepollo.install"; then
  fail_test 'the package turns it on by itself'
fi

# Staged as the Linux package stages it: the launcher beside Vibepollo, its unit naming it.
mkdir "$work/project"
cat >"$work/project/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
project(frametop_windows_stage NONE)
include(GNUInstallDirs)
set(SYSTEMD_FOUND TRUE)
set(SYSTEMD_USER_UNIT_INSTALL_DIR /usr/lib/systemd/user)
include("$root/cmake/packaging/linux_frametop_windows.cmake")
EOF
cmake -S "$work/project" -B "$work/build" -Wno-dev -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/etc >/dev/null
DESTDIR=$work/stage cmake --install "$work/build" >/dev/null
[[ $(stat -c %a "$work/stage/usr/bin/vibepollo-frametop-windows") == 755 ]] || fail_test 'the launcher is not installed to run'
grep -qx 'ExecStart=/usr/bin/vibepollo-frametop-windows run' "$work/stage/usr/lib/systemd/user/vibepollo-frametop-windows.service" ||
  fail_test "the installed unit doesn't name the installed launcher"
[[ -e $work/stage/usr/lib/firewalld/services/vibepollo-frametop-windows.xml &&
  -e $work/stage/etc/ufw/applications.d/vibepollo-frametop-windows ]] || fail_test 'firewall profiles not installed'
cmake -S "$work/project" -B "$work/build-steamos" -Wno-dev -DCMAKE_INSTALL_PREFIX=/usr -DSUNSHINE_BUILD_STEAMOS=ON >/dev/null
DESTDIR=$work/stage-steamos cmake --install "$work/build-steamos" >/dev/null
[[ ! -e $work/stage-steamos ]] || fail_test 'installed in a SteamOS build'
printf 'ok\n'
