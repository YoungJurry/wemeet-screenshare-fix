#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(dirname -- "$SCRIPT_DIR")
SYSTEM_LIB_DIR=${WEMEET_FIX_LIB_DIR:-/usr/lib/wemeet-screenshare-fix}
DEVELOPMENT_BUILD_DIR=${WEMEET_PW_GUARD_BUILD_DIR:-/tmp/wemeet-pw-guard-build}
DEVELOPMENT_GUARD=$DEVELOPMENT_BUILD_DIR/libwemeet_pw_guard.so
TOOLTIP_FIX=${WEMEET_TOOLTIP_FIX:-1}
case "$TOOLTIP_FIX" in
    0|1) ;;
    *) echo "Refusing: WEMEET_TOOLTIP_FIX must be 0 or 1." >&2; exit 1 ;;
esac

if [ -n "${WEMEET_PW_GUARD_LIBRARY:-}" ]; then
    GUARD=$WEMEET_PW_GUARD_LIBRARY
elif [ -f "$PROJECT_DIR/src/guard.c" ] && [ -f "$PROJECT_DIR/Makefile" ]; then
    # A checkout must not silently run an older installed Guard.
    GUARD=$DEVELOPMENT_GUARD
elif [ -r "$SYSTEM_LIB_DIR/libwemeet_pw_guard.so" ]; then
    GUARD=$SYSTEM_LIB_DIR/libwemeet_pw_guard.so
else
    GUARD=$DEVELOPMENT_GUARD
fi

LAYOUT_GUARD=${WEMEET_LAYOUT_GUARD_LIBRARY:-$(dirname -- "$GUARD")/libwemeet_layout_guard.so}

if [ -n "${WEMEET_HUNK0_TOOL:-}" ]; then
    HUNK0_TOOL=$WEMEET_HUNK0_TOOL
elif [ -x "$SCRIPT_DIR/stage_hunk0.py" ]; then
    HUNK0_TOOL=$SCRIPT_DIR/stage_hunk0.py
elif [ -x "$SYSTEM_LIB_DIR/stage_hunk0.py" ]; then
    HUNK0_TOOL=$SYSTEM_LIB_DIR/stage_hunk0.py
else
    HUNK0_TOOL=$SCRIPT_DIR/stage_hunk0.py
fi

if [ -n "${WEMEET_TOOLTIP_TOOL:-}" ]; then
    TOOLTIP_TOOL=$WEMEET_TOOLTIP_TOOL
elif [ -x "$SCRIPT_DIR/stage_tooltip.py" ]; then
    TOOLTIP_TOOL=$SCRIPT_DIR/stage_tooltip.py
else
    TOOLTIP_TOOL=$SYSTEM_LIB_DIR/stage_tooltip.py
fi

ORIGINAL_MODULE=/opt/wemeet/bin/modules/screen_share/libscreen_share_module.so
SANDBOX_MODULE=$ORIGINAL_MODULE

# The AUR package relocates Tencent's libraries and rewrites RUNPATH. The
# official deb keeps them under /opt/wemeet/lib. An alien-converted RPM keeps
# the deb layout and payload, so it is handled by the deb branch as well.
REQUESTED_LAYOUT=${WEMEET_LAYOUT:-auto}
case "$REQUESTED_LAYOUT" in
    auto|arch|deb) ;;
    *) echo "Refusing: WEMEET_LAYOUT must be auto, arch or deb." >&2; exit 1 ;;
esac
arch_layout=0
deb_layout=0
if [ -r /usr/lib/wemeet/libxcast.so ] && [ -r /usr/lib/wemeet/libwemeet_framework.so ]; then
    arch_layout=1
fi
if [ -r /opt/wemeet/lib/libxcast.so ] && [ -r /opt/wemeet/lib/libwemeet_framework.so ]; then
    deb_layout=1
fi
if [ "$REQUESTED_LAYOUT" = auto ]; then
    if [ "$arch_layout" = 1 ] && [ "$deb_layout" = 1 ]; then
        echo "Refusing: both AUR and deb layouts are present; set WEMEET_LAYOUT=arch or deb." >&2
        exit 1
    elif [ "$arch_layout" = 1 ]; then
        MEETING_LAYOUT=arch
    elif [ "$deb_layout" = 1 ]; then
        MEETING_LAYOUT=deb
    else
        echo "Refusing: no validated Tencent Meeting 3.26.10.401 layout found." >&2
        echo "Expected /usr/lib/wemeet (AUR) or /opt/wemeet/lib (official deb)." >&2
        exit 1
    fi
else
    MEETING_LAYOUT=$REQUESTED_LAYOUT
fi
case "$MEETING_LAYOUT" in
    arch)
        [ "$arch_layout" = 1 ] || {
            echo "Refusing: AUR layout not found (missing /usr/lib/wemeet)." >&2
            exit 1
        }
        XCAST=/usr/lib/wemeet/libxcast.so
        FRAMEWORK=/usr/lib/wemeet/libwemeet_framework.so
        LAUNCHER=/usr/bin/wemeet
        WRAPPER=/usr/lib/wemeet/libwemeetwrap.so
        APP_LIB_DIR=/usr/lib/wemeet
        APP_PLUGIN_DIR=/usr/lib/wemeet/plugins
        EXPECTED_MODULE_HASH=925e7c16a5d69adaa804d1ea55aac1edc4093aacb5324948ea6f38a71abd4e16
        EXPECTED_PATCHED_MODULE_HASH=075aef4471ed8ea87f0c9c95995981358087265987d05b656d3bf0b8a5f8f418
        EXPECTED_FRAMEWORK_HASH=2118f17803de4fb8e02b44d273b6301f206e893f6d1024495b605648c510ed2a
        EXPECTED_LAUNCHER_HASH=d6e4a6edcf2fca7dcca347bb24aa3be61924a9efa2013893c5e40f13a8ea354c
        # The AUR recipe rewrites RUNPATH with patchelf. Different patchelf
        # releases produce byte-distinct ELF layouts from the same official
        # libxcast.so, so accept only the verified reproducible outputs.
        XCAST_HASHES="53d5c38636d9235b28f6944c1c53a42b3292e27ffa81a550285c46cf1abba522 b3eddceebbeaa0b4bc4e2e84d66affdf061f862f9ce1fecc52e647d232ec94b2 d242e9813f4c2eb694597a31648ffbb251c95660cb20853fb9bc1dde4d895439 472a4c59b2ea9db68fcbc13cf477d4795a58353b5fe7716793f407cd5b8b174a"
        ;;
    deb)
        [ "$deb_layout" = 1 ] || {
            echo "Refusing: deb layout not found (missing /opt/wemeet/lib)." >&2
            exit 1
        }
        XCAST=/opt/wemeet/lib/libxcast.so
        FRAMEWORK=/opt/wemeet/lib/libwemeet_framework.so
        LAUNCHER=/opt/wemeet/bin/wemeetapp
        WRAPPER=
        APP_LIB_DIR=/opt/wemeet/lib
        APP_PLUGIN_DIR=/opt/wemeet/plugins
        EXPECTED_MODULE_HASH=b3296906e5aba1a0b4297cd6e59b4ba19519e56de912b2ea0c5a23f3a252e7ed
        EXPECTED_PATCHED_MODULE_HASH=64bc0a7800bf92ff6515bb78965fff8d5d8bb9dc85a048611d5543b2ce321d8d
        EXPECTED_FRAMEWORK_HASH=b6cb7bac32b3fe2cd7b76cc6104380c7d922eac2410a86ce9ab82d3783213d79
        EXPECTED_LAUNCHER_HASH=c552f51af406190ef9749108794e260c9f40aaced3ad5b0b36023d687f5965bd
        # Unmodified upstream libxcast.so. RPMs converted from this deb, and any
        # official RPM built from the same payload, keep these bytes.
        XCAST_HASHES="79e4d43d90d2f6a67834995d0dcd8ba4c49be1b934c2cfabef5f0e4894535dad"
        ;;
esac
FRAMEWORK_OVERLAY=$FRAMEWORK
RUNTIME_PARENT=${XDG_RUNTIME_DIR:-/tmp}
RUNTIME_DIR=$(mktemp -d "$RUNTIME_PARENT/wemeet-screenshare-fix-$(id -u).XXXXXX")
PATCHED_MODULE=$RUNTIME_DIR/libscreen_share_module.so
PATCHED_FRAMEWORK=$RUNTIME_DIR/libwemeet_framework.so

cleanup() {
    rm -f -- "$PATCHED_MODULE" "$PATCHED_FRAMEWORK"
    rmdir -- "$RUNTIME_DIR" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

BWRAP=$(command -v bwrap 2>/dev/null || true)
[ -n "$BWRAP" ] || {
    echo "Refusing: bubblewrap is required for an isolated Hunk 0 overlay." >&2
    exit 1
}
echo "[wemeet-pw-guard] detected layout: $MEETING_LAYOUT" >&2
if [ "$GUARD" = "$DEVELOPMENT_GUARD" ]; then
    [ -f "$PROJECT_DIR/Makefile" ] || {
        echo "Guard library not found and this is not a development checkout: $GUARD" >&2
        exit 1
    }
    make -C "$PROJECT_DIR" BUILD_DIR="$DEVELOPMENT_BUILD_DIR" all >&2
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
launcher_hash=$(sha256sum "$LAUNCHER"); launcher_hash=${launcher_hash%% *}
framework_hash=$(sha256sum "$FRAMEWORK"); framework_hash=${framework_hash%% *}
[ "$framework_hash" = "$EXPECTED_FRAMEWORK_HASH" ] || {
    echo "Refusing: host UI framework is not the audited original ($framework_hash)." >&2
    exit 1
}

[ "$original_hash" = "$EXPECTED_MODULE_HASH" ] || {
    echo "Refusing: host screen-share module is not original ($original_hash)." >&2
    exit 1
}
xcast_ok=0
for allowed in $XCAST_HASHES; do
    if [ "$xcast_hash" = "$allowed" ]; then
        xcast_ok=1
        break
    fi
done
[ "$xcast_ok" = 1 ] || {
    echo "Refusing: libxcast.so is not a validated package build ($xcast_hash)." >&2
    exit 1
}
[ "$launcher_hash" = "$EXPECTED_LAUNCHER_HASH" ] || {
    echo "Refusing: $LAUNCHER is not the validated package launcher." >&2
    exit 1
}

mkdir -p "$RUNTIME_DIR"
patched_hash=
if [ -r "$PATCHED_MODULE" ]; then
    patched_hash=$(sha256sum "$PATCHED_MODULE"); patched_hash=${patched_hash%% *}
fi
if [ "$patched_hash" != "$EXPECTED_PATCHED_MODULE_HASH" ]; then
    rm -f -- "$PATCHED_MODULE"
    "$HUNK0_TOOL" "$ORIGINAL_MODULE" "$PATCHED_MODULE"
fi

if [ "$TOOLTIP_FIX" = 1 ]; then
    [ -r "$LAYOUT_GUARD" ] || {
        echo "Layout guard library not found: $LAYOUT_GUARD" >&2
        exit 1
    }
    [ -x "$TOOLTIP_TOOL" ] || {
        echo "Tooltip staging tool not found: $TOOLTIP_TOOL" >&2
        exit 1
    }
    "$TOOLTIP_TOOL" "$FRAMEWORK" "$PATCHED_FRAMEWORK"
    framework_hash=$(sha256sum "$PATCHED_FRAMEWORK"); framework_hash=${framework_hash%% *}
    [ "$framework_hash" = "$EXPECTED_FRAMEWORK_HASH" ] || {
        echo "Refusing: staged tooltip framework hash mismatch ($framework_hash)." >&2
        exit 1
    }
    FRAMEWORK_OVERLAY=$PATCHED_FRAMEWORK
fi

if [ "${WEMEET_PW_GUARD_VALIDATE_ONLY:-0}" = 1 ]; then
    "$BWRAP" \
        --unshare-user-try --unshare-pid \
        --proc /proc \
        --ro-bind / / \
        --ro-bind "$PATCHED_MODULE" "$SANDBOX_MODULE" \
        --ro-bind "$FRAMEWORK_OVERLAY" "$FRAMEWORK" \
        /usr/bin/sha256sum "$SANDBOX_MODULE" "$FRAMEWORK"
    exit 0
fi

export QT_AUTO_SCREEN_SCALE_FACTOR=1
export QT_STYLE_OVERRIDE=fusion
export IBUS_USE_PORTAL=1
export LD_LIBRARY_PATH="$APP_LIB_DIR:/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$APP_PLUGIN_DIR"
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
if [ "$TOOLTIP_FIX" = 1 ]; then
    case ":$preload:" in
        *":$LAYOUT_GUARD:"*) ;;
        *) preload="$LAYOUT_GUARD:$preload" ;;
    esac
fi
# Keep Tencent's AUR wrapper first. Its RTLD_NEXT lookups must pass this guard
# before the guard forwards them to glibc's real dlsym implementation. The
# official deb does not ship libwemeetwrap.so.
if [ -n "$WRAPPER" ]; then
    LD_PRELOAD_WRAP="$WRAPPER:$preload"
else
    LD_PRELOAD_WRAP=$preload
fi

echo "[wemeet-pw-guard] host package remains original; Hunk 0 is sandbox-only" >&2
echo "[wemeet-tooltip-fix] full callback lifetime guard: $TOOLTIP_FIX" >&2
"$BWRAP" \
    --new-session \
    --unshare-user-try --unshare-pid --unshare-uts --unshare-cgroup-try \
    --proc /proc \
    --ro-bind / / \
    --ro-bind "$PATCHED_MODULE" "$SANDBOX_MODULE" \
    --ro-bind "$FRAMEWORK_OVERLAY" "$FRAMEWORK" \
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
    --setenv WEMEET_TOOLTIP_FIX "$TOOLTIP_FIX" \
    --setenv LD_PRELOAD "$LD_PRELOAD_WRAP" \
    /opt/wemeet/bin/wemeetapp "$@"
