#!/system/bin/sh
# 重载 vdrm.ko：先杀掉合成器和 App，确保没人打开 /dev/input；
# 加载后立即做设备排除，再恢复桌面。不需要软重启。
MODDIR=${0%/*}
. "$MODDIR/common.sh"

DS=/data/local/Droidspaces/bin/droidspaces
CT=Fedora-43

say() { echo "[vdrm-reload] $*"; }

# 1) 合成器/App 先放手
am force-stop com.vdrm.consumer >/dev/null 2>&1
if [ -x "$DS" ]; then
    "$DS" -n "$CT" run -- bash -lc \
        'systemctl stop sddm 2>/dev/null; pkill -9 -x kwin_wayland 2>/dev/null; sleep 1' \
        >/dev/null 2>&1
fi
sleep 1

# 2) 重载；必须在没人打开节点时做
rmmod vdrm 2>/dev/null
if grep -q '^vdrm ' /proc/modules; then
    say "rmmod 失败，仍有进程占用 /dev/vdrm"
    exit 1
fi
vdrm_load || exit 1

# 3) 立即排除，窗口期最短
vdrm_fix_nodes
vdrm_hide_input

# 4) 恢复桌面/App
if [ -x "$DS" ]; then
    "$DS" -n "$CT" run -- bash -lc 'systemctl start sddm' >/dev/null 2>&1
fi
am start -n com.vdrm.consumer/.MainActivity >/dev/null 2>&1
say "done"
