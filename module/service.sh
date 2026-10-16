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

insmod "$KO" || { log -t vdrm "insmod vdrm-$KMI.ko 失败"; exit 1; }
log -t vdrm "loaded vdrm-$KMI.ko"

# 2) /dev/vdrm 对 App 放行（DAC + SELinux 标签）。
#    这两句以前在 App 里靠 su 做 —— 所以 App 一直需要 root；现在归模块管，
#    App 就是个普通应用，不需要 KernelSU 授权。
[ -e /dev/vdrm ] && { chmod 666 /dev/vdrm; chcon u:object_r:gpu_device:s0 /dev/vdrm; }

# 3) 建池。mkpool 就在模块目录里，直接跑它 —— 不再往别处拷一份。
#    （以前拷到 /data/wayland/vdrm_mkpool，那是守护进程时代留下的路径，已退役。
#     它自己的日志写在 /proc/self/exe 旁边，所以也跟着模块走。）
#    mkpool 要够到 gralloc（binder → allocator HAL），得在能到它的域里跑；
#    服务域通常够，不够就退回 su（ksu 域，实测能到）。
[ -x "$MODDIR/vdrm_mkpool" ] || { log -t vdrm "模块里没有 vdrm_mkpool"; exit 0; }

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
[ -x "$MODDIR/status.sh" ] && "$MODDIR/status.sh"
