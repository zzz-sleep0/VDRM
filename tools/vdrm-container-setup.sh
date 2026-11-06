#!/bin/sh
# 容器侧的"迷你 udev"：不写 /sys、不依赖 udev 属性，也不碰宿主机的设备节点。
#
# 安全前提（踩过一次坑）：容器的 /dev/input、/dev/snd 是宿主机目录的 bind mount，
# 里面是【同一批 inode】。直接在容器里 rm/chmod 就是在改宿主机的节点 —— 实测删掉
# /dev/input/event* 之后 Android 立刻没有触摸/按键，只能强制重启才恢复。
#
# 所以这里的铁律是：**先挂 tmpfs 把目录顶掉并确认挂上了**，之后所有 mknod/chmod
# 都落在容器自己的 tmpfs 里；挂不上就直接退出，绝不去动下面的宿主节点。
#
# 作用：
#   1. /dev/input 只暴露 vdrm-kbd / vdrm-ptr / vdrm-touch（root:input 0660）；
#   2. /dev/snd 只暴露 vdrm 那张卡的节点（root:audio 0660）。
# 这样容器不需要「完整硬件访问」（/sys 只读 → 装/升级驱动不会软重启整机）。
#
# 模块重新加载后 eventN 编号会变，记得 `systemctl restart vdrm-input`。
set -e

ours() { grep -q "^$1 /dev/$2 " /proc/mounts; }

# 把 /dev 的传播关系切成 rprivate，避免容器里的挂载往宿主回流（实测本来也不回流，
# 这里是多加一道保险）。
mount --make-rprivate /dev 2>/dev/null || true

for pair in "vdrm-input input" "vdrm-snd snd"; do
    tag=${pair%% *}
    dir=${pair##* }
    if ! ours "$tag" "$dir"; then
        mount -t tmpfs -o mode=755 "$tag" "/dev/$dir" || {
            echo "vdrm-container-setup: 无法挂载 tmpfs 到 /dev/$dir，放弃（不动宿主机节点）" >&2
            exit 1
        }
    fi
    ours "$tag" "$dir" || {
        echo "vdrm-container-setup: /dev/$dir 不是我们的 tmpfs，放弃" >&2
        exit 1
    }
done

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
mkdir -p /run/udev/data
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
    } > "/run/udev/data/c$d"
    n=$((n + 1))
done

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

echo "vdrm-container-setup: 输入 $n 个设备，声卡 $m 个节点"
