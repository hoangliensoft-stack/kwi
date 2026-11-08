#!/usr/bin/env bash
# Build Kwi as a release Flatpak and install it as a desktop app for this user.
#
#   ./install-desktop.sh              Build, install and add Kwi to the app launcher
#   ./install-desktop.sh --run        Also start Kwi after installing
#   ./install-desktop.sh --clean      Rebuild from scratch
#   ./install-desktop.sh --uninstall  Remove the installed app
#
# The app is installed as org.kde.kwi//local in the user Flatpak installation,
# so it does not touch system packages and needs no sudo. The launcher entry
# comes from ~/.local/share/flatpak/exports/share/applications, which rofi,
# fuzzel and other launchers read through XDG_DATA_DIRS. A "kwi" command is
# also written to ~/.local/bin.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
app=org.kde.kwi
branch=local
wrapper="$HOME/.local/bin/kwi"
exports="$HOME/.local/share/flatpak/exports/share"
run=0
clean=0
uninstall=0

while (($#)); do
    case "$1" in
    --run) run=1 ;;
    --clean) clean=1 ;;
    --uninstall) uninstall=1 ;;
    -h | --help)
        sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "Unknown option: $1" >&2
        exit 2
        ;;
    esac
    shift
done

refresh_launcher() {
    command -v update-desktop-database >/dev/null && update-desktop-database "$exports/applications" 2>/dev/null || true
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -qtf "$exports/icons/hicolor" 2>/dev/null || true
}

if ((uninstall)); then
    if flatpak info --user "$app//$branch" >/dev/null 2>&1; then
        flatpak uninstall --user -y "$app//$branch"
        flatpak override --user --reset "$app"
    else
        echo "$app//$branch is not installed."
    fi

    # Remove the wrapper only if this script wrote it.
    if [[ -f "$wrapper" ]] && grep -q "$app//$branch" "$wrapper"; then
        rm -f "$wrapper"
    fi

    # The local repository was added as a remote by flatpak-builder --install.
    flatpak remotes --user --columns=name | grep -qx kwi-origin && flatpak remote-delete --user --force kwi-origin || true

    refresh_launcher
    echo "Kwi was removed."
    exit 0
fi

if ! command -v flatpak >/dev/null; then
    echo "Flatpak is required: sudo pacman -S flatpak" >&2
    exit 1
fi

args=(--flatpak --install)
((clean)) && args+=(--clean)

echo "==> Building and installing $app//$branch (this can take a while the first time)..."
"$root/build.sh" "${args[@]}"

if ! flatpak info --user "$app//$branch" >/dev/null 2>&1; then
    echo "Installation failed: $app//$branch is not installed." >&2
    exit 1
fi

# Desktops such as Hyprland set QT_QUICK_CONTROLS_STYLE to a style that only
# exists on the host. Inside the sandbox it makes Kwi fail to load its window.
flatpak override --user --unset-env=QT_QUICK_CONTROLS_STYLE "$app"

mkdir -p "$(dirname "$wrapper")"
cat >"$wrapper" <<EOF
#!/bin/sh
# Starts the Kwi build installed by $root/install-desktop.sh.
exec flatpak run $app//$branch "\$@"
EOF
chmod +x "$wrapper"

refresh_launcher

echo
echo "Kwi is installed:"
echo "  - App launcher: search for \"Kwi\" (rofi, fuzzel, ...)"
echo "  - Terminal:     $wrapper"

case ":$PATH:" in
*":$HOME/.local/bin:"*) ;;
*) echo "    ~/.local/bin is not in PATH. Add it to run plain \"kwi\"." ;;
esac

if ((run)); then
    exec flatpak run "$app//$branch"
fi
