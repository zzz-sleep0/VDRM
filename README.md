## 前置声明
理论上该模块仅适用于5.10-android13到6.12-android16对应KMI内遵循GKI/OKI标准的内核。

对于不同的内核，本模块的处理方式和Dere3046大佬的_patchHarmony项目处理方式类似，如果那个项目在你的内核上可以运行，理论上这个项目也可以

如果你的内核声明已经达到该模块支持的KMI，但内核本身并未遵循GKI/OKI标准，且没有对应的_patchHarmony部署成功先例，**先别刷.zip模块**：按文末「注意」里的方式单独 `insmod` 试一次，否则请做好音量键救砖甚至格式化data分区的准备！

对于KMI不在支持范围内的内核，该模块不会加载内核模块因此不会实质性生效

# VDRM

手机上的虚拟显示器：内核模块给出 `/dev/dri/card1`，容器里的合成器（niri / KWin /
Hyprland / sway）直接画在上面，App 负责显示、虚拟键鼠和声音。默认 1920x1080@120。

## 1. 装模块

模块管理器刷 `vdrm-lkm-*.zip`，重启手机。检查：

```sh
su -c 'lsmod | grep ^vdrm'
ls -l /dev/vdrm        # crw-rw-rw-
```

## 2. 建容器

**不要开「完整硬件访问」（`enable_hw_access`）**，把需要的设备逐个挂进来就够。实测的
Fedora 43 配置（改 `container.config`）：

```ini
enable_hw_access=0
enable_gpu_mode=1
allow_sandboxing=1
privileged=nomask
bind_mounts=/dev/dri/card1:/dev/dri/card1,/dev/input:/dev/input,/dev/snd:/dev/snd
```

**为什么不开 `enable_hw_access`**：开了之后 `/sys` 在容器里是**可写**的，容器里装或
升级驱动（Mesa 之类）会往 `/sys` 写，可能把整机软重启一次。不开的话 `/sys` 只读。

代价是容器里没有 udev 可用：`/sys` 只读，`udevadm trigger` 直接失败，于是设备节点和
`ID_INPUT*` 属性都不会有人建。解决办法是**用 systemd 的 oneshot 顶替 udev**：容器里
systemd 一定是 PID 1，让它开机跑一次脚本就行。脚本在仓库 `tools/vdrm-container-setup.sh`：

```sh
install -m 755 vdrm-container-setup.sh /usr/local/sbin/vdrm-container-setup
cat > /etc/systemd/system/vdrm-input.service <<'EOF'
[Unit]
Description=vdrm: only expose the virtual input devices
After=local-fs.target            # 要写 /dev、/run/udev/data，等挂载就绪
Before=display-manager.service   # 合成器起来前必须做完，libinput 只在启动时枚举

[Service]
Type=oneshot                     # 一次性任务，跑完就退出
RemainAfterExit=yes              # 但让 systemd 记住"已完成"（状态 active）
ExecStart=/usr/local/sbin/vdrm-container-setup

[Install]
WantedBy=multi-user.target
EOF
systemctl enable --now vdrm-input
```

**它开机干三件事（顺序不能换）**

1. `mount --make-rprivate /dev`，再用 tmpfs **顶掉** `/dev/input`、`/dev/snd`；
2. 在 tmpfs 里 `mknod` 出 vdrm 三台虚拟设备（输入 `0666`，声卡 `root:audio 0660`）
   并建 `vdrm-kbd` / `vdrm-ptr` / `vdrm-touch` 软链 —— 手机真实的触摸/电源键/指纹
   **根本不会出现在容器里**，root 跑合成器也不用再做设备排除；
3. 往 `/run/udev/data/c13:<minor>` 写属性：`ID_INPUT` + `MOUSE` / `TOUCHSCREEN` /
   `KEYBOARD`。**这一步不能省** —— libinput 走 libudev 枚举，没有这些属性会直接
   跳过设备，表现是"合成器起来了、画面正常、但完全没输入"。

> ⚠️ **不要手动在容器里 `rm` / `chmod` `/dev/input`、`/dev/snd`**：这两个目录是宿主机
> 目录的 bind mount，里面是**同一批 inode** —— 在容器里删一个节点，宿主机上就真的没了。
> （踩过一次：删掉 `/dev/input/event*` 之后 Android 直接没有触摸和按键，只能强制重启。）
> 脚本里所以先挂 tmpfs、**用我们自己的 fsname 确认挂上了**才动，任何一步不对就 `exit 1`。

**怎么确认它对**

```sh
systemctl is-active vdrm-input          # active
ls /dev/input/                          # 只有 event7/8/9 + vdrm-kbd/ptr/touch
libinput list-devices | grep -E '^Device'
#   Device: vdrm-kbd      Capabilities: keyboard
#   Device: vdrm-ptr      Capabilities: pointer
#   Device: vdrm-touch    Capabilities: touch
```

> 模块重新加载后 `eventN` 编号会变，记得 `systemctl restart vdrm-input`。

容器 `.env`：

```ini
MESA_LOADER_DRIVER_OVERRIDE=msm
LIBSEAT_BACKEND=noop
```

## 3. 容器里装三样

```sh
# Mesa（Fedora 43；Arch 换对应包）
curl -L -o /tmp/mesa.tar.gz https://github.com/lfdevs/mesa-for-android-container/releases/download/mesa-27.0.0-devel-20261025-r1/mesa-for-android-container_27.0.0-devel-20261025-r1_fedora_43_arm64.tar.gz
tar -xzf /tmp/mesa.tar.gz -C /

# GBM 后端（Fedora 是 /usr/lib64/gbm，Arch/Debian 是 /usr/lib/gbm）
cp vdrm_gbm.so /usr/lib64/gbm/

# 合成器
dnf install -y plasma-workspace kwin kscreen
```

## 4. 起桌面

```sh
install -d -m 700 /tmp/xdg
export XDG_RUNTIME_DIR=/tmp/xdg
dbus-run-session -- startplasma-wayland
```

换了合成器就换最后一行（`niri` / `Hyprland` / `sway`）。确认出图：

```sh
cat /sys/module/vdrm/parameters/frames_pushed   # 动鼠标，这个数要涨
cat /sys/module/vdrm/parameters/active
```

## 5. 装 App

装 `vdrm-debug.apk`，打开，给通知权限和 root 授权，**保持运行**。通知里两个按钮：

| | |
|---|---|
| `刷新` | 重读内核参数 |
| `命令` | `input 0` 触摸当鼠标 / `input 1` 触摸当触控屏 / `commit` 广播参数 |

## 6. 切分辨率 / 缩放

在合成器里切：

```sh
kscreen-doctor output.1.mode.2                 # KWin（-o 先看列表）
niri msg output Virtual-3 mode 1264x2780       # niri
swaymsg output Virtual-3 mode 1264x2780        # sway
```

缩放写在合成器自己的配置里（niri `output "Virtual-3" { scale 1.5 }`，
Hyprland `monitor=Virtual-3,preferred,auto,1.5`，KWin 用系统设置）。

开机默认档写在模块目录的 `mode.conf`：

```
mode=1920x1080
#refresh=120
#dpi=256
```

## 7. 声音

容器里当普通输出设备用：

```sh
aplay -l | grep vdrm
```

## 注意（如果你的内核在支持的KMI中，但是疑似较大程度上不符合GKI标准，请重点阅读这里）

- **先记一下你的/dev/input/event*下都有什么设备**，后面要用
- **先单独 `insmod` 验一遍。** 不要跳过模块自己的兼容性检验：
  用自己 KMI 的那个 `.ko` 单独 `insmod`，不重启再继续往下装。
- **设备排除。**
- 内核模块模块加载后自己把虚拟键鼠的 `/dev/input/event*`（相比于你加载内核模块之前多出来的那几个）换成 0600 root 节点，系统 InputReader 打不开就看不见。
- 容器侧不用再单独排除：按「配容器」装好那个 systemd 服务后，容器里只有 vdrm 三个虚拟设备，
  手机真实的触摸/电源键/指纹看不见（root 跑合成器也一样）。

## 鸣谢

- Goldzxcbug、Superturtlee：项目引路与参考项目
- Dere3046：普适化工具与排查思路
- lfdevs：[mesa-for-android-container](https://github.com/lfdevs/mesa-for-android-container)：硬件加速驱动
- [anland](https://github.com/SuperTurtleDev/anland)：初期思路与键鼠触控优化
