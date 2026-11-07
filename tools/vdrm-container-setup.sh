#!/bin/sh
# 容器侧的"迷你 udev"：不写 /sys、不依赖 udev 属性，也不碰宿主机的设备节点。
#
# 安全前提（踩过一次坑）：容器的 /dev/input、/dev/snd 是宿主机目录的 bind mount，
# 里面是【同一批 inode】。直接在容器里 rm/chmod 就是在改宿主机的节点 —— 实测删掉
# /dev/input/event* 之后 Android 立刻没有触摸/按键，只能强制重启才恢复。
#
# tmpfs 由 systemd 的 dev-input.mount / dev-snd.mount 负责；本脚本只验证
# 目录确实已经是 tmpfs，然后在里面创建节点，绝不去动下面的宿主节点。
#
# 作用：
#   1. /dev/input 只暴露 vdrm-kbd / vdrm-ptr / vdrm-touch（root:input 0660）；
#   2. /dev/snd 只暴露 vdrm 那张卡的节点（root:audio 0660）。
# 这样容器不需要「完整硬件访问」（/sys 只读 → 装/升级驱动不会软重启整机）。
#
# input 和 devices 分开：
#   vdrm-input.service   只做 input（键鼠识别）
#   vdrm-devices.service 只做 tty/DRM/audio 节点
# 模块重新加载后 eventN 编号会变，记得 restart 这两个 unit。
set -e

MODE=${1:-all}
case "$MODE" in
    input|devices|all) ;;
    *) echo "usage: $0 input|devices|all" >&2; exit 2 ;;
esac

# 挂载由 systemd .mount 单元负责（dev-input.mount / dev-snd.mount）。
# 这里先确认 /dev/input、/dev/snd 已经是我们的 tmpfs；绝不落到宿主 bind 上。
is_tmpfs() {
    awk -v m="$1" '$2 == m && $3 == "tmpfs" { found = 1 } END { exit !found }' /proc/mounts
}
if [ "$MODE" != devices ] && ! is_tmpfs /dev/input; then
    echo "vdrm-container-setup: /dev/input 不是 tmpfs（dev-input.mount 没起来？）" >&2
    exit 1
fi
if [ "$MODE" != input ] && ! is_tmpfs /dev/snd; then
    echo "vdrm-container-setup: /dev/snd 不是 tmpfs（dev-snd.mount 没起来？）" >&2
    exit 1
fi

mkdir -p /run/udev/data /run/udev/tags
# libudev 的 tag 枚举走 /run/udev/tags/<tag>/<devid>，不是只看 data 里的 Q:。
# 手动写 data 不建这个空文件的话，logind 启动时按 master-of-seat 枚举是空的。
udev_tag_add() {
    mkdir -p "/run/udev/tags/$1"
    : > "/run/udev/tags/$1/$2"
    chmod 444 "/run/udev/tags/$1/$2" 2>/dev/null || true
}

n=0
m=0

if [ "$MODE" != input ]; then
# ---- 虚拟 VT：/dev/tty0..6 ----
#
# 内核侧（vdrm 的 vdrm_vt）提供 major 4；但 droidspaces 会把容器的
# /dev/tty1..6 预置成 ->/dev/null（Fedora）或 tmpfs（Arch），logind 能看见
# /sys/class/tty/tty0/active，session 起来后却打不开 /dev/ttyN。这里换成
# 真正的 4:N 节点。只碰 tty0..6，不碰 /dev/tty（5:0）。
for tn in 0 1 2 3 4 5 6; do
    node=/dev/tty$tn
    if [ -c "$node" ] && [ "$(stat -c %t:%T "$node" 2>/dev/null)" = "4:$tn" ]; then
        continue
    fi
    umount "$node" 2>/dev/null || true
    rm -f "$node" 2>/dev/null || true
    mknod "$node" c 4 "$n" 2>/dev/null || true
    chmod 666 "$node" 2>/dev/null || true
done
# ---- DRM card：seat0 的 master-of-seat ----
#
# 不开 hw-access 时容器里的 udev 冷插拔不会给 vdrm 的 card 写 /run/udev/data；
# logind 启动时只枚举带 master-of-seat 的设备，缺了这份数据 seat0 就没有图形
# 设备（SDDM 报 "No graphical seats found"，KWin 拿不到 card）。
# 所以 vdrm-input 必须排在 systemd-logind 之前（unit 里的 Before=）。
for card in /sys/class/drm/card*; do
    [ -e "$card/dev" ] || continue
    case "$(readlink -f "$card" 2>/dev/null)" in
        */platform/vdrm/*) ;;
        *) continue ;;
    esac
    d=$(cat "$card/dev" 2>/dev/null) || continue
    {
        echo "I:$(date +%s)000000"
        echo "E:ID_PATH=platform-vdrm"
        echo "E:ID_PATH_TAG=platform-vdrm"
        echo "E:ID_FOR_SEAT=drm-platform-vdrm"
        echo "G:uaccess"
        echo "G:seat"
        echo "G:master-of-seat"
        echo "Q:uaccess"
        echo "Q:seat"
        echo "Q:master-of-seat"
        echo "V:1"
    } > "/run/udev/data/c$d"
    udev_tag_add seat "c$d"
    udev_tag_add master-of-seat "c$d"
    udev_tag_add uaccess "c$d"
done

fi

if [ "$MODE" != devices ]; then

# ---- 输入：只放 vdrm 三个虚拟设备 ----
#
# 光有 /dev 节点还不够：libinput 走 libudev 枚举，没有 ID_INPUT* 属性它会直接跳过
# 这个设备（表现为"合成器起来了但完全没输入"）。不开「完整硬件访问」时 /sys 是只读的，
# udevadm trigger 跑不了，所以这里手写 /run/udev/data 条目 —— 就是 udev 自己会写的那份。
udev_tag() {   # $1=名字  → 返回 "ID_INPUT=1 ID_INPUT_XXX=1 ..."
    case "$1" in
        vdrm-kbd)   echo "ID_INPUT=1 ID_INPUT_KEY=1 ID_INPUT_KEYBOARD=1" ;;
        vdrm-ptr)   echo "ID_INPUT=1 ID_INPUT_MOUSE=1" ;;
        vdrm-touch) echo "ID_INPUT=1 ID_INPUT_TOUCHSCREEN=1" ;;
        *)          echo "ID_INPUT=1" ;;
    esac
}
n=0
for p in /sys/class/input/event*/device/name; do
    [ -e "$p" ] || continue
    name=$(cat "$p" 2>/dev/null)
    case "$name" in vdrm-*) ;; *) continue ;; esac
    ev=$(basename "$(dirname "$(dirname "$p")")")
    d=$(cat "/sys/class/input/$ev/dev" 2>/dev/null) || continue
    [ -n "$d" ] || continue
    [ -c "/dev/input/$ev" ] || mknod "/dev/input/$ev" c "${d%%:*}" "${d##*:}" 2>/dev/null || continue
    chown root:input "/dev/input/$ev" 2>/dev/null || true
    # 0666：这个目录里只有 vdrm 三个虚拟设备，世界可读不影响任何人，
    # 但省掉"合成器用户不在 input 组 → libinput 打不开设备"这一类问题。
    chmod 666 "/dev/input/$ev"
    ln -sf "$ev" "/dev/input/$name"
    {
        echo "I:$(date +%s)000000"
        for kv in $(udev_tag "$name"); do echo "E:$kv"; done
        # logind / seatd 判断“设备属于哪个 seat”看的是 udev current tag（Q:seat），
        # 不是 G:seat。只写 ID_INPUT* 时：libinput 直接枚举能看到设备，但走
        # logind 的合成器（KWin 的 LogindSession）TakeDevice 会 ENODEV，
        # 表现就是“画面正常、键鼠触控全丢”。seatd 的 seat 分配同理。
        echo "G:seat"
        echo "Q:seat"
    } > "/run/udev/data/c$d"
    udev_tag_add seat "c$d"
    n=$((n + 1))
done

# 父设备 inputN 也要写：logind 的 session_device_verify() 对 evdev 是按
# eventN 的父 inputN 的 syspath 去 devices 表里找 seat 成员；只给 eventN
# 写 seat 时，TakeDevice(13:event_minor) 仍然 ENODEV。
for p in /sys/class/input/input*/name; do
    [ -e "$p" ] || continue
    name=$(cat "$p" 2>/dev/null)
    case "$name" in vdrm-*) ;; *) continue ;; esac
    in=$(basename "$(dirname "$p")")
    {
        echo "I:$(date +%s)000000"
        echo "G:seat"
        echo "Q:seat"
    } > "/run/udev/data/+input:$in"
    udev_tag_add seat "+input:$in"
done

fi

if [ "$MODE" != input ]; then

# ---- 声卡：只放 vdrm 那张卡 ----
card=$(sed -n 's/^ *\([0-9][0-9]*\) \[vdrm.*/\1/p' /proc/asound/cards 2>/dev/null | head -1)
m=0
if [ -n "$card" ]; then
    for s in /sys/class/sound/controlC"$card" /sys/class/sound/pcmC"$card"D* /sys/class/sound/timer; do
        [ -e "$s" ] || continue
        nm=$(basename "$s")
        d=$(cat "$s/dev" 2>/dev/null) || continue
        [ -n "$d" ] || continue
        [ -c "/dev/snd/$nm" ] || mknod "/dev/snd/$nm" c "${d%%:*}" "${d##*:}" 2>/dev/null || continue
        chown root:audio "/dev/snd/$nm" 2>/dev/null || true
        chmod 660 "/dev/snd/$nm"
        m=$((m + 1))
    done
fi

fi

echo "vdrm-container-setup: mode=$MODE 输入 $n 个设备，声卡 $m 个节点"
