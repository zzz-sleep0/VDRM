#!/system/bin/sh
# 卸载：把自己留下的东西【全收掉】，不指望管理器帮忙。
#
# 管理器（KernelSU / Magisk）点"移除"时的顺序是：先跑本脚本、再删模块目录 ——
# 所以按契约，目录其实不用自己动。但自己动更保险：手工删目录、或者管理器哪天
# 换了行为，这里一样是干净的。
#
# 唯一【不用管】的是 /vendor/etc/excluded-input-devices.xml：那是 metamodule 用
# overlayfs 盖上去的一层，模块目录一没、那层就没了，ROM 原版自己回来 —— 不需要
# 谁去"还原"它。

ID=vdrm-lkm

# 1) 内核里那个模块。合成器或 App 正占着 DRM 设备就会失败 —— 那就等重启，重启后
#    它自然不会再被加载。尽力而为，不阻断后面的清理。
rmmod vdrm 2>/dev/null

# 2) 早期版本把建池 helper 和它的日志拷到 /data/wayland（当时那个路径还叫这名）。
#    它已退役：helper 改成"就地跑、日志写自己旁边"，这两份是没人用的残留。
rm -f /data/wayland/vdrm_mkpool /data/wayland/mkpool.log

# 3) 更早一次【手工安装】留下的 ROM 备份。现在覆盖靠 metamodule，没这一步。
rm -f /data/adb/excluded-input-devices.xml.bak

# 4) 模块目录自己，外加可能存在的更新暂存目录。
#    —— 放在【最后】一行，因为这一步会把自己也删掉：shell 已把本文件读进内存了，
#    删了不影响正在跑的这几步，但放最后更稳妥。
rm -rf "/data/adb/modules/$ID" "/data/adb/modules_update/$ID"
