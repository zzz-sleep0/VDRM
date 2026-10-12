# 设备专用参考与验证范围

这些头文件来自用户原先能够上屏的 ko-backup；vdrm-vblank-experiment.ko 是本次实际加载且 glmark2-wayland 性能恢复的模块。它的 SHA256 为 830b3612c3f0fdd6e8c0fceddac80fe5f97997e7d4c2666dbb5286b8b3e93653。

crcs.tsv 的原有 76 条来自工作模块 __versions，arm/get 来自本次宿主采集并交叉验证，put 来自 msm_drm.ko 与宿主采集一致。该表是复现夹具，不是新全量采集器的实测输出。

hashes.tsv 是从已验证模块回调哈希生成的复现夹具；它不是本次新采集器输出的内核基准，不可用它证明其他设备的 ABI。

config.txt 和 kernel-release.txt 是当前宿主身份；vermagic.txt 保留已验证模块的值，与 uname 不完全相同。本次实机加载说明该设备配置允许此组合，但不是跨设备保证。

新版 prepare/finish 的 reference 模式已在容器干净构建，78 条 CRC 全部存在；全量 collector.kpm 已编译、链接并检查无 GOT。新的全量采集器未在宿主加载实测。新设备 BTF 生成流程需要 pahole 和目标快照；本次未执行跨设备移植测试。

复现命令（包根目录执行）：

```sh
python3 scripts/port.py prepare --reference --kp /path/to/KPatch-Next-main --target examples/reference --build /tmp/vdrm-reference-build
python3 scripts/port.py finish --build /tmp/vdrm-reference-build --capture examples/reference
```

不应把参考目录产物作为未知设备默认值。驱动中仍有目标相关常量（GFP、部分类型和 DRM offset 等）；BTF 自动生成并不等价于完整移植证明。必须阅读并核对这些常量与目标 ABI，测试加载、模式切换、buffer RETURN 和卸载行为。
