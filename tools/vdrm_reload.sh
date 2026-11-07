#!/system/bin/sh
# 重载 vdrm.ko：先停容器/合成器和 App，确保 tty、/dev/vdrm 都没人持有；
# 加载后立即做 Android 输入排除，再建池、恢复容器和 App。无需软重启。
MODDIR=${0%/*}
. "$MODDIR/common.sh"

DS=/data/local/Droidspaces/bin/droidspaces
CT=Fedora-43

say() { echo "[vdrm-reload] $*"; }

# 1) App 先杀，不能只 force-stop
am force-stop com.vdrm.consumer >/dev/null 2>&1
for p in $(pidof com.vdrm.consumer 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
for p in $(pidof vdrm_mkpool 2>/dev/null); do kill -9 "$p" 2>/dev/null; done

# 2) 容器整体停掉，释放 tty/KWin/card1 持有
if [ -x "$DS" ]; then
    "$DS" -n "$CT" stop >/dev/null 2>&1 || true
fi
sleep 2

# 3) 重载，加载后立即排除
rmmod vdrm 2>/dev/null
if grep -q '^vdrm ' /proc/modules; then
    say "rmmod 失败"
    exit 1
fi
vdrm_load || exit 1
vdrm_fix_nodes
vdrm_hide_input

# 4) 建池（没有池 App 会 0 bufs）
"$MODDIR/vdrm_mkpool" --reset >/dev/null 2>&1 || say "mkpool 失败"

# 5) 恢复容器/App
if [ -x "$DS" ]; then
    "$DS" -n "$CT" start >/dev/null 2>&1 || true
fi
am start -n com.vdrm.consumer/.MainActivity >/dev/null 2>&1
say "done"
