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

# 1) 挑对 KMI 的 .ko：文件名形如 vdrm-6.1.ko，按 uname -r 的主次版本选
REL=$(uname -r | cut -d. -f1,2)
KO="$MODDIR/vdrm-$REL.ko"
[ -f "$KO" ] || KO="$MODDIR/vdrm.ko"
[ -f "$KO" ] || { log -t vdrm "没有对应 $REL 的 vdrm.ko"; exit 1; }

insmod "$KO" && log -t vdrm "loaded $(basename "$KO")"

# 2) /dev/vdrm 对 App 放行（DAC + SELinux 标签）。
#    这两句以前在 App 里靠 su 做 —— 所以 App 一直需要 root；现在归模块管，
#    App 就是个普通应用，不需要 KernelSU 授权。
[ -e /dev/vdrm ] && { chmod 666 /dev/vdrm; chcon u:object_r:gpu_device:s0 /dev/vdrm; }

# 3) 建池。mkpool 要够到 gralloc（binder → allocator HAL），得在能到它的域里跑；
#    服务域通常够，不够就退回 su（ksu 域，实测能到）。
[ -x "$MODDIR/vdrm_mkpool" ] || { log -t vdrm "模块里没有 vdrm_mkpool"; exit 0; }
mkdir -p /data/wayland
cp -f "$MODDIR/vdrm_mkpool" /data/wayland/vdrm_mkpool && chmod 755 /data/wayland/vdrm_mkpool

if "$MODDIR/vdrm_mkpool"; then
    log -t vdrm "pool built"
elif su -c "$MODDIR/vdrm_mkpool"; then
    log -t vdrm "pool built (via su)"
else
    log -t vdrm "mkpool 失败 —— 池留给 App 的 ensure_pool_spawn 兜底"
fi
