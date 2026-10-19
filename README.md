# 前置声明！！理论上该模块仅适用于遵循GKI(OKI)标准的内核！！如果你的厂商对内核有魔改行为，或者你刷入了深度优化魔改的内核，请勿在没有音量键救砖的情况下刷入此模块，不遵循以上声明，由此模块引发的问题作者概不负责！

# VDRM

把一台 Android 手机变成**虚拟 DRM 显示器**（`/dev/vdrm`，虚拟 `card1`），
让 Linux 合成器（niri / KWin / Hyprland / sway …）跑在容器里、画面显示在手机屏幕上，
同时提供一套**通用**的虚拟键鼠。

不是投屏，也不是 X11 转发 —— 容器里的合成器看到的就是一块真实的 DRM 卡。
虚拟键鼠走**内核 evdev**，所以任何用 libinput 的合成器都通用。

**在你自己的手机上，跑一个完整的 Linux 桌面。**

默认 **1920x1080@120**；比例不合适时，用模块的 Action 切到真实分辨率。

---

## 你需要准备什么

| | |
|---|---|
| **手机** | 已 root（KernelSU / Magisk / APatch），**并装了 metamodule** |
| **容器** | [Droidspaces](https://github.com/ravindu644/Droidspaces-OSS)（或任何能起 Linux 容器、能指定设备节点的方案） |
| **合成器** | 容器里装一个。niri / KWin / Hyprland 都验过 |

---

## 拿到三个文件

去 GitHub Releases 下最新的 `weekly-YYYY-Www`：

```
vdrm-lkm-v2.0.zip      ← 模块（六个 KMI 的 .ko + mkpool + Action 脚本）
vdrm_gbm.so            ← 容器里的 GBM shim
vdrm-debug.apk         ← Android App
```

同一周内的更新会覆盖同一个 release；跨周自动新建。

---

## 安装

### 1. 装模块

用管理器安装 `vdrm-lkm-v2.0.zip`，然后**重启**。

> ⚠️ **别的机型要补屏蔽表**：把你**自己 ROM** 的 `/vendor/etc/excluded-input-devices.xml`
> 里加上 `vdrm-kbd`、`vdrm-ptr`、`vdrm-touch` 三个设备名，再按这个改模块里的那份。
> **必须装 metamodule**，文件覆盖才生效。三个都要：`vdrm-kbd`（键盘）、`vdrm-ptr`（指针）、
> `vdrm-touch`（触摸）。少一个就会出现大鼠标或幽灵触摸。

### 2. 装 App

装 `vdrm-debug.apk`。

- CI 的 debug APK 和你自己签名的包**签名不同**；如果手机上已经装了别的签名版本，先卸载再装。
- 模块启动时已经把 `/dev/vdrm` 设成 `0666`、标签 `gpu_device`；不需要 chown，第一次安装/重装直接可用。
- App 是横屏全屏；1920 模式居中留黑边，真实分辨率模式铺满（摄像头挖空会压在桌面上）。

### 3. 配容器

以 Droidspaces + Arch 为例：

**环境变量**

```
XDG_RUNTIME_DIR=/tmp/xdg
LIBSEAT_BACKEND=noop
```

> `MESA_LOADER_DRIVER_OVERRIDE` 跟着合成器走，见下面的合成器表。

**设备**

```
/dev/input        → /dev/input          # 整个目录
/dev/dri/card1    → /dev/dri/card1      # 只挂虚拟卡
```

Droidspaces 对应：

```ini
bind_mounts=/dev/input:/dev/input,/dev/dri/card1:/dev/dri/card1
enable_hw_access=1
privileged=nomask
```

> 输入挂整个目录，别逐个列 `eventN`。开了完整硬件访问后 `card0` / `renderD128` 已经在容器里，
> **不要**再整目录挂 `/dev/dri`。

### 4. 配合成器

- 打上 **[mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)**（lfdevs）
- 把 **`vdrm_gbm.so` 放到容器的 `/usr/lib/gbm/`**（Arch 是这路径，别的发行版自己找）

**已知合成器的最小适配**

| 合成器 | 要做的 |
|---|---|
| **niri** | 放 udev 规则（见下）+ `~/.config/niri/config.kdl` 末尾加 `debug { render-drm-device "/dev/dri/renderD128" }`；`MESA_LOADER_DRIVER_OVERRIDE=msm` |
| **KWin** | `KWIN_DRM_DEVICES=/dev/dri/card1`；`MESA_LOADER_DRIVER_OVERRIDE=kgsl` |
| **Hyprland** | `AQ_DRM_DEVICES=/dev/dri/card1`；`MESA_LOADER_DRIVER_OVERRIDE=msm` |

niri 的 udev 规则，存成 `/etc/udev/rules.d/99-vdrm-primary.rules`：

```
SUBSYSTEM=="drm", KERNEL=="card0", ENV{ID_SEAT}="seat0"
SUBSYSTEM=="drm", KERNEL=="card1", ENV{ID_SEAT}="seat0"
SUBSYSTEM=="drm", KERNEL=="renderD128", ENV{ID_SEAT}="seat0"
```

改完重启（或 `udevadm trigger`）。

---

## 分辨率与 120Hz

### 默认

`.ko` 默认 **1920x1080@120**。模块的 `mode.conf` 默认也是：

```
mode=1920x1080
```

### 用 Action 切

模块管理器里点 **Action**，5 秒内：

| 按键 | 结果 |
|---|---|
| **音量上** | 按真实分辨率取最大 **16:9**（例如 1920x1080），居中留黑边 |
| **音量下** | 真实横屏分辨率（例如 2376x1080），铺满，摄像头挖空压在桌面上 |
| **不按** | 保持当前配置 |

结果写入 `mode.conf`，**重启后生效**。

也可以直接改 `/data/adb/modules/vdrm-lkm/mode.conf`：

```
mode=1920x1080
# mode=2376x1080
# mode=auto
```

### 怎么选

- **1920x1080（音量上）**：带黑边、避开挖空，合成器负担小，优先选这个
- **真实分辨率（音量下）**：铺满，挖空会压在桌面上；如果卡就切回 1920

> 120 模式在部分机型上实际只呈现 60 次/秒；想要更稳就用 1920 模式。

---

## 怎么确认成功了

```sh
# 容器里
niri msg outputs     # 1920x1080@120（或你选的模式），Current mode 有值
```

- 管理器里模块状态是 `🟢` / 有 KMI / `ready 😋`
- 按一下虚拟键盘：**在容器中打出字**
- `ls -l /dev/vdrm` 是 `crw-rw-rw-`、标签 `gpu_device`

---

## 出问题先看这里

| 情况 | 怎么做 |
|---|---|
| **全黑屏**，合成器在跑 | 重启；确认 `ls -l /dev/vdrm` 是 666 + `gpu_device`；`cat /sys/module/vdrm/parameters/flip_done_immediate` 是 N；用 Action 重选分辨率并重启 |
| App 打不开 `/dev/vdrm` | `su -c 'chmod 666 /dev/vdrm; chcon u:object_r:gpu_device:s0 /dev/vdrm'`，确认模块 service.sh 跑过 |
| **手机上冒出个大鼠标** | 用管理器装模块；给模块里的 `excluded-input-devices.xml` 打 `vendor_configs_file` 标签；确认装了 metamodule |
| **Android 多出触摸屏/幽灵触摸** | 在 `/vendor/etc/excluded-input-devices.xml` 里补 `vdrm-touch`（三个设备一起），装 metamodule 后重启 |
| 合成器**起不来 / 说没有渲染器** | 按合成器设 Mesa 后端：niri / Hyprland=`msm`，KWin=`kgsl` |
| niri 报 `error opening primary GPU DRM node` | 确认容器里有 `card0`（完整硬件访问 / `enable_gpu_mode=1`，或显式 bind `/dev/dri/card0`） |
| niri 报 `primary node is missing, display-only devices may not work` | 可忽略，不影响出图 |
| niri 没起来 / 没输出 | 检查 `/etc/udev/rules.d/99-vdrm-primary.rules` 三行、`render-drm-device`；重启或 `udevadm trigger` |
| KWin / Hyprland 没输出 | `KWIN_DRM_DEVICES=/dev/dri/card1` / `AQ_DRM_DEVICES=/dev/dri/card1` |
| **120 模式但感觉只有 60** | 用 Action 选 1920 模式 |
| **2376 模式很卡** | 用 Action 选音量上（1920 带黑边），重启 |
| **Action 按音量键没反应** | 直接编辑 `mode.conf`，或换物理音量键再试 |
| **装 CI 的 APK 提示签名冲突** | 先卸载旧 App 再装 |
| niri 日志 `got vblank after 0ns` | 可忽略，不影响出图 |

---

## 自己构建（可选）

```sh
# 模块 zip（需要六档 .ko 和 mkpool）
python3 make-module.py

# mkpool（NDK bionic clang）
cd vdrm-mkpool && make CC=/data/data/com.termux/files/usr/bin/aarch64-linux-android-clang

# vdrm_gbm.so（容器 aarch64 glibc，只依赖 libc）
cd vdrm-gbm && gcc -shared -fPIC -O2 -Wall -o vdrm_gbm.so vdrm_gbm.c

# APK
gradle assembleDebug
```

推上去后 CI 会自动出 `weekly-YYYY-Www`，资产就是上面那三个文件。

---

## 鸣谢

- Goldzxcbug、Superturtlee：项目引路与参考项目
- Dere3046：普适化工具与排查思路
- lfdevs：[mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)：硬件加速驱动
- anland：初期思路与键鼠触控优化

## 许可

GPL-2.0
