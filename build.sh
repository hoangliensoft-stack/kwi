#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause
#
# Build Kwi, optionally running the tests or the app.
#
#   ./build.sh                 Build (Flatpak if cmake is missing, else native)
#   ./build.sh --test          Build and run the unit tests
#   ./build.sh --test -R foo   Build and run tests matching "foo" (extra args go to ctest)
#   ./build.sh --run           Build and start Kwi
#   ./build.sh --run --no-build  Start the last build without rebuilding
#   ./build.sh --flatpak       Force a Flatpak build
#   ./build.sh --native        Force a native build (needs cmake, Qt 6.11, KF6, libghostty-vt)
#   ./build.sh --clean         Remove the build directory first
#   ./build.sh --install       Build a release and install it as org.kde.kwi//local for this user (Flatpak)
#
# Flatpak builds go to build-flatpak/ and use the org.kde.Sdk runtime from Flathub.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode=""
test=0
run=0
clean=0
install=0
build=1
ctest_args=()

while (($#)); do
    case "$1" in
    --flatpak) mode=flatpak ;;
    --native) mode=native ;;
    --test) test=1 ;;
    --run) run=1 ;;
    --clean) clean=1 ;;
    --install) install=1 ;;
    --no-build) build=0 ;;
    -h | --help)
        sed -n '5,17p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) ctest_args+=("$1") ;;
    esac
    shift
done

if ((${#ctest_args[@]} == 0)); then
    ctest_args=(-L unit)
fi

if [[ -z "$mode" ]]; then
    if command -v cmake >/dev/null && command -v ninja >/dev/null; then
        mode=native
    else
        mode=flatpak
    fi
fi

build_native() {
    local dir="$root/build"

    if ((!build)); then
        exec "$dir/bin/kwi"
    fi

    ((clean)) && rm -rf "$dir"
    cmake -S "$root" -B "$dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DBUILD_TESTING=$( ((test)) && echo ON || echo OFF)
    cmake --build "$dir"

    if ((test)); then
        ctest --test-dir "$dir" --output-on-failure "${ctest_args[@]}"
    fi

    if ((run)); then
        exec "$dir/bin/kwi"
    fi
}

# Start a Flatpak build directory with the host's flatpak. flatpak-builder --run
# cannot reach the display when flatpak-builder itself runs as a Flatpak.
run_flatpak() {
    local app="$1"
    local permissions

    mapfile -t permissions < <(python3 -I -c 'import json, sys; print(*json.load(open(sys.argv[1]))["finish-args"], sep="\n")' \
        "$root/.flatpak-manifest.json")

    exec flatpak build --with-appdir --allow=devel --die-with-parent "${permissions[@]}" \
        --env=WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" --env=DISPLAY="${DISPLAY:-}" \
        "$app" kwi
}

build_flatpak() {
    local work="$root/build-flatpak"
    local manifest="$work/org.kde.kwi.json"
    local builder

    # Dependencies are installed per user, which needs Flathub as a user remote
    # even when it is already configured system-wide.
    if ! flatpak remotes --user --columns=name | grep -qx flathub; then
        echo "Adding the Flathub remote to the user installation..."
        flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
    fi

    if command -v flatpak-builder >/dev/null; then
        builder=(flatpak-builder)
    else
        if ! flatpak info org.flatpak.Builder >/dev/null 2>&1; then
            echo "Installing org.flatpak.Builder from Flathub (user installation)..."
            flatpak install --user -y flathub org.flatpak.Builder
        fi
        builder=(flatpak run org.flatpak.Builder)
    fi

    if ((!build)); then
        if [[ ! -d "$work/app" ]]; then
            echo "No Flatpak build found in $work. Run ./build.sh first." >&2
            exit 1
        fi

        run_flatpak "$work/app"
    fi

    ((clean)) && rm -rf "$work"
    mkdir -p "$work"

    # Derive a manifest that builds this checkout, with tests when requested.
    python3 -I - "$root/.flatpak-manifest.json" "$manifest" "$root" "$install" "$test" "${ctest_args[@]}" <<'EOF'
import json, shlex, sys

source, target, root, release, test, *ctest_args = sys.argv[1:]
manifest = json.load(open(source))
kwi = next(module for module in manifest["modules"] if module["name"] == "kwi")

for entry in kwi["sources"]:
    if entry.get("type") == "dir":
        entry["path"] = root
        entry["skip"] = ["build", "build-flatpak", ".flatpak-builder"]

if release == "1":
    # ECM defaults to a Debug build when no build type is given.
    for module in manifest["modules"]:
        if module.get("buildsystem", "").startswith("cmake"):
            opts = [o for o in module.get("config-opts", []) if not o.startswith("-DCMAKE_BUILD_TYPE")]
            module["config-opts"] = opts + ["-DCMAKE_BUILD_TYPE=Release"]

if test == "1":
    kwi["config-opts"] = [o for o in kwi.get("config-opts", []) if not o.startswith("-DBUILD_TESTING")]
    kwi["config-opts"].append("-DBUILD_TESTING=ON")
    # The SDK has no dbus-daemon. Run the application tests directly instead;
    # tests that need their own session bus may fail in the sandbox.
    kwi["config-opts"].append("-DDBUS_RUN_SESSION_EXECUTABLE=/usr/bin/env")
    kwi["builddir"] = True
    # flatpak-builder's own test step always runs the whole suite through
    # "ninja test". Run only the selected tests after installation instead.
    kwi["post-install"] = ["ctest --test-dir /run/build/kwi/_flatpak_build --output-on-failure " + shlex.join(ctest_args)]

json.dump(manifest, open(target, "w"), indent=4)
EOF

    local args=(--force-clean --install-deps-from=flathub --state-dir="$work/state" --ccache --disable-tests)

    # A separate branch keeps the installed build apart from other org.kde.kwi
    # installations, such as nightlies.
    if ((install)); then
        args+=(--install --default-branch=local --repo="$work/repo")
    fi

    (cd "$work" && "${builder[@]}" --user "${args[@]}" "$work/app" "$manifest")

    if ((install)); then
        echo
        echo "Installed org.kde.kwi//local. Start it with: flatpak run org.kde.kwi//local"
        ((run)) && exec flatpak run org.kde.kwi//local
    fi

    if ((run)); then
        run_flatpak "$work/app"
    fi
}

case "$mode" in
native) build_native ;;
flatpak) build_flatpak ;;
esac
