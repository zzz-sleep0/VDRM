#!/system/bin/sh
# VDRM —— 模块服务（boot 的 late_start 阶段）
#
# 内核模块、/dev/vdrm 的权限、把虚拟键鼠的 evdev 节点对 Android 藏起来 ——
# 这三件已经在 post-fs-data.sh 里做完了（必须早于 system_server，原因见
# common.sh 开头的长注释）。这里只做必须晚做的：建池、写状态。
#
# 池必须早于合成器第一次 dumb_create 就绪：合成器只分配一次缓冲然后一直用，
# 池空时 dumb_create 走 heap 回退，那批缓冲不在池里、pipe_update 匹配不上，
# 一个帧事件都推不出去 —— 表现就是黑屏（关掉合成器再开才好）。
# 所以放在 boot 阶段建好，不再依赖"先开一次 App"。
#
# 池里的槽必须带 gralloc 出身（fd0 + fd1 metadata blob + ints），内核造不出来，
# 建池天然属于用户态 —— 而这里（模块服务 + root）就是最省事的用户态。

MODDIR=${0%/*}
. "$MODDIR/common.sh"

# post-fs-data.sh 没跑到（老管理器、手工装模块）时这里兜一次：设备还是能起来，
# 只是这次启动 Android 会看见虚拟键鼠 —— 重启一次就干净了。
vdrm_load || exit 1
vdrm_sync_nodes

# 2) 建池。mkpool 就在模块目录里，直接跑它 —— 不再往别处拷一份。
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

# 3) 把状态写进 module.prop 的 description —— 管理器直接显示那一行，不用点任何东西。
#    照 Droidspaces 的做法（它的 service.sh 也是这么更新自己那行的）。
#    放在最后，因为要反映的正是上面几步的结果。
if [ -f "$MODDIR/status.sh" ]; then
    chmod 755 "$MODDIR/status.sh" 2>/dev/null
    "$MODDIR/status.sh"
fi
