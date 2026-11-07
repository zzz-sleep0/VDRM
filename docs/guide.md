# VDRM 使用指南（深水区三章）

> 这三章是教程里最容易被追问、也只有本项目才有的内容：**配置面怎么用**、
> **哪些事它做不到**、**为什么以前只有 30fps**。
> 其余章节（安装、配容器、起合成器、排障）现有 `README.md` 已经够用。
>
> 本文里的数字都是本机（SM8650 / android14-6.1 / 120Hz 面板）实测，
> 换机器会变，但**量级和判断方式**不变。

---

## 7. 配置面：通知栏那一条

### 为什么是通知

容器占着整块屏，App 自己的界面缩成窗口里的一小块，平时根本碰不到。
而通知栏是唯一**随时可达**、又不用我们另造一套东西的地方：

- 不新开 socket / 不加 `/dev` 节点 → 不用写 SELinux 策略；
- 权限边界就是"拿着手机的人" → 别的应用改不了你的配置；
- 前台服务本来就要发通知 → 边际成本为零。

之前试过两种方案（常驻 watcher 脚本、ctl 设备节点），都删了 —— 它们只是在
重新发明通知栏。

### 长什么样

```
1920x1080@120                     ← 标题：当前档位
就绪                              ← 正文：上一条命令的结果
[全面屏]  [切横竖]  [命令]
```

点正文 = 打开 App（这是保底入口：个别 OEM 会把 inline reply 的输入框藏掉，
这时"命令"按钮看起来没反应，至少还能从正文进来）。

### 三个按钮

| 按钮 | 行为 |
|---|---|
| `全面屏` / `黑边屏` | **一个按钮两个文本**：现在是什么档，就显示"切到另一档"。切换**保持当前横竖** |
| `切横竖` | 把当前档宽高对调（`1920x1080 ↔ 1080x1920`、`2780x1264 ↔ 1264x2780`） |
| `命令` | 展开输入框 |

两个预置档不是写死的两个数：

- **黑边屏 = 1920x1080**（居中留黑边，避开摄像头挖孔）。这一档固定，因为它的竖版
  `1080x1920` 是确定能上屏的；按"能放下的最大 16:9"去算会得到 2247x1264，而它的
  竖版 `1264x2247` 恰好是 HWC 扫不出去的尺寸（见第 10 章）。
- **全面屏 = `wm size` 报的真实分辨率**（Override 优先，竖屏自动转横屏）。换台机器
  自动跟着变。

### 命令

输入框里的灰字就是**当前配置的可执行写法**，想改哪个数字挪过去改一下即可：

```
input 1 · refresh 120
```

| 命令 | 背后做什么 | 生效时机 |
|---|---|---|
| `input 0` / `input 1` | 手机触摸注入到哪个虚拟设备：`vdrm-ptr`（REL 相对位移，触控板手感）/ `vdrm-touch`（ABS_MT 绝对坐标，按哪点哪） | 下一次触摸 |
| `refresh 90` | 写 `refresh` 参数 + `commit`（重算 EDID + hotplug） | 立刻 |
| `commit` | 只广播参数，不重建池 | 立刻 |

两个容易混的东西：

- **缩放不在通知里**：`wl_output.scale` 是合成器自己的状态（还牵着字体、光标、
  XWayland 缩放），设备侧表示不了。在合成器配置里写一次：niri `output "Virtual-3" { scale 1.5 }`、
  sway `output Virtual-3 scale 1.5`、Hyprland `monitor=Virtual-3,preferred,auto,1.5`、
  KWin 用系统设置/`kscreen-doctor`。
- `mode.conf` 里的 `dpi=` 只改 EDID 里的物理尺寸。实测（niri 26.04）：`edid_dpi` +
  `commit` 之后**当场**就换成新的物理尺寸，不需要重启合成器；但它推出来的比例并不
  严格是 `dpi/128`（`0`→160x90mm/2.25、`256`→190x100mm/2.0、`384`→120x70mm/2.25），
  拿来精调缩放不靠谱。
- `input 0` 之后光标是相对的（手指拖动 → 指针走）；`input 1` 是绝对的（点哪就是哪）。
  切的时候会先把所有触点抬起，不会留一个"按住"。

### 配置存在哪：`mode.conf` 是种子，不是真相

- 开机 `service.sh` 读 `mode.conf` 拼 `insmod` 参数 → 决定**开机默认档**；
- 在通知里切档 / 改刷新率之后，App 会把结果**写回 `mode.conf`**（只替换对应那一行，
  注释和其它键不动）。

所以：**通知 = 唯一配置入口，`mode.conf` = 它的持久化记录**。设完重启还是你的选择。

### 怎么判断"真的生效了"

`mode` 是"我要什么"，`active` 是"合成器真正编进 pipe 的是什么"。**两者相等才算成功**：

```sh
cat /sys/module/vdrm/parameters/mode      # 要求值，比如 1920x1080
cat /sys/module/vdrm/parameters/active    # 实际值，必须相等
cat /sys/module/vdrm/parameters/frames_pushed   # 动一下鼠标，这个数要跟着涨
```

`mode` 对、`active` 不对 → 合成器没跟上（多半是那个尺寸 HWC 扫不出去，第 10 章）。
两个都对、`frames_pushed` 不涨 → 没有帧在走（合成器没出图，或 App 不在前台）。

---

## 10. 已知局限（诚实清单）

### 10.1 只用 android14-6.1 实测过

`.ko` 覆盖六档 GKI KMI（`android13-5.10` … `android16-6.12`），但**只在
`android14-6.1` 上实测过**。别的 KMI 装上去，第一反馈可能是黑屏而不是"有点小 bug"
—— 这也是 README 开头要求先单独 `insmod` 试一次的原因。

`service.sh` 会按 `uname -r` 拼出 KMI 再找同名 `.ko`，找不到会明确报
"没有 `vdrm-android14-6.1.ko`"而不是糊过去 —— 这是故意的。

### 10.2 有些分辨率永远上不了屏

**不是驱动的错。** HWC 会按 gralloc 的 stride 规则重建平面布局，把那个 pitch 交给
DRM；真实 pitch 不是这条规则固定点的 buffer 会 `ADDFB2` 失败，平面什么都不显示。
而 GPU 合成（以及截图）看起来完全正常，所以极难查。

本机实测（同一块面板）：

| 尺寸 | 上屏 |
|---|---|
| 1920x1080 / 1080x1920 | ✅ |
| 2780x1264 / 1264x2780 | ✅ |
| 1080x1440 / 1200x2132 | ✅ |
| 1264x2247 / 1264x2246 / 1200x2133 | ❌ |

判断方式永远是 `active` 有没有跟上（见 7 章末）。通知里的两个预置档都避开了这类尺寸；
自己用 `mkpool --mode` 试的时候请以 `active` 为准。

### 10.3 默认档位是"用可能覆写换流畅"，有开关可退

默认情况下 flip-done 在下一个 vblank 就完成，**不等** SurfaceFlinger 的 present。
这正是 88fps 的来源（第 11 章），代价是合成器可能重提交一块 SF 还在显示的显存 ——
`frames_reuse` 会涨（本机连续移动鼠标 10 秒：889 帧、`reuse +519`），实测**视觉无撕裂**。

别的机器上如果看到"局部色块/半帧"，把它当开关用：

```sh
echo 1 > /sys/module/vdrm/parameters/safe_present   # 退回保守模式
echo 0 > /sys/module/vdrm/parameters/safe_present   # 回到默认
```

保守模式把 SF 的 present 又放回了关键路径：

| 模式 | 帧率 | `frames_reuse` |
|---|---|---|
| 默认 0 | 63–90 fps | 涨 |
| 保守 1 | ~13 fps | **0** |

它慢得多，只作排障/兼容用，不是常态。

### 10.4 App 切到后台会显式暂停消费

App 的 `onPause()` 现在会调 `nativeSetPaused(true)`：渲染线程拿到帧立刻 RETURN，
不 latch、不拷贝，并清掉 present/held/拷贝环。回前台的 `onResume()` 先恢复，
再 `nativeSetReady()`，`nativeStart()` 末尾也会补一发 `READY`，让内核重推当前帧。

所以后台不再攒帧；回前台如果合成器桌面完全静止，可能只看到 READY 推来的
那一帧，动鼠标/触摸后继续更新，这是正常的。

### 10.5 Release 回调要 Android 14+ 的 API

App 的 `minSdk` 是 29，但"合成器何时放手这块 buffer"依赖
`ASurfaceTransaction_setBufferWithRelease`（**API 36**）。低版本会退化成
`setBuffer` + 立即归还 —— 能出图，但偶发撕裂。日志里会有一次
`setBufferWithRelease missing; fallback to setBuffer` 的警告。

### 10.6 其它

- 音频只有播放没有录音；App 必须开着才有声音。
- PipeWire 原生 ALSA 输出在部分容器里起不来，日常建议 PulseAudio（见 README）。
- 硬件解码是树外内容（mesa-for-android-container / droidspaces-media-decode），
  不在本仓库和 CI 里。

---

## 11. 原理：为什么以前只有 30fps

这一节解释一个反直觉的结论：**同一套代码、同一个面板，帧率能从 30 变成 88，而
显示通路一行没少。**

### 11.1 链路是串行的

```
合成器提交 → 内核推帧事件 → App 贴到图层 → SF 合成 → 扫描输出 → present
      ↑                                                          │
      └──────────────────── flip-done ◀──────────────────────────┘
```

合成器（niri 这类）是"**上一帧 flip-done 才能画下一帧**"的串行循环。
所以帧率不是 120，而是 `1 / 这一整圈的时间`。

我们量到的是：`present_lat ≈ 30ms`（commit → 帧真的在屏上），1080p120 下
约 3.6 个 vblank 周期 → 30fps。偶发还会飙到 100ms+，那就是"移动鼠标突然卡一下"。

### 11.2 错在把 present 当节拍

第 45、46 项改动的思路是"present 才是真的上屏，flip-done 就该等它"，还加了
500ms 存活兜底。出发点是**正确性**（别让合成器覆写还在显示的显存），但把一条链里
最慢的环节（SF 的 present fence）变成了节拍源。

**别人先量出了这件事。** anland 6.2 的 `services/waylandbridge/awl_sc.cpp` 开头写着：

> frame_done goes out at the vsync TICK for every visible layer … **not from SF's
> OnComplete (one vsync later: a frame-callback-paced client then ran at 29 fps)**

它跟我们同构：用户态造池、把 dma-buf forge 成 AHardwareBuffer、`setBuffer` 贴到
SurfaceControl 兄弟图层。它踩过同样的坑，结论是：**"帧完成"要在 vsync 那一拍发。**

我们照这条改成 `drm_crtc_arm_vblank_event()`（下一个 vblank 完成）：

| | 之前 | 之后 |
|---|---|---|
| 帧率（鼠标连续移动） | 30 fps | **88 fps** |
| `present_lat` | 30ms | 17.5ms（降级为观测值） |
| 帧间隔 >80ms 的次数 | 有，最长 128ms | **0**（最长 40–59ms） |

### 11.3 那"覆写正在显示的显存"怎么办

靠**池的回收链**，而不是靠等 present：

- 槽在 App `RETURN`（= SF 的 release 信号）之前一直是 `DISPL`；
- `dumb_create` 只发 `FREE` 的槽 → 合成器拿不到"可能还在屏上"的那块；
- 残余风险只剩"合成器重提交自己手里的同一块 fb" —— 这就是 `frames_reuse`，
  用来看这个风险的量。

这条路选的是"**用代价换流畅，并把代价量化出来**"，而不是"绝对不撕但永远 30fps"。
两种立场都合理，所以留了 `safe_present` 让你现场切换（10.3）。

### 11.4 顺手踩过的一个坑：换档后第一帧被自己扔了

一次 modeset 提交里的顺序是：

```
commit_modeset_disables → commit_planes(pipe_update 推【新尺寸第一帧】)
                        → commit_modeset_enables(pipe_enable)
```

`pipe_enable` 里原本有一句"清帧队列"（防止旧尺寸的帧贴到新窗口），它把**刚刚推
进去的新帧**一起清掉了。静态桌面换完档不会再有新提交 → App 永远等不到帧事件 →
屏幕上一直贴着旧帧。**这就是"尾帧"。**

修法是两处：`pipe_enable` 不再清队列、改成补推当前帧；App 重建池成功后补发一次
`READY`（因为 `pipe_enable` 推的时候它往往还没 forge 完新尺寸的槽）。

### 11.5 怎么读那几行日志

App 每 2 秒一行（`logcat -s VDRM | grep STATS`）：

```
STATS events=169 shown=169 deferred=0 outstanding=1 over 2s
    | apply2latch=6383us(max 18332) latch2present=14748us(max 106289)
    | evgap_max=59ms n40=2 n80=0 latch_max=2538us
```

| 字段 | 含义 | 怎么判 |
|---|---|---|
| `events` | 这 2 秒收到多少帧事件 | ÷2 ≈ 帧率 |
| `apply2latch` | App 提交图层 → SF 锁定它 | 大了说明我们提交错过了 SF 那一拍 |
| `latch2present` | SF 锁定 → present fence | 它是"真上屏"的耗时，不再是节拍 |
| `evgap_max` / `n40` / `n80` | 相邻帧事件最大间隔 / >40ms、>80ms 的次数 | **卡顿看这里**：`n80` 应为 0 |
| `latch_max` | 我们贴一帧花多久 | 毫秒级正常，几十毫秒说明我们自己在堵 |
| `outstanding` | 在飞的提交数 | 长期 >4 说明回收跟不上 |

内核侧（`dmesg | grep vdrm`）关键两行：

```
vdrm: commit -> 1920x1080@120 dpi=0 (pending=0x1 mode_gen=8 commit_gen=8)
vdrm: pipe enable 1920x1080 period=8333333 ns
```

`commit` 是我们广播了新参数；**`pipe enable` 才是合成器真的换了档**。
只有前者没有后者，就是"参数变了、屏没变"。

---

### 附：三章之外

安装、配容器、起合成器、排障表在 `README.md`；每一项改动的根因与证据在
`vdrm-ko-ddk/CHANGES.md`（按项目顺序编号，42 项）。

---

## 附录 A：容器 / 合成器实测矩阵

环境：Arch Linux ARM 容器、Mesa 27.0.0、niri 26.04、Hyprland 0.57.1、
KWin/Plasma 6.8.2、sway 1.12，全部用**非 root 用户**（uid 1001，组
`video,input,audio,droidspaces-gpu`）。判据一律是 15 秒内
`/sys/module/vdrm/parameters/frames_pushed` 涨不涨。

| 设置 | niri | Hyprland | KWin / Plasma | sway |
|---|---|---|---|---|
| `msm`（+ `LIBSEAT_BACKEND=noop`） | +197 | +1160 | +888（不需要 noop） | +13（空闲无damage） |
| `kgsl`（+ noop） | +465 | +1381 | +1048 | — |
| 不设 `MESA_LOADER_DRIVER_OVERRIDE` | 0 帧，`software EGL renderers are skipped` | 崩 | 1 秒内 SIGTRAP | — |
| 不设 `LIBSEAT_BACKEND=noop` | 0 帧，`Failed to open session: Function not implemented (os error 38)` | `CBackend::create() failed!` | 不受影响 | — |
| 删掉 `/usr/lib/gbm/vdrm_gbm.so` | 0 帧 | 0 帧 | 0 帧 | 0 帧 |
| 不给任何 DRM 设备变量 | 出图 | 出图 | 出图 | 出图（card0 打开失败后自己换 card1） |

标准行为抽查（niri）：
- 热插拔 1920x1080 → 2780x1264：日志只有 `connector changed: Virtual-3` 然后
  `picking mode: Mode { name: "2780x1264" … }`，合成器不重启；
- `grim` 截屏尺寸 = 当前 mode（scale ≠ 1 时拿到的是逻辑尺寸，本次测试里 scale 被
  niri 依物理尺寸定成 2.25/3，属于合成器行为，不是驱动行为）；
- `niri msg outputs` 报 `Current mode: 1920x1080 @ 120.000 Hz (preferred)`；
- `niri msg output Virtual-3 scale 1.5`、workspace、`foot` 等客户端都按标准工作。

两个容易误判的地方：

1. **容器里没有 `card0`。** droidspaces 不绑真 GPU 的主节点（`/dev/dri/by-path/` 里
   `platform-ae00000.qcom,mdss_mdp-card -> ../card0` 是悬空链接），所以**不需要**
   "删掉 card0" 的 udev 规则 —— 实测把那条规则去掉、`udevadm trigger` 之后 card0
   也不会出现。合成器卡在设备选择时先看 `ls /dev/dri`。
2. **缺 render 组和缺 Mesa 覆盖的报错是同一句**
   （`no allocator available for device`），一个查组、一个查
   `MESA_LOADER_DRIVER_OVERRIDE`，别只查一头。

---

## 附录 B：不开「完整硬件访问」的容器

`enable_hw_access=1` 会让容器里的 `/sys` 变可写 —— 容器里装/升级驱动时就可能把整机
软重启一次。所以更稳的配置是**不开硬件访问**，把设备逐个绑进来，再用一个小
systemd 服务代替 udev 建节点（脚本：`tools/vdrm-container-setup.sh`）。

实测（Arch、Mesa 27、`/sys` 只读、非 root 用户）：

```
sysfs on /sys type sysfs (ro,relatime,seclabel)
udevadm trigger … → Failed to write 'change' to '…/uevent': Read-only file system
```

| 检查 | 结果 |
|---|---|
| niri / KWin | 出图（`frames_pushed` 在涨）、`grim` 截 1920x1080 |
| 输入 | 注入 8 个事件 → 容器内 `dd` 读到 192 字节（`/dev/input/event8`，0660 root:input） |
| 音频 | `pcmC0D0p` 能被普通用户打开（服务把 vdrm 那张卡的节点改成 root:audio 0660） |
| 容器重启 | 服务开机自动生效，`/dev/input` 里只有 vdrm 三个设备 |

**坑一（很要命）**：容器里的 `/dev/input`、`/dev/snd` 是宿主机目录的 bind mount，
里面是同一批 inode —— 在容器里 `rm` 一个节点，宿主机上就真的没了。第一版脚本就因为
"以为已经挂了 tmpfs 其实没挂"删掉了宿主机 `/dev/input/event*`，Android 当场没有
触摸和按键，只能强制重启。现在的脚本：先用**自己的 fsname** 挂 tmpfs、挂完再确认一次，
任何一步不对就退出，绝不碰下面的宿主机节点。写自己的容器脚本时请照这个顺序。

****坑二**：光建 `/dev/input/eventN` 还不够 —— libinput 走 libudev 枚举，没有
`ID_INPUT` 属性会直接跳过设备，表现是"合成器起来了、画面正常，但完全没有输入"。
`/sys` 只读时 `udevadm trigger` 跑不了（`Failed to write 'change' …: Read-only file
system`），所以服务要自己写 `/run/udev/data/c13:*`：

```
I:<usec>
E:ID_INPUT=1
E:ID_INPUT_MOUSE=1          # vdrm-ptr
E:ID_INPUT_TOUCHSCREEN=1    # vdrm-touch
E:ID_INPUT_KEY=1 + E:ID_INPUT_KEYBOARD=1   # vdrm-kbd
```

补上之后 `libinput list-devices` 能列出 `vdrm-kbd`(keyboard) / `vdrm-ptr`(pointer) /
`vdrm-touch`(touch)，触摸才真正进来。

**坑三**：模块重新加载后 `eventN` 会变，需要 `systemctl restart vdrm-input`。
