#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(dirname -- "$SCRIPT_DIR")
SYSTEM_LIB_DIR=${WEMEET_FIX_LIB_DIR:-/usr/lib/wemeet-screenshare-fix}
DEVELOPMENT_GUARD=/tmp/wemeet-pw-guard-build/libwemeet_pw_guard.so

if [ -n "${WEMEET_PW_GUARD_LIBRARY:-}" ]; then
    GUARD=$WEMEET_PW_GUARD_LIBRARY
elif [ -r "$SYSTEM_LIB_DIR/libwemeet_pw_guard.so" ]; then
    GUARD=$SYSTEM_LIB_DIR/libwemeet_pw_guard.so
else
    GUARD=$DEVELOPMENT_GUARD
fi

if [ -n "${WEMEET_HUNK0_TOOL:-}" ]; then
    HUNK0_TOOL=$WEMEET_HUNK0_TOOL
elif [ -x "$SYSTEM_LIB_DIR/stage_hunk0.py" ]; then
    HUNK0_TOOL=$SYSTEM_LIB_DIR/stage_hunk0.py
else
    HUNK0_TOOL=$SCRIPT_DIR/stage_hunk0.py
fi

ORIGINAL_MODULE=/opt/wemeet/bin/modules/screen_share/libscreen_share_module.so
SANDBOX_MODULE=$ORIGINAL_MODULE
XCAST=/usr/lib/wemeet/libxcast.so
RUNTIME_PARENT=${XDG_RUNTIME_DIR:-/tmp}
RUNTIME_DIR=$(mktemp -d "$RUNTIME_PARENT/wemeet-screenshare-fix-$(id -u).XXXXXX")
PATCHED_MODULE=$RUNTIME_DIR/libscreen_share_module.so

cleanup() {
    rm -f -- "$PATCHED_MODULE"
    rmdir -- "$RUNTIME_DIR" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ -x /usr/bin/bwrap ] || {
    echo "Refusing: bubblewrap is required for an isolated Hunk 0 overlay." >&2
    exit 1
}
installed_package=$(/usr/bin/pacman -Q wemeet-bin 2>/dev/null || true)
[ "$installed_package" = "wemeet-bin 3.26.10.401-5" ] || {
    echo "Refusing: expected wemeet-bin 3.26.10.401-5, found: ${installed_package:-not installed}." >&2
    exit 1
}
if [ ! -r "$GUARD" ] && [ "$GUARD" = "$DEVELOPMENT_GUARD" ]; then
    [ -f "$PROJECT_DIR/Makefile" ] || {
        echo "Guard library not found and this is not a development checkout: $GUARD" >&2
        exit 1
    }
    echo "[wemeet-pw-guard] rebuilding development library after reboot" >&2
    make -C "$PROJECT_DIR" all >&2
fi
[ -r "$GUARD" ] || {
    echo "Guard library not found: $GUARD" >&2
    exit 1
}
[ -x "$HUNK0_TOOL" ] || {
    echo "Hunk 0 staging tool not found: $HUNK0_TOOL" >&2
    exit 1
}

original_hash=$(sha256sum "$ORIGINAL_MODULE"); original_hash=${original_hash%% *}
xcast_hash=$(sha256sum "$XCAST"); xcast_hash=${xcast_hash%% *}
launcher_hash=$(sha256sum /usr/bin/wemeet); launcher_hash=${launcher_hash%% *}

[ "$original_hash" = 925e7c16a5d69adaa804d1ea55aac1edc4093aacb5324948ea6f38a71abd4e16 ] || {
    echo "Refusing: host screen-share module is not original ($original_hash)." >&2
    exit 1
}
# The AUR recipe rewrites RUNPATH with patchelf. Different patchelf releases
# produce byte-distinct ELF layouts from the same official libxcast.so, so
# accept only the exact, reproducible outputs verified from that source file.
case "$xcast_hash" in
    # Verified with patchelf 0.14.5, 0.15.0, 0.16.0 and 0.16.1.
    53d5c38636d9235b28f6944c1c53a42b3292e27ffa81a550285c46cf1abba522) ;;
    # Verified with patchelf 0.17.0, 0.17.1 and 0.17.2.
    b3eddceebbeaa0b4bc4e2e84d66affdf061f862f9ce1fecc52e647d232ec94b2) ;;
    # Verified with patchelf 0.18.0.
    d242e9813f4c2eb694597a31648ffbb251c95660cb20853fb9bc1dde4d895439) ;;
    # Verified with patchelf 0.19.0 and 0.19.1.
    472a4c59b2ea9db68fcbc13cf477d4795a58353b5fe7716793f407cd5b8b174a) ;;
    *)
        echo "Refusing: libxcast.so is not a validated package build ($xcast_hash)." >&2
        exit 1
        ;;
esac
[ "$launcher_hash" = d6e4a6edcf2fca7dcca347bb24aa3be61924a9efa2013893c5e40f13a8ea354c ] || {
    echo "Refusing: /usr/bin/wemeet is not the validated package launcher." >&2
    exit 1
}

mkdir -p "$RUNTIME_DIR"
patched_hash=
if [ -r "$PATCHED_MODULE" ]; then
    patched_hash=$(sha256sum "$PATCHED_MODULE"); patched_hash=${patched_hash%% *}
fi
if [ "$patched_hash" != 075aef4471ed8ea87f0c9c95995981358087265987d05b656d3bf0b8a5f8f418 ]; then
    rm -f -- "$PATCHED_MODULE"
    "$HUNK0_TOOL" "$ORIGINAL_MODULE" "$PATCHED_MODULE"
fi

if [ "${WEMEET_PW_GUARD_VALIDATE_ONLY:-0}" = 1 ]; then
    /usr/bin/bwrap \
        --unshare-user-try --unshare-pid \
        --proc /proc \
        --ro-bind / / \
        --ro-bind "$PATCHED_MODULE" "$SANDBOX_MODULE" \
        /usr/bin/sha256sum "$SANDBOX_MODULE"
    exit 0
fi

export QT_AUTO_SCREEN_SCALE_FACTOR=1
export QT_STYLE_OVERRIDE=fusion
export IBUS_USE_PORTAL=1
export LD_LIBRARY_PATH="/usr/lib/wemeet:${LD_LIBRARY_PATH:-/usr/lib}"
export QT_QPA_PLATFORM=xcb
export XDG_SESSION_TYPE=x11
unset WAYLAND_DISPLAY
export WEMEET_XWAYLAND=1

USER_RUN_DIR="/run/user/$(id -u)"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}"
FONTCONFIG_DIR="$CONFIG_DIR/fontconfig"
KDE_GLOBALS_FILE="$CONFIG_DIR/kdeglobals"
KDE_ICON_CACHE_FILE="${XDG_CACHE_HOME:-$HOME/.cache}/icon-cache.kcache"
WEMEET_APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/wemeetapp"
mkdir -p "$WEMEET_APP_DIR"

GUARD_ACTIVE=${WEMEET_PW_GUARD:-1}
case ":${LD_PRELOAD:-}:" in
    *":$GUARD:"*) preload=${LD_PRELOAD:-} ;;
    *) preload="$GUARD${LD_PRELOAD:+:$LD_PRELOAD}" ;;
esac
# Keep Tencent's wrapper first. Its RTLD_NEXT lookups must pass this guard before
# the guard forwards them to glibc's real dlsym implementation.
LD_PRELOAD_WRAP="/usr/lib/wemeet/libwemeetwrap.so:$preload"

echo "[wemeet-pw-guard] host package remains original; Hunk 0 is sandbox-only" >&2
/usr/bin/bwrap \
    --new-session \
    --unshare-user-try --unshare-pid --unshare-uts --unshare-cgroup-try \
    --proc /proc \
    --ro-bind / / \
    --ro-bind "$PATCHED_MODULE" "$SANDBOX_MODULE" \
    --dev-bind /dev /dev \
    --bind /tmp /tmp \
    --ro-bind /dev/null /proc/cpuinfo \
    --tmpfs /sys/devices/virtual \
    --bind "$USER_RUN_DIR" "$USER_RUN_DIR" \
    --tmpfs /var \
    --tmpfs "$CONFIG_DIR" \
    --ro-bind-try "$KDE_GLOBALS_FILE" "$KDE_GLOBALS_FILE" \
    --ro-bind-try "$FONTCONFIG_DIR" "$FONTCONFIG_DIR" \
    --bind-try "$KDE_ICON_CACHE_FILE" "$KDE_ICON_CACHE_FILE" \
    --bind "$WEMEET_APP_DIR" "$WEMEET_APP_DIR" \
    --setenv WEMEET_PW_GUARD "$GUARD_ACTIVE" \
    --setenv LD_PRELOAD "$LD_PRELOAD_WRAP" \
    /opt/wemeet/bin/wemeetapp "$@"
