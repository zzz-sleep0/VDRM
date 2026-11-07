#!/system/bin/sh
# VDRM —— post-fs-data.sh 和 service.sh 共用的一段。
#
# 为什么要把内核模块尽量早加载（post-fs-data，早于 zygote / system_server）：
#
# Android 的 InputReader 会把 vdrm 这三台虚拟设备当真设备打开 —— 屏幕上冒出
# 一个莫名的大鼠标，用户点容器界面时 Android 那边也跟着点。要挡住它只有一招：
# 在它扫 /dev/input 之前把节点改成 0600 root:root。system_server 是
# uid system(1000) + 附加组 input(1004)，0600 打不开；它也没有
# CAP_DAC_OVERRIDE（实测 CapEff=0x1a06887c20，那一位是空的）。
#
# 节点路径没法挪走。ueventd.rc 里写着
#     subsystem input
#         devname uevent_devpath
#         dirname /dev/input
# 节点因此固定是 /dev/input/<sysfs 名>，内核 uevent 里的 DEVNAME 根本不参与
# 选路（给 input class 的 devnode 回调包过一层，改 DEVNAME 为 vdrm-input/…，
# 节点照样落在 /dev/input，实测）。
#
# 容器那边不受影响：容器的 /dev/input 是它自己 udev 冷插拔建的节点
# （0600 root root），libinput 以 root 打开，够用。

vdrm_log() {
    log -t vdrm "$@"
}

# 1) 挑对 KMI 的 .ko，加载它。
#
# 【文件名就是 KMI】，由 CI 定死（见 .github/workflows/build-vdrm-ko.yml）：
#
#     vdrm-android14-6.1.ko
#
# KMI 从 uname -r 拼出来 —— GKI 的版本串里同时带着版本号和 android 大版本：
#
#     6.1.141-android14-11-o-gca13bffobf09
#     └─6.1─┘  └─android14─┘
#
# 光看版本号不够：CI 里 5.15 有 android13/android14 两档。
#
# 对不上就不加载。那不是这里该兜的事 —— 说明模块里缺这一档的 .ko，
# 该去补编，报出来比糊过去强。
vdrm_load() {
    REL=$(uname -r)
    VER=$(echo "$REL" | cut -d. -f1,2)
    AND=$(echo "$REL" | sed -n 's/.*\(android[0-9][0-9]*\).*/\1/p')
    KMI="$AND-$VER"

    KO="${KO:-$MODDIR/vdrm-$KMI.ko}"
    if [ ! -f "$KO" ]; then
        vdrm_log "没有 vdrm-$KMI.ko（uname -r = $REL）——不加载"
        return 1
    fi

    if lsmod | grep -q '^vdrm '; then
        vdrm_log "vdrm already loaded; skip insmod"
    else
        # 分辨率：mode.conf 的 mode=1920x1080（默认，留黑边）/ 2376x1080（铺满，挖空）
        # / auto（开机采当前逻辑分辨率，竖屏自动转横屏）。没有配置就按 .ko 默认 1920x1080。
        MODE=$(sed -n 's/^mode=//p' "$MODDIR/mode.conf" 2>/dev/null | tr -d ' \r' | tail -n1)
        if [ "$MODE" = "auto" ]; then
            MODE=$(wm size 2>/dev/null | sed -n 's/.*Override size: *//p' | head -n1)
            [ -z "$MODE" ] && MODE=$(wm size 2>/dev/null | sed -n 's/.*Physical size: *//p' | head -n1)
            if [ -n "$MODE" ]; then
                W=${MODE%x*}
                H=${MODE#*x}
                case "$W$H" in *[!0-9]*) MODE= ;; esac
                if [ -n "$MODE" ] && [ "$W" -lt "$H" ]; then
                    MODE="${H}x${W}"
                fi
            fi
        fi
        INSMOD_ARGS=
        case "$MODE" in
            [0-9]*x[0-9]*) INSMOD_ARGS="mode=$MODE" ;;
        esac
        # dpi=：EDID 里报的 DPI。合成器拿它算 scale（niri 实测 scale ≈ dpi/128）：
        # 越大 → 逻辑分辨率越小、UI 越大。0/没写 = 用 .ko 默认（160x90mm ≈ 305dpi）。
        DPI=$(sed -n 's/^dpi=//p' "$MODDIR/mode.conf" 2>/dev/null | tr -d ' \r' | tail -n1)
        case "$DPI" in
            ''|*[!0-9]*) ;;
            *) INSMOD_ARGS="$INSMOD_ARGS edid_dpi=$DPI" ;;
        esac
        # refresh=：虚拟输出的刷新率（Hz，24..240，默认 120）。也可以运行时改
        # （echo 到 parameters/refresh + commit），这里只是开机种子。
        REFRESH=$(sed -n 's/^refresh=//p' "$MODDIR/mode.conf" 2>/dev/null | tr -d ' \r' | tail -n1)
        case "$REFRESH" in
            ''|*[!0-9]*) ;;
            *)
                if [ "$REFRESH" -ge 24 ] && [ "$REFRESH" -le 240 ]; then
                    INSMOD_ARGS="$INSMOD_ARGS refresh=$REFRESH"
                else
                    vdrm_log "mode.conf: refresh=$REFRESH 超出 24..240，忽略"
                fi
                ;;
        esac
        insmod "$KO" $INSMOD_ARGS || { vdrm_log "insmod vdrm-$KMI.ko 失败"; return 1; }
        vdrm_log "loaded vdrm-$KMI.ko ${INSMOD_ARGS:+($INSMOD_ARGS)}"
    fi
    return 0
}

# 2) 设备节点正确性 —— GKI 没有 devtmpfs，节点只能由 ueventd 建。
#
# 先把事实摆清楚，免得把"节点是原生的"和"节点是对的"混成一件事：
#
#   - `CONFIG_DEVTMPFS` 在 GKI 上是 n，`/dev` 是 init 挂的 tmpfs。节点由 Android
#     的 ueventd 在收到 KOBJ_ADD 时 mknod 出来 —— 名字和主次号都来自内核，
#     这一段是原生的，我们不插手的部分。
#   - 但 ueventd 只在 ADD 时动作，KOBJ_REMOVE 不删节点。rmmod 之后旧节点照旧
#     躺在 /dev 里，重新 insmod 拿到的主次号一变，它们就是指向死设备的空壳。
#   - 权限和 SELinux 标签：`/vendor/etc/ueventd.rc` 里没有我们的条目，而它
#     在 init 第二阶段就读完了，早于模块挂载 —— 模块改不动它。
#
# 所以这里只补 ueventd 做不到的三件事，全部以 sysfs 的 `dev` 为准：
#
#   1. 主次号对不上（或节点缺失）就删了重建 —— 这是正确性，不是补丁；
#   2. 权限 / owner / 标签按设备类型给（App 走 gpu_device、VT 按原生
#      `0620 root:tty`、虚拟输入 0600 root:root 挡 Android、声卡 system:audio）；
#   3. 虚拟输入节点必须"删了重建"才会触发 EventHub 的 IN_DELETE / IN_CREATE，
#      只 chmod 收不回已经打开的 fd。
#
# 只认 /sys 里属于 vdrm 的设备：drm / snd 的 realpath 里含 `/platform/vdrm`，
# tty 看 `/sys/class/tty/tty0/active` 在不在（内核自带 VT 时模块根本不会注册，
# 这个文件也就没有）。任何一步都不对 event* 之类做通配。

vdrm_node_ok() {   # $1=节点 $2=期望的 hex 主次号
    [ -c "$1" ] || return 1
    [ "$(stat -c '%t:%T' "$1" 2>/dev/null)" = "$2" ]
}

vdrm_sync_node() { # $1=节点 $2=sysfs 目录 $3=模式 $4=owner $5=标签 $6=kick
    n=$1; s=$2; mode=$3; own=$4; ctx=$5; kick=$6
    dev=$(cat "$s/dev" 2>/dev/null) || return 1
    maj=${dev%%:*}; min=${dev##*:}
    want=$(printf '%x:%x' "$maj" "$min")

    # 设备号和权限都对：只补标签（ueventd 重建后标签会退回默认类型）
    if vdrm_node_ok "$n" "$want" && [ "$(stat -c %a "$n" 2>/dev/null)" = "$mode" ]; then
        chcon "$ctx" "$n" 2>/dev/null
        return 0
    fi

    # 设备号对、只是权限不对：kick=1 的（虚拟输入）也要删了重建，
    # 否则 InputReader 手里那个 fd 还开着，chmod 对它无效。
    if vdrm_node_ok "$n" "$want" && [ "$kick" != 1 ]; then
        chmod "$mode" "$n" 2>/dev/null
        chown "$own" "$n" 2>/dev/null
        chcon "$ctx" "$n" 2>/dev/null
        return 0
    fi

    rm -f "$n"
    mknod "$n" c "$maj" "$min" 2>/dev/null || return 1
    chmod "$mode" "$n" 2>/dev/null
    chown "$own" "$n" 2>/dev/null
    chcon "$ctx" "$n" 2>/dev/null
    vdrm_log "node: $n 重建（$dev $mode $own）"
    return 0
}

vdrm_sync_once() {
    VDRM_PENDING=0

    # 控制节点：App 要 0666 直接 open，不 chown（换 UID 重装也不用重启）
    for n in vdrm vdrm-audio vdrm-audio-in; do
        if [ -e "/sys/class/misc/$n" ]; then
            vdrm_sync_node "/dev/$n" "/sys/class/misc/$n" 666 0:0 \
                u:object_r:gpu_device:s0 0 || VDRM_PENDING=$((VDRM_PENDING + 1))
        else
            VDRM_PENDING=$((VDRM_PENDING + 1))
        fi
    done

    # 虚拟 DRM：ueventd 已按 drm 规则给 0666 root:graphics + graphics_device
    for c in /sys/class/drm/card[0-9]*; do
        case "$(basename "$c")" in *-*) continue ;; esac   # card1-Virtual-3 是连接器，不是设备
        case $(readlink -f "$c" 2>/dev/null) in
            */platform/vdrm/*)
                vdrm_sync_node "/dev/dri/$(basename "$c")" "$c" 666 0:1003 \
                    u:object_r:graphics_device:s0 0 || VDRM_PENDING=$((VDRM_PENDING + 1))
                ;;
        esac
    done

    # 虚拟 VT：原生 VT 的惯例是 0620 root:tty，session 用户不必是 root 也能拿
    if [ -e /sys/class/tty/tty0/active ]; then
        for t in 0 1 2 3 4 5 6; do
            vdrm_sync_node "/dev/tty$t" "/sys/class/tty/tty$t" 620 0:5 \
                u:object_r:tty_device:s0 0 || VDRM_PENDING=$((VDRM_PENDING + 1))
        done
    fi

    # 虚拟键鼠/触摸：0600 root:root，Android 的 InputReader 打不开
    for e in /sys/class/input/event*; do
        case "$(cat "$e/device/name" 2>/dev/null)" in
            vdrm-*)
                vdrm_sync_node "/dev/input/$(basename "$e")" "$e" 600 0:0 \
                    u:object_r:input_device:s0 1 || VDRM_PENDING=$((VDRM_PENDING + 1))
                ;;
        esac
    done

    # 虚拟声卡：system:audio 0660，和原生声卡一致
    for c in /sys/class/sound/controlC[0-9]* /sys/class/sound/pcmC*; do
        case $(readlink -f "$c" 2>/dev/null) in
            */platform/vdrm/*)
                vdrm_sync_node "/dev/snd/$(basename "$c")" "$c" 660 1000:1005 \
                    u:object_r:audio_device:s0 0 || VDRM_PENDING=$((VDRM_PENDING + 1))
                ;;
        esac
    done
}

# ueventd 建节点是异步的，开机那一下可能还没出来：15 秒内轮询，全就位就退。
vdrm_sync_nodes() {
    i=0
    while [ "$i" -lt 30 ]; do
        vdrm_sync_once
        [ "$VDRM_PENDING" -eq 0 ] && return 0
        sleep 0.5
        i=$((i + 1))
    done
    vdrm_log "node: 还有 $VDRM_PENDING 个没就位"
    return 0
}

# 兼容旧调用点（tools/、文档里还写着这两个名字）：收敛到同一条路径。
vdrm_fix_nodes() { vdrm_sync_nodes; }
vdrm_hide_input() { vdrm_sync_nodes; }
