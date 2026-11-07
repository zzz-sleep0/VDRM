#!/bin/sh
# 容器侧的 udev 数据填充。不 mknod、不 mount、不 mask。
# 设备节点由容器运行时逐个 bind；这里只写 /run/udev 让合成器/logind 能激活。
set -e

mkdir -p /run/udev/data /run/udev/tags

udev_tag_add() {
    mkdir -p "/run/udev/tags/$1"
    : > "/run/udev/tags/$1/$2"
    chmod 444 "/run/udev/tags/$1/$2" 2>/dev/null || true
}

# ---- DRM card ----
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
        echo "G:uaccess"; echo "G:seat"; echo "G:master-of-seat"
        echo "Q:uaccess"; echo "Q:seat"; echo "Q:master-of-seat"
        echo "V:1"
    } > "/run/udev/data/c$d"
    udev_tag_add seat "c$d"
    udev_tag_add master-of-seat "c$d"
    udev_tag_add uaccess "c$d"
done

# ---- input ----
udev_tag() {
    case "$1" in
        vdrm-kbd)   echo "ID_INPUT=1 ID_INPUT_KEY=1 ID_INPUT_KEYBOARD=1" ;;
        vdrm-ptr)   echo "ID_INPUT=1 ID_INPUT_MOUSE=1" ;;
        vdrm-touch) echo "ID_INPUT=1 ID_INPUT_TOUCHSCREEN=1" ;;
        *)          echo "ID_INPUT=1" ;;
    esac
}
for p in /sys/class/input/event*/device/name; do
    [ -e "$p" ] || continue
    name=$(cat "$p" 2>/dev/null)
    case "$name" in vdrm-*) ;; *) continue ;; esac
    ev=$(basename "$(dirname "$(dirname "$p")")")
    d=$(cat "/sys/class/input/$ev/dev" 2>/dev/null) || continue
    [ -n "$d" ] || continue
    {
        echo "I:$(date +%s)000000"
        for kv in $(udev_tag "$name"); do echo "E:$kv"; done
        echo "G:seat"; echo "Q:seat"
    } > "/run/udev/data/c$d"
    udev_tag_add seat "c$d"
done

# logind 的 session_device_verify 按 eventN 的父 inputN 查 seat
for p in /sys/class/input/input*/name; do
    [ -e "$p" ] || continue
    name=$(cat "$p" 2>/dev/null)
    case "$name" in vdrm-*) ;; *) continue ;; esac
    in=$(basename "$(dirname "$p")")
    {
        echo "I:$(date +%s)000000"
        echo "G:seat"; echo "Q:seat"
    } > "/run/udev/data/+input:$in"
    udev_tag_add seat "+input:$in"
done

# ---- sound ----
card=$(sed -n 's/^ *\([0-9][0-9]*\) \[vdrm.*/\1/p' /proc/asound/cards 2>/dev/null | head -1)
if [ -n "$card" ]; then
    for s in /sys/class/sound/controlC"$card" /sys/class/sound/pcmC"$card"D* /sys/class/sound/timer; do
        [ -e "$s" ] || continue
        d=$(cat "$s/dev" 2>/dev/null) || continue
        [ -n "$d" ] || continue
        {
            echo "I:$(date +%s)000000"
            echo "E:ID_PATH=platform-vdrm"
            echo "E:ID_FOR_SEAT=sound-platform-vdrm"
            echo "G:seat"; echo "Q:seat"
        } > "/run/udev/data/c$d"
        udev_tag_add seat "c$d"
    done
fi

echo "vdrm-container-setup: udev data written (no mknod/mount)"
