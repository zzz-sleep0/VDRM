#!/bin/sh
# 容器侧的"迷你 udev"：不写 /sys、不依赖 udev 属性。
#
# 开了「完整硬件访问」的容器里 /sys 是可写的，装/升级驱动可能让整机软重启一次；
# 所以更稳的做法是不开硬件访问（/sys 只读），由这个脚本把该有的节点建出来：
#
#   1. /dev/input 换成只含 vdrm 三个虚拟设备（vdrm-kbd / vdrm-ptr / vdrm-touch）
#      的 tmpfs，权限 root:input 0660 —— 手机真实的触摸/电源键/指纹根本进不来；
#   2. vdrm 那张声卡的 /dev/snd 节点改成 root:audio 0660。
#
# 装法：拷到 /usr/local/sbin/，配一个 oneshot 的 systemd unit（README「配容器」）。
# 模块重新加载后设备号会变，记得 `systemctl restart vdrm-input`。
set -e

# ---- 输入 ----
mkdir -p /dev/input
if ! grep -q ' /dev/input ' /proc/mounts; then
    mount -t tmpfs -o mode=755 vdrm-input /dev/input
fi
rm -f /dev/input/event*
n=0
for p in /sys/class/input/event*/device/name; do
    [ -e "$p" ] || continue
    name=$(cat "$p" 2>/dev/null)
    case "$name" in vdrm-*) ;; *) continue ;; esac
    ev=$(basename "$(dirname "$(dirname "$p")")")
    dev=$(cat "/sys/class/input/$ev/dev" 2>/dev/null)
    [ -n "$dev" ] || continue
    node=/dev/input/$ev
    [ -c "$node" ] || mknod "$node" c "${dev%%:*}" "${dev##*:}"
    chown root:input "$node" 2>/dev/null || true
    chmod 660 "$node"
    ln -sf "$ev" "/dev/input/$name"
    n=$((n + 1))
done

# ---- 声卡 ----
card=$(sed -n 's/^ *\([0-9][0-9]*\) \[vdrm.*/\1/p' /proc/asound/cards 2>/dev/null | head -1)
m=0
if [ -n "$card" ]; then
    for node in /dev/snd/*C${card}D* /dev/snd/controlC${card}; do
        [ -e "$node" ] || continue
        chown root:audio "$node" 2>/dev/null || true
        chmod 660 "$node"
        m=$((m + 1))
    done
fi

echo "vdrm-container-setup: 输入 $n 个设备，声卡 $m 个节点"
