## 前置声明
理论上该模块仅适用于5.10-android13到6.12-android16对应KMI内遵循GKI/OKI标准的内核。

对于不同的内核，本模块的处理方式和Dere3046大佬的_patchHarmony项目处理方式类似，如果那个项目在你的内核上可以运行，理论上这个项目也可以

如果你的内核声明已经达到该模块支持的KMI，但内核本身并未遵循GKI/OKI标准，且没有对应的_patchHarmony部署成功先例，**先别刷.zip模块**：按文末「注意」里的方式单独 `insmod` 试一次，否则请做好音量键救砖甚至格式化data分区的准备！

对于KMI不在支持范围内的内核，该模块不会加载内核模块因此不会实质性生效

# VDRM

手机上的虚拟显示器：内核模块给出 `/dev/dri/card1`，容器里的合成器（niri / KWin /
Hyprland / sway）直接画在上面，App 负责显示、虚拟键鼠和声音。默认 1920x1080，
EDID 报 120/90/60 三档，**App 不主动投票刷新率**，由合成器/SF 自己选（见第 9 节）。

## 1. 装模块

刷 `vdrm-lkm-*.zip`，重启手机。（内核不符合 GKI 标准的话，先做文末「兼容性自测」，
别直接刷 zip。）

```sh
su -c 'lsmod | grep ^vdrm'
ls -l /dev/vdrm                    # crw-rw-rw-
ls -l /sys/class/tty/tty0/active   # VT-less 内核上由模块提供
grep vdrm-vt /proc/tty/drivers     # vdrm-vt /dev/tty 4 0-6 console
```

模块在 `CONFIG_VT=n` 的内核上注册 major 4 的虚拟 VT（`/dev/tty0..6`）；
内核自带 VT 时 major 4 已被占，模块自动跳过，用原生的。`/sys/module/vdrm/parameters/vt`
可以关掉它（0）。

### 1.1 设备节点是谁建的（为什么模块里有一小段用户态）

先把边界说清楚：**图形栈本身零 patch** —— KMS/DRM、GBM、EGL/Vulkan、Wayland、
KWin、libinput、logind 全是原生实现，内核侧没有一行图形相关改动，合成器那边也没有。

真正需要用户态插手的只有设备节点，原因是内核事实，不是我们的选择：

- GKI 上 `CONFIG_DEVTMPFS` 是 **n**，内核根本不会建节点；`/dev` 是 init 挂的
  tmpfs，节点由 Android 的 **ueventd** 在收到 `KOBJ_ADD` 时 `mknod` 出来。
  所以节点的名字和主次号本身是原生的，来自内核 uevent。
- 但 ueventd 只在 ADD 时动作，`KOBJ_REMOVE` **不删节点**：`rmmod` 之后旧节点
  照旧留在 `/dev` 里，重新 `insmod` 拿到的主次号一变，它们就是指向死设备的空壳。
- 权限和 SELinux 标签：`/vendor/etc/ueventd.rc` 里没有 vdrm 的条目，而它在 init
  第二阶段就读完了，早于模块挂载 —— 模块改不动它。

于是 `module/common.sh` 里有一段 `vdrm_sync_nodes`，只做 ueventd 做不到的三件事，
全部以 sysfs 的 `dev` 为准：

| 设备 | 节点 | 模式 / owner | 标签 |
|---|---|---|---|
| 控制面 | `/dev/vdrm`、`/dev/vdrm-audio`、`/dev/vdrm-audio-in` | `0666 root:root` | `gpu_device`（App 能 open） |
| DRM | `/dev/dri/cardN`（realpath 含 `/platform/vdrm`） | `0666 root:graphics` | `graphics_device` |
| VT | `/dev/tty0..6` | `0620 root:tty`（原生 VT 惯例） | `tty_device` |
| 虚拟输入 | `/dev/input/eventN`（name 匹配 `vdrm-*`） | `0600 root:root` | `input_device` |
| 虚拟声卡 | `/dev/snd/*`（realpath 含 `/platform/vdrm`） | `0660 system:audio` | `audio_device` |

主次号对不上就删了重建；虚拟输入必须"删了重建"才会触发 EventHub 的
`IN_DELETE`/`IN_CREATE`，只 `chmod` 收不回已经打开的 fd。只认上面这几类 vdrm
设备，不对 `event*` 之类做任何通配。

这段脚本的作用等价于原生驱动的 `ueventd.rc` + udev 规则，不是什么内核或合成器补丁。

## 2. 建容器

改 `container.config`（实测的 Fedora 43 配置）：

```ini
enable_hw_access=0
enable_gpu_mode=1
allow_sandboxing=1
privileged=nomask
bind_mounts=/dev/tty0:/dev/tty0,/dev/tty1:/dev/tty1,/dev/tty2:/dev/tty2,/dev/tty3:/dev/tty3,/dev/tty4:/dev/tty4,/dev/tty5:/dev/tty5,/dev/tty6:/dev/tty6,/dev/dri/card1:/dev/dri/card1,/dev/input/event7:/dev/input/event7,/dev/input/event8:/dev/input/event8,/dev/input/event9:/dev/input/event9,/dev/snd/controlC0:/dev/snd/controlC0,/dev/snd/pcmC0D0p:/dev/snd/pcmC0D0p,/dev/snd/pcmC0D1c:/dev/snd/pcmC0D1c
```

`/dev/input`、`/dev/snd` 也可以整目录 bind，但逐个节点更干净：Android 自己的
event 节点和另一张声卡就不会跟着进容器。

**event 号要对上 `vdrm-kbd` / `vdrm-ptr` / `vdrm-touch` 这三个名字**，用
`for e in /sys/class/input/event*/device/name; do echo "$e: $(cat $e)"; done` 查。
在实测机上是 `event7/8/9`；别把手机自己的设备 bind 进去（`event10` 是耳机 jack、
`event11` 是按键 jack、`event12` 是 `uinput_nav`，它们没有 udev 数据、不在 seat 里，
KWin 根本不会用），那样容器里就一个可用输入都没有。

ALSA 节点名也是大小写敏感的：`pcmC0D0p`（小写 p = playback），写成 `pcmC0D0P`
会 bind 出一个没人认的节点。

**`/dev/tty0..6` 是硬前提，少一个都不行**：logind 判断"这台机器有没有 VT"
的唯一依据就是 `/dev/tty0` 存不存在（`access("/dev/tty0")`），`CanTTY` 直接由它
决定；SDDM 的 sddm-helper 则要打开 `/dev/tty1` 做 `TIOCSCTTY`。缺了就是
`Inappropriate ioctl for device`，session 开完 40ms 就关。

运行时会把 `tty1..6` 预置成 `/dev/null` 或空节点的，必须确认它们被换成了真的
`4:N`。droidspaces 7.1.0 用指向 `/dev/null` 的软链占位，而且拒绝在软链上 bind
（`Security Violation: Bind target is a symlink`）；7.2.0 改成 `1:3` 字符节点占位，
bind 就能覆盖上去。


改 `.env`：

```ini
MESA_LOADER_DRIVER_OVERRIDE=msm
LIBSEAT_BACKEND=noop
```

`LIBSEAT_BACKEND=noop` 只对 niri / Hyprland 有意义；KWin 6.7 不链 libseat，
它走 logind（见第 5 节），不需要这一行。

`enable_hw_access` 保持 0：开了之后容器里 `/sys` 可写，装驱动可能把整机软重启。
代价是容器里没有 udev，所以下一步用 systemd oneshot 顶替。

## 3. 容器里写 udev 数据

设备节点由容器运行时逐个 bind 进容器：

```text
/dev/tty0..6       ← 宿主 vdrm VT（logind 的 CanTTY / sddm-helper 的 TIOCSCTTY）
/dev/dri/card1     ← 宿主虚拟 DRM
/dev/input/eventN  ← 宿主 vdrm-kbd/ptr/touch
/dev/snd/*         ← 宿主 vdrm 声卡
```

systemd 只负责 udev 的职责：运行 `vdrm-container-setup`，把
`/run/udev/data`、`/run/udev/tags` 写好，让 logind/合成器能激活这些设备。
它**不 mknod、不 mount、不 mask、不碰 `/dev`**。

```sh
install -m 755 tools/vdrm-container-setup.sh /usr/local/sbin/vdrm-container-setup
install -m 644 docs/container/systemd/vdrm-udev.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now vdrm-udev
```

```sh
systemctl is-active vdrm-udev
cat /run/udev/data/c226:1              # DRM seat/master-of-seat
cat /run/udev/data/c13:71              # vdrm-kbd 的 ID_INPUT*/Q:seat
```

## 4. Mesa 与 GBM

- **Mesa**：用 [lfdevs/mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)
  出的包（按你的发行版/架构选）；
- **GBM 后端**：把仓库里的 `vdrm_gbm.so` 放进发行版的 GBM 后端目录
  （Fedora `/usr/lib64/gbm/`，Debian/Arch `/usr/lib/gbm/`）。

其余按发行版的常规方式装合成器即可，不赘述。

## 5. 起桌面：交给 SDDM

有了 `/dev/tty0..6` 之后，桌面直接交给 SDDM 自动登录拉起即可 —— 不需要自己写
service 去起合成器。现成文件在 [`docs/container/sddm/`](docs/container/sddm/README.md)，
装完 `systemctl enable sddm` 就行。

三个要点：

- **SDDM 以 root 跑**（`sddm.service` 里没有 `User=`），不用额外加组；
- **会话包装脚本必须 `trap '' HUP`**。实测把这行去掉后：SDDM 日志立刻出现
  `session closed for user root` + `Auth: sddm-helper exited with 1`，
  `loginctl` 里没有 session、`kwin_wayland` 起不来；
- **环境变量要给两处**：`/etc/environment`（PAM/session）和
  `/etc/environment.d/`（`systemd --user`），否则 `plasma-kwin_wayland.service`
  拿不到 `KWIN_DRM_DEVICES` / `MESA_LOADER_DRIVER_OVERRIDE`。

确认：

```sh
loginctl list-sessions                    # 应有一条 SEAT=seat0、TTY=tty1
pgrep -c -x kwin_wayland                  # 1
cat /sys/module/vdrm/parameters/frames_pushed   # 动鼠标要涨
```

**关于 VT 的边界**：这套虚拟 VT 还不成熟，只实现了 logind/SDDM 需要的那部分
uAPI。它能做到的是"SDDM 在 tty1 上拉起一个桌面"；想同时装多个合成器、在 SDDM
里按需切换 niri / sway / Hyprland，会遇到会话和输入的归属问题 —— **别把它当真机
上的多会话 VT 用**。换合成器建议直接改 SDDM 会话，而不是并存多套。

## 6. 装 App

装 `vdrm-debug.apk`，打开，给通知权限和 root 授权，保持运行。

| 通知按钮 | 作用 |
|---|---|
| `鼠标` | 手机触摸当鼠标（相对位移） |
| `触控` | 手机触摸当触控屏（按哪点哪） |
| `刷新` | 重读内核参数 |

**长按桌面图标 → 「调试选项」**（`VdrmSettingsActivity`）：

| 项 | 默认 | 调大 | 调小 |
|---|---|---|---|
| 在飞帧数（in-flight，1..6） | 4 | 画面更平滑，但合成器领先屏幕更多帧、延迟变大 | 更跟手、延迟更小，但容易被上屏速度限住（1 会明显卡顿） |
| 锁相环（pll） | 关 | —— | —— |

锁相环：把内核假 vblank 的相位锁到面板 vsync，**只在两者同频时有用**，不同频请保持关闭。

## 7. 切分辨率

```sh
kscreen-doctor output.1.mode.2                 # KWin（-o 先看列表）
niri msg output Virtual-3 mode 1264x2780       # niri
swaymsg output Virtual-3 mode 1264x2780        # sway
```

开机默认档在模块目录 `mode.conf`：

```
mode=1920x1080
#refresh=120
#dpi=256
```

`refresh` 只决定 EDID 里报的 preferred 档（120/90/60 都会报出去），合成器自己选；
App 不投票。同频不闪、90 那档容易撞（见第 9 节）。

## 8. 声音

```sh
aplay -l | grep vdrm
```

容器里的声卡是**真实的内核 ALSA 设备**（`/dev/snd/controlC0`、`pcmC0D0p`、
`pcmC0D1c`），行为和真机一致 —— 按你所装发行版的常规方式配置音频即可
（PipeWire / PulseAudio / 直接 ALSA 都行），没有任何 VDRM 专用步骤。

声音是**按需**的：容器里有人打开 PCM，内核把 `play_users` 置 1 并叫醒 App，
App 这时才起 AAudio；PCM 关掉之后 App 再停。所以容器里要有一个声卡服务真的
会开、也会关这个 PCM。

> Fedora 上的两个坑（`ConditionUser=!root`、WirePlumber 枚举不到 ALSA 卡）
> 和现成配置在 [`docs/container/audio/`](docs/container/audio/README.md)。

裸 ALSA 也能直接测：

```sh
aplay -D hw:0,0 -f S16_LE -r 48000 -c 2 /dev/zero
cat /sys/module/vdrm/parameters/play_users     # 播放期间应为 1
```

`play_users` 一直是 0 = 容器根本没开 PCM（或旧内核模块的那个约束返回值 bug），
这时 App 不会起 AAudio，dmesg 里也不会有 `vdrm_audio` 相关日志。

## 9. 延迟与闪屏

**默认就是最优组合**：`pace=0`（flip-done 在假 vblank tick 上无条件发）、
在飞帧数 4、锁相环关、不投刷新率。没有特别需要一般不用改。

**看延迟**：

```sh
logcat -s VDRM | grep STATS
```

| 字段 | 含义 |
|---|---|
| `push2read` | 内核推帧 → App 读到 |
| `read2apply` | 读到 → 提交进 SF |
| `apply2latch` | SF 把它锁进本帧 |
| `latch2present` | SF 合成 + 扫描上屏 |
| `present2ret` | 上屏 → App 归还缓冲区 |

前两段是 App 自己的（正常 0.1ms 级）；后三段是 SurfaceFlinger 的，通常
13–24ms，这部分改不动。

**闪屏**：面板和合成器同频（都 120 或都 60）不闪；不同频会闪，`90` 那档最容易
和 120 的 tick 撞。先把刷新率调成同频；同频还闪就把「在飞帧数」调大。

**卡顿**：把「在飞帧数」调大（更平滑，延迟变大）；调小更跟手，但太小会卡。

## 注意：KMI 对得上，也可能不兼容

`uname -r` 在支持的 KMI 里、模块里也有对应的 `.ko`，不代表一定能加载：厂商魔改过的
内核可能 `insmod` 报 `Unknown symbol`、`probe` 失败甚至当场 panic。**先别刷 zip**，
按下面六步单独验一遍，哪一步不对就 `rmmod` 收手。

### 1. 记下现状

```sh
uname -r                                  # 决定的 KMI，比如 6.1.x-android14-...
for p in /sys/class/input/event*/device/name; do echo "$p: $(cat $p)"; done
```

把 `eventN` 列表抄下来——模块加载后会多出 `vdrm-kbd` / `vdrm-ptr` / `vdrm-touch` 三个，随后的/dev/input/event*的设备号也会多处三个，跟着下面**排除设备**处理。

### 2. 只解出 .ko，单独 insmod

```sh
mkdir -p /data/local/tmp/vdrm && cd /data/local/tmp/vdrm
unzip /sdcard/vdrm-lkm-v3.*.zip 'vdrm-*.ko'      # 只取 .ko，别的先别装
su -c 'insmod /data/local/tmp/vdrm/vdrm-android14-6.1.ko'
```

### 3. 看 dmesg 走到哪一步

```sh
su -c 'dmesg | grep vdrm' | tail -30
```

正常应该依次出现（缺哪个，就是卡在哪一步）：

```
vdrm: registered
vdrm: /dev/vdrm registered (minor=..)
vdrm: stage=register-ok
input: vdrm-kbd as /devices/virtual/input/inputNN
input: vdrm-ptr ...
input: vdrm-touch ...
vdrm: stage=input-ok
vdrm: audio ready (/dev/vdrm-audio out 48000 Hz 2 ch ring 524288, ...)
vdrm: bring-up ok (dev=...)
```

`insmod` 直接 panic / 手机重启 = 这个内核不兼容，**到此为止**。

### 4. 检查节点

```sh
ls -l /dev/vdrm /dev/vdrm-audio /dev/vdrm-audio-in     # 三个都在
ls -l /dev/dri/card1                                   # 226,1
cat /proc/asound/cards | grep vdrm                     # 有一张 vdrm 声卡
```

节点权限在手工 `insmod` 时是默认值，验证阶段先给开：

```sh
su -c 'chmod 666 /dev/vdrm /dev/vdrm-audio /dev/vdrm-audio-in'
su -c 'chcon u:object_r:gpu_device:s0 /dev/vdrm'
```
**设备排除**

```sh
# 只对 vdrm 三个设备做；绝不能用 /dev/input/event* 通配，
# 否则会把手机真实触摸/电源键/指纹一起 root:root。
for p in /sys/class/input/event*/device/name; do
    case "$(cat "$p" 2>/dev/null)" in vdrm-*) ;; *) continue ;; esac
    ev=$(basename "$(dirname "$(dirname "$p")")")
    chown root:root "/dev/input/$ev"
    chmod 0600 "/dev/input/$ev"
done
```

### 5. 用 App 打通一条帧

装 APK、打开、授权，然后：

```sh
su -c 'dmesg | grep -E "vdrm: (app eventfd|REGISTER)" | tail -8'
```

应该看到 `app eventfd registered` 和 **6 条** `REGISTER id=1..6 1920x1080`。

如果没有问题，而且急于想要看到效果，可以按照上面说的打开一个合成器看看有没有画面。

### 6. 通过之后再刷 zip

在模块管理器刷 `vdrm-lkm-*.zip`，重启。之后按第 2 步往后正常部署。



### 容器侧不用再排除

模块加载后 `vdrm_sync_nodes` 只按 `vdrm-*` 名字把对应 event 节点换成
`0600 root:root`（并顺手校正主次号/标签）；**不要对 `/dev/input/event*` 通配**。
容器侧设备节点由运行时 bind，不再有 systemd mknod/service。

重载模块**不需要重启手机**：用 `tools/vdrm_reload.sh`（先停容器和 App → `rmmod`
→ `insmod` → 校正节点并隐藏输入 → 重建池 → 起容器和 App）。它的顺序保证
InputReader 手里没有旧 fd；开机时 post-fs-data 也会在 zygote 之前完成隐藏。

验证：

```sh
su -c 'dumpsys input | grep -E "vdrm-kbd|vdrm-ptr|vdrm-touch"'   # 应该为空
```

## 鸣谢

- Goldzxcbug、Superturtlee：项目引路与参考项目
- Dere3046：普适化工具与排查思路
- lfdevs：[mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)：硬件加速驱动
- [anland](https://github.com/SuperTurtleDev/anland)：初期思路与键鼠触控优化
