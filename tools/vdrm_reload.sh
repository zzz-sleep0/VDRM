#!/system/bin/sh
# 重载 vdrm.ko，并把 Android 侧设备节点的权限/标签重新修好。
# 必须走这个脚本，不要裸 insmod：裸 insmod 会绕过 vdrm_fix_nodes/vdrm_hide_input，
# 新 input 会以 0660 root:input 暴露给 Android，变成"幽灵输入设备"。
MODDIR=${0%/*}
. "$MODDIR/common.sh"

am force-stop com.vdrm.consumer >/dev/null 2>&1
rmmod vdrm 2>/dev/null
vdrm_load || exit 1
vdrm_fix_nodes
vdrm_hide_input
