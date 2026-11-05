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

# 2) /dev/vdrm、/dev/vdrm-audio：gpu_device 标签 + 0666。
#
# 不做 chown：0666 让第一次安装/重装换 UID 都能直接 open，不需要查 UID、不需要
# 重启。SELinux 边界是 gpu_device —— 有该权限的域能打开，没有的域打不开；
# 不要求“只有 VDRM App 能开”。mkpool 以 root 运行，不受影响。
# ueventd 可能在 insmod 返回之后才把节点建出来，直接 chmod 会落空
# （实测 /dev/vdrm-audio 会停在 0600）。先等两个节点都出现。
vdrm_fix_nodes() {
    i=0
    while [ "$i" -lt 5 ]; do
        [ -e /dev/vdrm ] && [ -e /dev/vdrm-audio ] &&
            [ -e /dev/vdrm-audio-in ] && break
        sleep 1
        i=$((i + 1))
    done
    for n in /dev/vdrm /dev/vdrm-audio /dev/vdrm-audio-in; do
        [ -e "$n" ] || { vdrm_log "vdrm perms: $n missing"; continue; }
        chmod 666 "$n"
        chcon u:object_r:gpu_device:s0 "$n"
        vdrm_log "vdrm perms: $n 0666 gpu_device"
    done
}

# 3) 把虚拟键鼠的 evdev 节点从 Android 手里拿掉。
#
# 只 chmod 是不够的：
#   - 已经 open 的 fd 收不回来（chmod 只影响之后的 open）；
#   - ueventd 建节点是异步的，开机时可能晚于脚本 —— 追着改权限永远有窗口，
#     谁先跑到只能靠运气（第一次开机实测就输在这：脚本 5 秒窗口过了节点才出来，
#     Android 先开走了）。
#
# 所以换成"删掉、再由我们重建"：
#   1. rm /dev/input/eventN
#      → EventHub 收到 inotify IN_DELETE，把设备 close 掉，InputReader 里立刻
#        没有这台设备（实测 dumpsys input 的 vdrm 行数掉到 0）；
#   2. mknod 同样的主次设备号，chmod 0600 root:root
#      → EventHub 收到 IN_CREATE 去 open，EACCES，跳过。
#
# 于是不管节点什么时候建出来、Android 有没有抢在前面，最终都停在"Android 看不见、
# 容器照样能用"（容器用自己的 /dev 里的节点，libinput 以 root 打开，够权限）。
# 这个"删了重建"由 post-fs-data.sh / service.sh 在模块加载后跑一次（带重试）；
# 手动 reload 的脚本（tools/vdrm_up.sh --reload）也会补一刀。不再有常驻 watcher。
#
# 不动 SELinux 标签：input_device 这个类型 root 照样能开，换它没必要。

vdrm_hide_input_once() {
    VDRM_HIDDEN=0
    for p in /sys/class/input/event*/device/name; do
        [ -e "$p" ] || continue
        case "$(cat "$p" 2>/dev/null)" in
            vdrm-*) ;;
            *) continue ;;
        esac
        ev=$(basename "${p%/device/name}")
        node="/dev/input/$ev"

        # 已经是我们的节点（0600）就别再删一次了：每删一次 InputReader 都要重扫
        if [ -c "$node" ] && [ "$(stat -c %a "$node" 2>/dev/null)" = "600" ]; then
            VDRM_HIDDEN=$((VDRM_HIDDEN + 1))
            continue
        fi

        d=$(cat "/sys/class/input/$ev/dev" 2>/dev/null)     # 形如 13:71
        [ -n "$d" ] || continue

        rm -f "$node"
        mknod "$node" c "${d%%:*}" "${d##*:}" 2>/dev/null
        chown root:root "$node" 2>/dev/null
        chmod 600 "$node" 2>/dev/null
        if [ -c "$node" ] && [ "$(stat -c %a "$node" 2>/dev/null)" = "600" ]; then
            VDRM_HIDDEN=$((VDRM_HIDDEN + 1))
            vdrm_log "input hide: $node ($d) 删掉重建为 0600"
        fi
    done
}

# 单次处理，带重试：给 post-fs-data 用（那时节点可能还没被 ueventd 建出来）。
# 15 秒足够覆盖 ueventd 的异步建节点；跑完就退，不再常驻。
vdrm_hide_input() {
    i=0
    while [ "$i" -lt 30 ]; do
        vdrm_hide_input_once
        [ "$VDRM_HIDDEN" -ge 3 ] && return 0
        sleep 0.5
        i=$((i + 1))
    done
    vdrm_log "input hide: 只处理到 $VDRM_HIDDEN 个 vdrm 节点"
    return 0
}

