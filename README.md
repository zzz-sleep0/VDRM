# 前置声明！！理论上该模块仅适用于遵循GKI(OKI)标准的内核！！如果你的厂商对内核有魔改行为，或者你刷入了深度优化魔改的内核，请勿在没有音量键救砖的情况下刷入此模块，不遵循以上声明，由此模块引发的问题作者概不负责！

# VDRM

把一台 Android 手机变成**虚拟 DRM 显示器**（`/dev/vdrm`，虚拟 `card1`），
让 Linux 合成器（KWin / niri / Hyprland / sway …）跑在容器里、画面显示在手机屏幕上，
同时提供一套**通用**的虚拟键鼠。

不是投屏，也不是 X11 转发 —— 容器里的合成器看到的就是一块真实的 DRM 卡。
虚拟键鼠走**内核 evdev**，所以任何用 libinput 的合成器都通用，不需要为每个桌面环境写适配。

**换句话说：在你自己的手机上，跑一个完整的 Linux 桌面。**

---

## 它是怎么跑起来的

```
① 显示   合成器（容器）──写池缓冲──► 内核模块 ──► App 用 AHB 重建 ──► SurfaceFlinger ──► 屏幕
② 输入   App 触控板/键盘 ──ioctl(/dev/vdrm)──► 内核模块 ──input_event()──► evdev ──► 合成器
③ 屏蔽   虚拟键鼠是内核级设备，Android 侧也会认到 → 用排除表按设备名屏蔽掉
```

池由内核模块持有，App 重启不影响；虚拟键鼠由内核模块注册，生命周期绑在模块上。

---

## 你需要准备什么

| | |
|---|---|
| **手机** | 已 root（KernelSU / Magisk / APatch），**并装了 metamodule** —— 模块的文件覆盖靠它 |
| **容器** | [Droidspaces](https://github.com/ravindu644/Droidspaces-OSS)（或任何能起 Linux 容器、能指定设备节点的方案） |
| **合成器** | 容器里装一个。KWin 和 niri 都验过；其它用 libinput 的应该也能，环境变量照抄 |

---

## 这套东西由哪几块组成

| 路径 | 是什么 | 怎么得到 |
|---|---|---|
| `module/` | **内核侧模块** `vdrm-lkm`：装 .ko、建池、把状态写进模块列表 | 装下面的 zip |
| `vdrm-ko-ddk/` | 内核模块**源码**（按 KMI 编，六档一批） | CI（DDK） |
| `vdrm-mkpool/` | **建池 helper**（用户态小程序，要 NDK 的 bionic clang） | 见其 `Makefile` |
| `vdrm-gbm/` | **GBM 后端 shim**：让合成器在虚拟卡上分配缓冲 | 见其 `Makefile` |
| `app/` | **Android 应用**：把池里的缓冲贴上屏 + 注入输入 | Gradle，CI 自动编 |

**正常情况下你只需要一个文件**：`vdrm-lkm-<version>.zip` —— 里面六个 KMI 的 `.ko`、
helper、屏蔽表都齐了。要自己出这个 zip：

```sh
python3 make-module.py          # 本地装配；或者推上去让 CI 出（artifact: vdrm-lkm-module）
```

---

## 三步跑起来

### 1. 装模块

把 `vdrm-lkm-<version>.zip` **用管理器安装**（KernelSU / Magisk 里"从本地安装"）。

> ⚠️ **别的机型要改一下屏蔽表**：模块里那份 `excluded-input-devices.xml` 是拿**本机
> ROM 的原文件**改的（在原文件上加 `vdrm-kbd` / `vdrm-ptr` 两行）。你的 ROM 里那份
> 列的设备不一样，直接套用会丢掉它自己的排除项 —— 把这两行补进**你自己**的
> `/vendor/etc/excluded-input-devices.xml`，再按这个改模块里的那份。**并且要装
> metamodule**，文件覆盖才生效。

装完**重启**。

### 2. 起容器，把两个设备交给它

容器配置里（以 Droidspaces 和Arch为例）：

**环境变量**

```
XDG_RUNTIME_DIR=/home/alarm/.xdg
LIBSEAT_BACKEND=noop
MESA_LOADER_DRIVER_OVERRIDE=msm
```

**设备挂载**

```
/dev/input        → /dev/input          # 整个目录
/dev/dri/card1    → /dev/dri/card1      # 虚拟卡
```

> 输入**挂整个目录**，别逐个列 `eventN` —— 设备编号会变，写死就会一段时间后失灵。

### 3. 在容器里装合成器

- 打上 **`mesa-for-android-container`**（lfdevs 大佬那套）—— 硬件加速靠它
- 把 **`vdrm_gbm.so` 放到容器的 `/usr/lib/gbm/`**（Arch 是这路径，别的发行版自己找）
  —— **必须**：没有它，合成器的缓冲不来自池，画面就出不来
- niri 的配置加一句：

```kdl
debug {
    render-drm-device "/dev/dri/renderD128"
}
```

（其它合成器自行探索。KWin 是用 `AQ_DRM_DEVICES=/dev/dri/card1`。）

---

## 怎么确认成功了

```sh
# 容器里 —— 应该看到一块 1920x1080@60 的输出，且 Current mode 有值（已启用）
niri msg outputs
```

- 管理器里模块那三行是 `🟢` / 有 KMI / `ready 😋`
- 按一下虚拟键盘：**在容器中打出字**

---

## 出问题先看这里

| 症状 | 多半是 | 怎么办 |
|---|---|---|
| **全黑屏**，但合成器明明在跑 | 池没建起来。合成器只分配一次缓冲，抢在池之前以后就再也匹配不上 | 重启（模块在 boot 时建池）；或关掉合成器再开一次 |
| **手机上冒出个大鼠标在滑** | 屏蔽表没生效，通常是模块被手工拷进去、丢了 SELinux 标签 | 用管理器装模块。已经拷了的话：<br>`chcon u:object_r:vendor_configs_file:s0 <模块>/system/vendor/etc/excluded-input-devices.xml` |
| 合成器**起不来 / 说没有渲染器** | `MESA_LOADER_DRIVER_OVERRIDE=kgsl` 会被部分合成器(如niri)误判成"软件渲染器"而拒绝 | 改成 `msm` |

---

## 鸣谢

- Goldzxcbug，Superturtlee，项目引路人以及参考项目的开发者

- Dere3046，提供了项目普适化的必要工具以及解决问题的思路

- lfdevs大佬，该项目想要拥有硬件加速需要打他的驱动

- anland项目，提供了初期思路以及大部分键鼠触控优化方法

- mesa-for-android-container项目，该项目硬件加速所需要的驱动来源于此

## 许可

GPL-2.0
