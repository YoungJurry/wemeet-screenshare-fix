#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(dirname -- "$SCRIPT_DIR")
APPLICATION_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/applications
ENTRY=$APPLICATION_DIR/wemeet-wayland-guard.desktop

case "$PROJECT_DIR" in
    *'"'*|*'`'*|*'$'*|*'\'*|*'
'*)
        echo "Refusing unsupported characters in project path: $PROJECT_DIR" >&2
        exit 1
        ;;
esac

mkdir -p "$APPLICATION_DIR"
cat > "$ENTRY" <<EOF
[Desktop Entry]
Type=Application
Name=Tencent Meeting (Wayland Guard)
Name[zh_CN]=腾讯会议（Wayland 修复版）
Comment=Launch Tencent Meeting with sandboxed Hunk 0 and PipeWire buffer guard
Comment[zh_CN]=使用沙箱 Hunk 0 与 PipeWire 缓冲守卫启动腾讯会议
Exec="$PROJECT_DIR/tools/run-guarded.sh" %u
Icon=wemeet
Terminal=false
Categories=AudioVideo;
Keywords=wemeet;tencent;meeting;wayland;pipewire;screen share;
StartupWMClass=wemeetapp
EOF
chmod 0644 "$ENTRY"

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$ENTRY"
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APPLICATION_DIR"
fi
printf 'Installed %s\n' "$ENTRY"
