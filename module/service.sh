#!/system/bin/sh
# VDRM —— 加载内核模块 + 把池建起来
#
# 池必须早于合成器第一次 dumb_create 就绪：合成器只分配一次缓冲然后一直用，
# 池空时 dumb_create 走 heap 回退，那批缓冲不在池里、pipe_update 匹配不上，
# 一个帧事件都推不出去 —— 表现就是黑屏（关掉合成器再开才好）。
# 所以放在 boot 阶段建好，不再依赖"先开一次 App"。
#
# 池里的槽必须带 gralloc 出身（fd0 + fd1 metadata blob + ints），内核造不出来，
# 建池天然属于用户态 —— 而这里（模块服务 + root）就是最省事的用户态。

MODDIR=${0%/*}

# 1) 挑对 KMI 的 .ko。
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
REL=$(uname -r)
VER=$(echo "$REL" | cut -d. -f1,2)
AND=$(echo "$REL" | sed -n 's/.*\(android[0-9][0-9]*\).*/\1/p')
KMI="$AND-$VER"

KO="$MODDIR/vdrm-$KMI.ko"
if [ ! -f "$KO" ]; then
    log -t vdrm "没有 vdrm-$KMI.ko（uname -r = $REL）——不加载"
    exit 1
fi

if lsmod | grep -q '^vdrm '; then
    log -t vdrm "vdrm already loaded; skip insmod"
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
    insmod "$KO" $INSMOD_ARGS || { log -t vdrm "insmod vdrm-$KMI.ko 失败"; exit 1; }
    log -t vdrm "loaded vdrm-$KMI.ko ${INSMOD_ARGS:+($INSMOD_ARGS)}"
fi

# 稳定节拍：flip_done_immediate 默认 Y 在 120 下会让翻转立即完成，反而掉帧；设 0。
if [ -e /sys/module/vdrm/parameters/flip_done_immediate ]; then
    echo 0 > /sys/module/vdrm/parameters/flip_done_immediate
fi

# 2) /dev/vdrm 只放给 VDRM App（DAC + SELinux 标签）。
#
#    以前是 666，任何 App 都能打开它 —— 既能向容器注入键鼠/触摸，也能用
#    GET_POOL 把池缓冲的 dma-buf fd 装进自己进程。现在 chown 到 App 的
#    uid 并保持 0600；mkpool 以 root 运行，不受影响。
#
#    UID 查询失败时保持 600 root-only（App 打不开，但绝不会误放行）。
#    App 卸载重装若 uid 变了，重刷模块或重启一次即可。
if [ -e /dev/vdrm ]; then
    APP_UID=$(cmd package list packages -U 2>/dev/null \
              | sed -n 's/.*package:com\.vdrm\.consumer uid:\([0-9][0-9]*\).*/\1/p' \
              | head -n1)
    [ -z "$APP_UID" ] && APP_UID=$(stat -c '%u' /data/user/0/com.vdrm.consumer 2>/dev/null)
    [ -z "$APP_UID" ] && APP_UID=$(stat -c '%u' /data/data/com.vdrm.consumer 2>/dev/null)

    if [ -n "$APP_UID" ]; then
        chown "$APP_UID:$APP_UID" /dev/vdrm
        chmod 600 /dev/vdrm
        log -t vdrm "vdrm perms: owner=$APP_UID mode=600"
    else
        chmod 600 /dev/vdrm
        log -t vdrm "vdrm perms: app uid not found, root-only"
    fi
    chcon u:object_r:gpu_device:s0 /dev/vdrm
fi

# 3) 建池。mkpool 就在模块目录里，直接跑它 —— 不再往别处拷一份。
#    （以前拷到 /data/wayland/vdrm_mkpool，那是守护进程时代留下的路径，已退役。
#     它自己的日志写在 /proc/self/exe 旁边，所以也跟着模块走。）
#    mkpool 要够到 gralloc（binder → allocator HAL），得在能到它的域里跑；
#    服务域通常够，不够就退回 su（ksu 域，实测能到）。
# 模块管理器解压 zip 时可能不保留可执行位（实测重装后是 644），
# 所以这里只判存在，再把需要的文件补成 755。
[ -f "$MODDIR/vdrm_mkpool" ] || { log -t vdrm "模块里没有 vdrm_mkpool"; exit 0; }
chmod 755 "$MODDIR/vdrm_mkpool" 2>/dev/null

if "$MODDIR/vdrm_mkpool"; then
    log -t vdrm "pool built"
elif su -c "$MODDIR/vdrm_mkpool"; then
    log -t vdrm "pool built (via su)"
else
    log -t vdrm "mkpool 失败 —— 池留给 App 的 ensure_pool_spawn 兜底"
fi

# 4) 把状态写进 module.prop 的 description —— 管理器直接显示那一行，不用点任何东西。
#    照 Droidspaces 的做法（它的 service.sh 也是这么更新自己那行的）。
#    放在最后，因为要反映的正是上面几步的结果。
if [ -f "$MODDIR/status.sh" ]; then
    chmod 755 "$MODDIR/status.sh" 2>/dev/null
    "$MODDIR/status.sh"
fi
