# 屏蔽性挂载：让 Android 不去认那对虚拟键鼠

## 为什么需要

虚拟键鼠是**内核级**设备 —— `vdrm.ko` 直接注册出 `vdrm-kbd` / `vdrm-ptr`
两个 evdev 设备（早期版本用 uinput + 用户态守护进程，现在设备搬进内核了，
但"是内核级设备"这一点没变，所以这条屏蔽**依然必需**）。

好处是任何用 libinput 的合成器（KWin / Hyprland / sway / Xorg）都能直接用，
不需要为每个合成器写私有虚拟输入协议。

代价是 **Android 自己也会认到它们**：系统的 InputReader 会把它们当成真实键鼠，
于是注入的按键/移动会**同时**作用到 Android 和容器里的桌面。

## 怎么屏蔽

Android 支持按**设备名**排除输入设备：`/vendor/etc/excluded-input-devices.xml`。
在里面加上两行即可：

```xml
    <device name="vdrm-kbd"/>
    <device name="vdrm-ptr"/>
```

`vendor/etc/excluded-input-devices.xml` 就是加好这两行的文件（其余内容与原文件一致）。

## 怎么装

`/vendor` 是只读 erofs，改不了。用 Magisk / KernelSU 模块做覆盖挂载：

```
module.prop                          # 模块声明
vendor/etc/excluded-input-devices.xml  # 会被挂到 /vendor/etc/ 上
```

将该目录下.zip模块中的文件按照你自己设备真实情况patch上新增的两行，改好后刷入设备即可