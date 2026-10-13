# VDRM

把一台 Android 手机变成**虚拟 DRM 显示器**（`/dev/vdrm`，虚拟 `card1`），
让 Linux 合成器（KWin / Hyprland / sway …）跑在容器里、画面显示在手机屏幕上，
同时提供一套**通用**的虚拟键鼠。

不是投屏，也不是 X11 转发 —— 容器里的合成器看到的就是一块真实的 DRM 卡。
虚拟键鼠走**内核 evdev**，所以任何用 libinput 的合成器都通用，不需要为每个桌面环境写适配。

---

## 三条链路

```
① 显示   合成器（容器）──写池缓冲──► KO ──► App 用 AHB 重建 ──► SurfaceFlinger ──► 屏幕
② 输入   App 触控板/键盘 ──ioctl(/dev/vdrm)──► KO ──input_event()──► evdev ──► 合成器
③ 屏蔽   虚拟键鼠是内核级设备，Android 侧也会认到 → 用排除表按设备名屏蔽掉
```

池由 KO 持有，App 重启不影响；虚拟键鼠由 KO 注册，生命周期绑在模块上。

---

## 目录

| 路径 | 是什么 |
|---|---|
| `app/` | **Android 应用** —— 显示消费端 + 输入注入。标准 Gradle 工程，`.github/workflows/build.yml` 会在推送时自动编 APK |
| `vdrm-ko-ddk/` | **内核模块**。标准 kbuild，在 DDK 容器里按 KMI 编译；改动的来龙去脉见其中的 `CHANGES.md` |
| `vendor-overlay/` | **屏蔽模块** —— 让 Android 的 InputReader 忽略 `vdrm-kbd` / `vdrm-ptr` |

---

## 怎么跑起来

- 1.根据`先看我.md`中提到的，从你的设备上提取`excluded-input-devices.xml`文件，加上

`<device name="vdrm-kbd"/>`

`<device name="vdrm-ptr"/>`

两行补丁，之后修改`vdrm-input-overlay.zip`模块中的文件，安装元模块并刷入`vdrm-input-overlay`模块

- 2.下载安装app，下载内核模块，之后执行

`insmod /path/to/your/file/vdrm*.ko`

- 3.下载lxc容器镜像并安装到DroidSpaces中(lxc images中下载即可，如果想立即体验，推荐Arch，以下以Arch为例)(注：这之后带*的步骤可以省略，不过可能会丢失硬件加速)

  容器配置中需要

  **配置环境变量**
  
  `XDG_RUNTIME_DIR=/home/alarm/.xdg`

  `LIBSEAT_BACKEND=noop`

  `AQ_DRM_DEVICES=/dev/dri/card1`

  `MESA_LOADER_DRIVER_OVERRIDE=kgsl`

  (注：此处环境变量仅在hyprland，niri和kde合成器上得到验证，其他合成器请自行探索)

  **挂载对应设备节点**
  `/dev/dri/card1→/dev/dri/card1`→虚拟DRM设备
  `/dev/input/event10→/dev/input/event10`→虚拟键盘
  `/dev/input/event10→/dev/input/event11`→虚拟鼠标

  *在容器中打上`lfdevs`大佬的`mesa-for-android-container`对应你容器的驱动

  **在容器中安装相应vdrm后端**(此处存放路径仅仅适用于Arch！其他系统请自行查找！)
  在容器的/usr/lib/gbm目录下放置`vdrm_gbm.so`(看release，一起下发)

  
- 4.打开一次app，这个是加载`vdrm*.ko`后需要先做的一个时序性问题，否则合成器可以正常启动，但是app无法显示合成器的画面，为黑屏表现，后续app可以随便关停，无需在意(时序问题做反已经黑屏了？没问题，关掉合成器再重新打开即可)

- 5.安装合成器，随后就可以快乐玩耍了


---

## 鸣谢

- Goldzxcbug，Superturtlee，项目引路人以及参考项目的开发者

- Dere3046，提供了项目普适化的必要工具以及解决问题的思路
 
- lfdevs大佬，该项目想要拥有硬件加速需要打他的驱动
 
- anland项目，提供了初期思路以及大部分键鼠触控优化方法
 
- mesa-for-android-container项目，该项目硬件加速所需要的驱动来源于此


## 许可

GPL-2.0
