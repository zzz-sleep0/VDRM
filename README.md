## 前置声明
理论上该模块仅适用于5.10-android13到6.12-android16对应KMI内遵循G(O)KI标准的内核。

如果你的内核声明已经达到该模块支持的KMI，但是内核本身并未遵循G(O)KI标准，请务必在刷入该模块前加载其中对应你内核版本的内核模块进行兼容性测试，兼容性测试的标准之一为与内核同一KMI的内核模块加载(insmod)是否导致设备重启（这一步不会伤害你的设备，因为内核模块是运行在内存中的，并未通过模块方式固化加载），如有重启，请勿刷入此模块，如没有重启，可以刷入该模块并跟着README指导进行进一步部署并进行进一步的兼容性测试

对于KMI不在支持范围内的内核，该模块不会加载内核模块因此不会实质性生效；不遵循以上声明，由此模块引发的问题作者概不负责！

# VDRM

把一台 Android 手机变成**虚拟 DRM 显示器**（`/dev/vdrm`，虚拟 `card1`），
让 Linux 合成器（niri / KWin / Hyprland / sway …）跑在容器里、画面显示在手机屏幕上，
同时提供一套**通用**的虚拟键鼠和一张虚拟声卡。
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

用管理器安装 `vdrm-lkm-***.zip`，然后**重启**。

> ⚠️ **不同机型要补屏蔽表**：把你**自己手机** 的 `/vendor/etc/excluded-input-devices.xml` 里按照模块内置文件的格式加上 `vdrm-kbd`、`vdrm-ptr`、`vdrm-touch` 三个设备名，再用修补过的文件覆盖模块内置文件。
> **必须装 metamodule**，针对设备的排除列表覆盖才生效。三个都要：`vdrm-kbd`（键盘）、`vdrm-ptr`（指针）、
> `vdrm-touch`（触摸）。少一个就会出现大鼠标或幽灵触摸。

### 2. 装 App

装 `vdrm-debug.apk`。

- CI 的 debug APK 和你自己签名的包**签名不同**；如果手机上已经装了别的签名版本，先卸载再装。
- 模块启动时已经把 `/dev/vdrm` 设成 `0666`、标签 `gpu_device`；不需要 chown，第一次安装/重装直接可用。
- App 是横屏全屏；1920 模式居中留黑边，真实分辨率模式铺满（摄像头挖空会压在桌面上）。
- **模块和 App 要一起更新**：音频靠 App 的 `VDRM_AUDIO_KICK` 唤醒容器里的写者，新 `.ko` 配旧 APK 会周期性卡顿（10 秒一次 `-EIO`）。

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
/dev/snd          → /dev/snd            # 音频（整个目录）
```

Droidspaces 对应：

```ini
bind_mounts=/dev/input:/dev/input,/dev/dri/card1:/dev/dri/card1,/dev/snd:/dev/snd
enable_hw_access=1
privileged=nomask
```

> `/dev/snd` 里是 `vdrm` 声卡的 PCM/control 节点。容器里用非 root 用户放声时，
> 如果打不开设备，检查一下节点权限/所属组（通常是 `0660 root:audio`）。

### 4. 配合成器

- 打上 **[mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)**（lfdevs），用于获取硬件加速(可选)
- 把 **`vdrm_gbm.so` 放到容器的 `/usr/lib/gbm/`**（Arch发行版是这个路径，别的发行版需要自行寻找对应路径）

**已知合成器的最小适配**

| 合成器 | 要做的 |
|---|---|
| **niri** | 放 udev 规则（见下）+ `~/.config/niri/config.kdl` 末尾加 `debug { render-drm-device "/dev/dri/renderD128" }`；`MESA_LOADER_DRIVER_OVERRIDE=msm` |
| **KWin** | `KWIN_DRM_DEVICES=/dev/dri/card1`；`MESA_LOADER_DRIVER_OVERRIDE=kgsl` |
| **Hyprland** | `AQ_DRM_DEVICES=/dev/dri/card1`；`MESA_LOADER_DRIVER_OVERRIDE=ksgl` |

niri 的 udev 规则，存成 `/etc/udev/rules.d/99-vdrm-primary.rules`：

```
SUBSYSTEM=="drm", KERNEL=="card0", ENV{ID_SEAT}="seat0"
SUBSYSTEM=="drm", KERNEL=="card1", ENV{ID_SEAT}="seat0"
SUBSYSTEM=="drm", KERNEL=="renderD128", ENV{ID_SEAT}="seat0"
```

改完重启（或 `udevadm trigger`）。

> 由于niri对mesa后端的识别缺陷，目前只有msm后端可用，其他后端会被识别为软件渲染导致niri不工作

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

> 这里已经初见端倪了，mode.conf中的分辨率设置优先级是要高于内核模块内置分辨率的，可以试着自行调试

### 怎么选

- **1920x1080（音量上）**：带黑边、避开挖空，合成器负担小，优先选这个
- **真实分辨率（音量下）**：铺满，挖空会压在桌面上；如果卡就切回 1920

> 120 模式在部分机型上实际只呈现 60 次/秒；想要更稳就用 1920 模式。

---

## 声音

`vdrm.ko` 会同时注册一张虚拟声卡：**48 kHz / 立体声 / S16_LE**，名字是 `vdrm`。
容器里的 ALSA / PulseAudio / PipeWire 直接把它当输出用；PCM 数据经
`/dev/vdrm-audio` 的 512KB 共享环交给 App，App 用 AAudio 播到手机扬声器。

**前提：VDRM App 得开着**（它就是这个声卡的"消费端"）。
App 没开时容器里的播放器不会卡住，但数据会被丢掉（`discarded_bytes` 会涨）。

容器里确认声卡存在：

```sh
aplay -l | grep vdrm
# card 1: vdrm [vdrm], device 0: vdrm [vdrm]

# 直接播一段（不经过 PulseAudio/PipeWire）
speaker-test -D plughw:vdrm,0 -c2 -r48000 -t sine -l1
# 或者
aplay -D plughw:vdrm,0 test.wav
```

用 PulseAudio 的话，直接把 `vdrm` 选成输出即可；也可以显式加载：

```sh
pactl load-module module-alsa-sink device=hw:1,0 sink_name=vdrm
pactl set-default-sink vdrm
```

> **PipeWire 注意**：虚拟卡的 ALSA 节点在某些容器里会一直 `suspended`、不建端口，
> `pw-play` 会卡住。这不是声卡 PCM 的问题；需要 PipeWire 原生输出时请先确认
> 你的 PipeWire/WirePlumber 能正常驱动真实声卡。日常使用推荐 PulseAudio 或直接 ALSA。

**卡顿/爆音先看这里：**

```sh
cat /sys/module/vdrm/parameters/dropped_bytes     # 环满丢掉的字节
cat /sys/module/vdrm/parameters/discarded_bytes   # App 没开时丢掉的字节
cat /sys/module/vdrm/parameters/ring_used_bytes   # 当前环里积压的字节（约 192000 B/s）
```

`dropped_bytes` 持续增长说明 App 侧 AAudio 消费不过来（后台限流、采样率被系统
重采样等）。正常情况下它应该一直是 0。共享环最多压 96KB（约 0.5 秒）；
停止/关闭播放时驱动会直接清空环，不会把尾巴继续放完。

> 目前只有**播放**，没有录音（capture 还没做）。App 退到后台后声音会中断。

---

## 硬件解码（树外，可选）

VDRM 本身只提供虚拟显示（DRM）和虚拟声卡，**不包含视频硬解代码**，CI 也不会构建它。
浏览器硬解和 Mesa 一样属于容器侧的树外优化，按需单独安装：

- 项目：[`droidspaces-media-decode`](https://github.com/Yizhou147/droidspaces-media-decode)
- 它把高通 Venus 的 `/dev/video32` 通过标准 VA-API 暴露给容器里的
  ffmpeg / Firefox / Chrome，不需要 Android 侧 daemon
- 容器需要透传 `/dev/video32`、`/dev/dma_heap/system`（或 `/dev/ion`）、
  `/dev/dri/renderD128`；编译安装和浏览器参数以该项目 README 为准

实测边界（仅作为提示，不是 VDRM 的构建依赖）：

- AV1 1080p 在 Firefox + VDRM 上可流畅硬解；
- H.264/HEVC 带 B 帧的流在部分 Firefox 版本上会被浏览器自身的
  送帧节奏限制（驱动解码和 `vaSyncSurface` 均正常），表现为 ~19fps 卡顿；
  遇到时优先让站点使用 AV1，或换用该项目验证过的浏览器版本。

---

## 怎么确认成功了

```sh
# 容器里
niri msg outputs     # 1920x1080@120（或你选的模式），Current mode 有值
aplay -l             # 能看到 card ...: vdrm
```

- 管理器里模块状态是 `🟢` / 有 KMI / `ready 😋`
- 按一下虚拟键盘：**在容器中打出字**
- `ls -l /dev/vdrm` 是 `crw-rw-rw-`、标签 `gpu_device`
- 容器里放声音，**手机扬声器出声**

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
| **全面屏模式很卡** | 用 Action 选音量上（1920 带黑边），重启 |
| **Action 按音量键没反应** | 直接编辑 `mode.conf`，或换物理音量键再试 |
| **装 CI 的 APK 提示签名冲突** | 先卸载旧 App 再装 |
| **没声音**，但 `aplay` 不报错 | 确认 App 开着；`cat /sys/module/vdrm/parameters/dropped_bytes` 是否在涨；`ls -l /dev/snd` |
| 容器里 `aplay -l` 看不到 `vdrm` | 容器没绑 `/dev/snd`（见"配容器"）；宿主机 `lsmod \| grep vdrm` 要有 |
| 播放卡顿/爆音 | 看 `dropped_bytes`；App 被后台限流时也会掉；先试 1920 模式降负载 |
| `insmod` 报 `Unknown symbol snd_*` | 内核没开 ALSA 或 `snd` 没加载；`dmesg` 看具体符号 |
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

## 已知局限

1. 音频只有播放，没有录音；App 必须开着才有声音
2. PipeWire 原生 ALSA 输出在部分容器里无法激活（节点 `suspended`），日常建议用
   PulseAudio；这不是 VDRM 的 PCM 问题，需要时可单独排查容器音频栈
3. 硬件解码是树外内容，不在本仓库/CI 中；H.264/HEVC B 帧在部分 Firefox 版本上
   会受浏览器送帧节奏影响，详见上文“硬件解码（树外，可选）”
4. 部分依赖高级设备特性的部分功能无法使用(如多屏协同，合成器录屏等)

## 鸣谢

- Goldzxcbug、Superturtlee：项目引路与参考项目
- Dere3046：普适化工具与排查思路
- lfdevs：[mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)：硬件加速驱动
- anland：初期思路与键鼠触控优化

## 许可

GPL-2.0
