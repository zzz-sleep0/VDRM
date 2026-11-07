#!/system/bin/sh
# VDRM —— boot 最早的一段（post-fs-data，早于 zygote / system_server）
#
# 只做"必须早做"的事：加载 .ko、修 /dev/vdrm 的权限和标签、把虚拟键鼠的
# evdev 节点设成 0600 —— 让 Android 的 InputReader 从头就打不开它们。
# 为什么必须这么早、为什么路径不能挪，见 common.sh 开头的长注释。
#
# 建池不在这里：mkpool 要够到 gralloc（binder → allocator HAL），那要等
# 系统服务起来，放在 service.sh 里。

MODDIR=${0%/*}
. "$MODDIR/common.sh"

vdrm_load || exit 0
vdrm_sync_nodes
