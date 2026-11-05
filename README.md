## 前置声明
理论上该模块仅适用于5.10-android13到6.12-android16对应KMI内遵循G(O)KI标准的内核。

如果你的内核声明已经达到该模块支持的KMI，但是内核本身并未遵循G(O)KI标准，请务必在刷入该模块前加载其中对应你内核版本的内核模块进行兼容性测试，兼容性测试的标准之一为与内核同一KMI的内核模块加载(insmod)是否导致设备重启（这一步不会伤害你的设备，因为内核模块是运行在内存中的，并未通过模块方式固化加载），如有重启，请勿刷入此模块，如没有重启，可以刷入该模块并跟着README指导进行进一步部署并进行进一步的兼容性测试（这也不会对你的设备造成永久性伤害，因为加载模块本身并不会导致设备启动失败，你可以后续在你的root管理器中卸载它）

对于KMI不在支持范围内的内核，该模块不会加载内核模块因此不会实质性生效；不遵循以上声明，由此模块引发的问题作者概不负责！

# VDRM

把一台 Android 手机变成**虚拟 DRM 显示器**（`/dev/vdrm`，虚拟 `card1`），
让 Linux 合成器（niri / KWin / Hyprland / sway …）跑在容器里、画面显示在手机屏幕上，
同时提供一套**通用**的虚拟键鼠和一张虚拟声卡。
不是投屏，也不是 X11 转发 —— 容器里的合成器看到的就是一块真实的 DRM 卡。
虚拟键鼠走**内核 evdev**，所以任何用 libinput 的合成器都通用。

**在你自己的手机上，跑一个完整的 Linux 桌面。**

默认 **1920x1080@120**；比例不合适时，在 App 的常驻通知里一键切（见
[通知里的配置面](#通知里的配置面)）。

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
vdrm-lkm-*.zip         ← 模块（六个 KMI 的 .ko + mkpool + 启动脚本）
vdrm_gbm.so            ← 容器里的 GBM shim
vdrm-debug.apk         ← Android App
```

同一周内的更新会覆盖同一个 release；跨周自动新建。

---

## 安装

### 1. 装模块

用管理器安装 `vdrm-lkm-***.zip`，然后**重启**。

> 虚拟键鼠**不用再靠屏蔽表**了：模块在加载时（`post-fs-data.sh` / `service.sh`）会把
> `/dev/input/eventN` 换成一个 0600 的、归 root 的节点，Android 的 InputReader 打不开就看不见
> （细节见 `module/common.sh` 第 3 节）。这是一次性动作，不再常驻 watcher；手动 reload 后用
> `tools/vdrm_up.sh --reload` 或在 shell 里跑一次 `vdrm_hide_input` 即可。模块里那份
> `excluded-input-devices.xml` 只当 Android ≤16 的兜底，16 以上没人读它
> （Android 17 的 `dumpsys input` 里 `ExcludedDeviceNames` 恒为 `[]`）。

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

**`vdrm_gbm.so` 的兼容性**（它不是普通链接库，是 **GBM 后端**：libgbm 看到
card1 的驱动名是 `vdrm`，就去后端目录找 `vdrm_gbm.so` 并加载它；它再把一切委托给
发行版的 `dri_gbm.so`，只把分配改走 DRM dumb 路径）

跨发行版能用，但有三个前提：

| 条件 | 说明 |
|---|---|
| **glibc ≥ 2.34** | 产物只依赖 `libc.so.6` + `ld-linux-aarch64.so.1`。Alpine 之类的 musl 发行版、Debian 11 这类老 glibc 不行（要自己重编） |
| **发行版 Mesa 带 `dri_gbm.so`** | 它靠 `dlopen("dri_gbm.so")` 转发；裁掉了 GBM 后端的 Mesa 不行 |
| **Mesa 的 GBM 后端 ABI 只做"追加式"演进** | 我们按 `GBM_BACKEND_ABI_VERSION=1` 编译，loader 取 `min(backend, core)`。Mesa 若改了布局/语义，要按它的 `gbm_backend_abi.h` 重编 |

它**不依赖** mesa-for-android-container：那套是渲染侧（card0 / EGL / msm-kgsl），
而 `vdrm_gbm.so` 只管 card1 上的 dumb buffer 分配。真出问题时它会往 stderr 打原因
（`dlopen` 失败、或 ABI 版本不一致）。

自己重编：

```sh
cd vdrm-gbm && make      # 只依赖 gcc + 发行版的 gbm.h
```

**已知合成器的最小适配**

| 合成器 | 要做的 |
|---|---|
| **niri** | 不需要任何额外配置：`XDG_RUNTIME_DIR=… LIBSEAT_BACKEND=noop MESA_LOADER_DRIVER_OVERRIDE=msm niri` 直接起（见下） |
| **KWin** | `KWIN_DRM_DEVICES=/dev/dri/card1`；`MESA_LOADER_DRIVER_OVERRIDE=kgsl` |
| **Hyprland** | `AQ_DRM_DEVICES=/dev/dri/card1`；`MESA_LOADER_DRIVER_OVERRIDE=ksgl` |

**niri 的启动命令**就是普通的 niri，容器里没有 TTY，只多两个环境变量：

```sh
mkdir -p /tmp/xdg && chmod 700 /tmp/xdg     # XDG_RUNTIME_DIR 必须存在且 0700
XDG_RUNTIME_DIR=/tmp/xdg \
LIBSEAT_BACKEND=noop \
MESA_LOADER_DRIVER_OVERRIDE=msm \
niri
```

两个变量的理由（都是实测过的，不是保险起见）：

- `LIBSEAT_BACKEND=noop`：容器里没有能激活的 seat 会话。装的 seatd 也一样 ——
  niri 会打印 `session is not active, starting libinput in paused state`，
  然后一个输出都不碰（`frames_pushed` 一直是 0）。`noop` 是让 niri 以 root
  直接开 DRM 设备。
- `MESA_LOADER_DRIVER_OVERRIDE=msm`：不设的话 mesa 加载器给这台设备挑不到
  硬件驱动，niri 会报 `failed to initialize renderer, falling back to primary
  gpu: software EGL renderers are skipped`，接着 `no allocator available`，
  不出图。

> **不需要** udev 规则，也不需要 `debug { render-drm-device … }`。
> 那是早期没有 `renderD129` 的时候，niri 会把只有 card1 的设备当成"没有渲染节点"
> 才要那么写；现在 render 节点在位，niri 自己就会选中它：
> `using as the render node: "/dev/dri/renderD128"`、`this is the primary node`。
> 只有在它真挑错 GPU 时，才用 `render-drm-device` 手动指一下。

> 由于niri对mesa后端的识别缺陷，目前只有msm后端可用，其他后端会被识别为软件渲染导致niri不工作

---

## 分辨率与 120Hz

### 默认

`.ko` 默认 **1920x1080@120**。模块的 `mode.conf` 默认也是：

```
mode=1920x1080
```

### 运行时切（不用重启）

一条命令走完 **prepare → 补槽 → commit（EDID + hotplug）**：

```sh
su -c '/data/adb/modules/vdrm-lkm/vdrm_mkpool --mode 2376x1080'
```

它等价于：

```sh
echo 2376x1080 > /sys/module/vdrm/parameters/mode   # 只 prepare，不发 hotplug
# mkpool 按目标尺寸补 6 个槽
echo 1 > /sys/module/vdrm/parameters/commit         # 真正广播
```

池由 `vdrm_mkpool --mode` 建好（`mode_slots` 是当前目标尺寸的槽数），App 会自动
重新 forge。只写 `mode` 不写 `commit` 的话合成器收不到 hotplug，会一直按旧档跑。
`cat /sys/module/vdrm/parameters/mode_slots` 应该回到 6；槽的分配占用现在会在
合成器销毁 dumb buffer 时释放，来回切不会把池耗光。

### 另外两个运行时参数：刷新率和 DPI

和 `mode` 一样的两段式（**写参数只准备，写 `commit` 才生效**）。它们只改 EDID，
不改几何，所以**不需要重建池**：

```sh
echo 90  > /sys/module/vdrm/parameters/refresh    # 24..240，默认 120（实时生效）
echo 256 > /sys/module/vdrm/parameters/edid_dpi   # 0..1000，0 = 默认 305dpi
echo 1   > /sys/module/vdrm/parameters/commit     # 一次性广播（EDID + hotplug）
```

`refresh` 决定 EDID 里的 preferred 档，合成器也仍可在报出去的几档
（`refresh` / 120 / 90 / 60）之间自己切。

`edid_dpi` 要单独说一句：它只是 EDID 里的**物理尺寸**，合成器**只在输出创建时读一次**
（niri 实测 `scale ≈ dpi/128`）。改完不重启合成器，屏幕上不会有任何变化 —— 想要
"字立刻变大变小"请用通知里的 `scale 1.5`，那条走的是合成器自己的比例。开机种子
写在 `mode.conf` 的 `refresh=` / `dpi=`。

`GET_INFO.flags` 是**能力位图**（ABI 2）：想知道当前这份 `.ko` 支不支持某个
运行时参数，看它有没有对应的位，别比版本号。App 日志里那行
`opened /dev/vdrm fd=.. abi=2 caps=0x1ff [mode,refresh,dpi,...]` 就是它。

### 开机默认档：`mode.conf`

日常切换用通知（下面那一节），`mode.conf` 只是**开机种子**；在通知里切过档之后
App 会把它写回这个文件，所以重启还是上一次的选择。

```
mode=1920x1080      # 开机默认档
#refresh=120        # 开机默认刷新率
#dpi=256            # 开机默认 EDID DPI（只在合成器启动时读一次）
```

- **1920x1080**：带黑边、避开摄像头挖孔，合成器负担最小，推荐
- **真实分辨率**（比如 `2780x1264`）：铺满，挖孔会压在桌面上；卡就切回 1920

---

## 通知里的配置面

装好之后会有一条常驻通知，它就是这个项目的**控制台** —— 容器占着整块屏，App 的
界面缩在窗口里平时碰不到，通知栏是唯一随时可达、又不用另造 IPC/权限模型的地方。

```
1920x1080@120                     ← 标题：当前档位
就绪                              ← 正文：上一条命令的结果
[全面屏]  [切横竖]  [命令]
```

**三个按钮**

| 按钮 | 作用 |
|---|---|
| `全面屏` / `黑边屏` | **一个按钮两个文本**：现在是什么档，就显示"切到另一档"。全面屏 = 真实横屏尺寸铺满；黑边屏 = 1920x1080 居中留黑边（避开摄像头挖孔）。切换会**保持当前横竖** |
| `切横竖` | 把当前档宽高对调（`1920x1080 ↔ 1080x1920`、`2780x1264 ↔ 1264x2780`） |
| `命令` | 展开输入框，输入下面这些命令 |

**命令**（输入框里的灰字就是"当前配置的可执行写法"，想改哪个数字挪过去改一下即可）

| 命令 | 作用 | 实时 |
|---|---|---|
| `input 0` / `input 1` | 手机触摸当**鼠标**（相对位移，触控板手感）/ 当**触控屏**（按哪点哪） | 下一次触摸 |
| `refresh 90` | 刷新率（24..240） | ✅ |
| `scale 1.5` | UI 缩放 —— **真正让字变大的那个**，走 `niri msg output … scale` | ✅ |
| `commit` | 只广播参数（极少用） | — |

> `scale` 和 `mode.conf` 里的 `dpi=` 是两件事：`dpi` 只改 EDID 里的物理尺寸，
> 合成器只在**启动时**读一次，运行中改毫无反应；`scale` 是合成器自己的比例，
> 改完立刻生效。

**前提**：命令要 root（走 `su`），通知要通知权限；`scale` 还需要容器里 niri 在跑
（它会自动找活着的 niri socket，重启过也不会连错）。

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

想量化地确认，看这三个（前两个是内核参数，第三个是通知）：

```sh
cat /sys/module/vdrm/parameters/mode      # 你要求的档，比如 1920x1080
cat /sys/module/vdrm/parameters/active    # 合成器【真正】编进 pipe 的尺寸，要等于 mode
cat /sys/module/vdrm/parameters/frames_pushed   # 动一下鼠标，这个数应该跟着涨
```

`mode` 对、`active` 不对 = 合成器没跟上（多半是那个尺寸 HWC 扫不出去，见"已知局限"）。
两个都对、`frames_pushed` 不涨 = 没有帧在走（合成器没出图，或 App 没在前台）。

---

## 出问题先看这里

| 情况 | 怎么做 |
|---|---|
| **全黑屏**，合成器在跑 | 重启；确认 `ls -l /dev/vdrm` 是 666 + `gpu_device`；在通知里切一次档；再看 `cat /sys/module/vdrm/parameters/active` 是否等于 `mode` |
| App 打不开 `/dev/vdrm` | `su -c 'chmod 666 /dev/vdrm; chcon u:object_r:gpu_device:s0 /dev/vdrm'`，确认模块 service.sh 跑过 |
| **手机上冒出个大鼠标 / Android 多出触摸屏、幽灵触摸** | `ls -l /dev/input/event*`：vdrm 那三个应该是 `crw------- root root`。被改回 `crw-rw---- root input` 就手动补一刀：`su -c '. /data/adb/modules/vdrm-lkm/common.sh; vdrm_hide_input'`（reload 场景用 `tools/vdrm_up.sh --reload`） |
| 合成器**起不来 / 说没有渲染器** | 按合成器设 Mesa 后端：niri / Hyprland=`msm`，KWin=`kgsl` |
| niri 报 `error opening primary GPU DRM node` | 确认容器里有 `card0`（完整硬件访问 / `enable_gpu_mode=1`，或显式 bind `/dev/dri/card0`） |
| niri 报 `primary node is missing, display-only devices may not work` | 可忽略，不影响出图 |
| niri 没起来 / 没输出 | 看日志里有没有 `software EGL renderers are skipped`（缺 `MESA_LOADER_DRIVER_OVERRIDE=msm`）或 `session is not active`（缺 `LIBSEAT_BACKEND=noop`）；`niri msg outputs` 里 `Virtual-3` 要是 `Current mode` 有值 |
| KWin / Hyprland 没输出 | `KWIN_DRM_DEVICES=/dev/dri/card1` / `AQ_DRM_DEVICES=/dev/dri/card1` |
| **120 模式但感觉只有 60** | 在通知里切到黑边屏（1920x1080） |
| **全面屏模式很卡** | 在通知里切到黑边屏（1920x1080） |
| **通知里切档没反应** | 看 `cat /sys/module/vdrm/parameters/active` 有没有跟着变；没变多半是那个尺寸 HWC 扫不出去（见"已知局限"） |
| **装 CI 的 APK 提示签名冲突** | 先卸载旧 App 再装 |
| **无线 adb 老是掉** | 这不再随模块发布。仓库 `tools/adbd_keep.sh` 里留了一份自用版本（每 20 秒把 TCP 5555 重挂一遍），需要就自己 `setsid sh tools/adbd_keep.sh` 起 |
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
# 注意构建机的 glibc 就是产物的 glibc 下限；CI 固定在 ubuntu-22.04 构建
cd vdrm-gbm && make

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
4. **某些分辨率上不了屏。** 现象是档切了、`mode`/`active` 都对，但画面不动。
   原因不在驱动：HWC 会按 gralloc 的 stride 规则重建平面布局，真实 pitch 不是
   那条规则固定点的 buffer 会 `ADDFB2` 失败 —— 而 GPU 合成（以及截图）看起来
   完全正常，所以很难查。通知里的两个预置档（1920x1080 / 真实分辨率）都避开了
   这类尺寸；自己用 `mkpool --mode` 试别的尺寸时请以 `active` 是否跟上为准
5. **帧时序是“不等 SF 上屏”换来的流畅。** 默认档位下 flip-done 在下一个 vblank
   就完成，不等 SurfaceFlinger 的 present —— 这样子 `frames_reuse` 会缓慢增长
   （合成器重提交了一块 SF 可能还在显示的显存）。实测视觉无撕裂；万一在别的
   机器上看到“局部色块”，把它当开关用：
   ```sh
   echo 1 > /sys/module/vdrm/parameters/safe_present   # 退回保守模式（慢，但绝不覆写）
   echo 0 > /sys/module/vdrm/parameters/safe_present   # 回到默认（快）
   ```
   保守模式实测会掉到十几~三十帧，它只是排障和兼容用的退路，不是常态
6. 部分依赖高级设备特性的部分功能无法使用(如多屏协同，合成器录屏等)

## 鸣谢

- Goldzxcbug、Superturtlee：项目引路与参考项目
- Dere3046：普适化工具与排查思路
- lfdevs：[mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)：硬件加速驱动
- [anland](https://github.com/SuperTurtleDev/anland)：初期思路与键鼠触控优化。
  **本项目的帧时序方案直接来自它的开发分支**（tag `6.0`…`6.2`，见
  `services/waylandbridge/awl_sc.cpp` 头部的设计注释）——"帧完成"必须从 vsync
  那一拍发出去、而不是从 SurfaceFlinger 的完成回调发，正是它先量出来的
  （我们从 30fps 变成 88fps 就是照这条改的），连"某些尺寸 HWC 扫不出去"那个
  坑也是它记下来的。上游是另一套更完整的实现，值得一并看。

## 许可

GPL-2.0
