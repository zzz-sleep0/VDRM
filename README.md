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

### 1. App

```sh
gradle assembleRelease          # 产物 app/build/outputs/apk/release/
```

或用仓库里的 GitHub Actions（推送即编）。装到手机上即可。
Native 部分（`app/src/main/jni/`）在构建时由 CMake 现编，不需要预置 `.so`。

### 2. 内核模块

**不需要目标设备在场。** 模块在 DDK 容器里按 KMI 编译，一个 KMI 出一份产物，
通吃该 KMI 下所有厂商/机型：

```sh
# 本地（有 Docker）
cd vdrm-ko-ddk && sh envsetup.sh          # 或用 .devcontainer / docker run

# 或者交给 CI：推送后 .github/workflows/build-vdrm-ko.yml 自动出 vdrm.ko
```

内核树与工具链由 [DDK](https://github.com/Ylarod/ddk) 提供（每个 KMI 一套），
模板照抄 [ddk-module-template](https://github.com/Ylarod/ddk-module-template)。

### 3. 屏蔽模块

`vendor-overlay/` 是个 Magisk / KernelSU 模块，装上它让 Android 不去认那对虚拟键鼠。
**里面的排除表不一定通用**，按 `先看我.md` 的说明把你机器上对应文件 patch 一下。

### 4. 容器侧前置条件

三者缺一不可，否则合成器**连光标都不会画**：

| 项 | 值 |
|---|---|
| 完整硬件访问 | 开（`enable_hw_access=1`） |
| 特权模式 | `nomask` |
| 按节点绑定 | `/dev/input/event10`、`/dev/input/event11` 逐个绑进容器 |

**为什么**：systemd 的冷插拔靠往 `/sys/<设备>/uevent` 写字来重放事件，`/sys` 只读时
这一步**全部失败且不报错**。而容器 udev 又收不到宿主 netns 发出的 uevent ——
两条路都堵死，容器启动前就存在的设备它永远不认识，libinput 一个设备都拿不到。

> 这跟**文件权限无关**：libinput 只认 udev 数据库里的 `ID_INPUT`，拿 root 跑也绕不过去。

### 5. 启动顺序（是死的）

```
insmod vdrm.ko  →  确认 /dev/input/event10,11  →  起容器  →  起合成器
```

按节点绑是绑**文件**，源节点必须在容器启动那一刻就存在。
代价：模块重载后节点被重建，容器里那个绑定会指向已删除的 inode —— **模块一重载就得重启容器**。

---

## 内核模块改了什么

相对最初基线共四处，每项的根因、证据、修法见 `vdrm-ko-ddk/CHANGES.md`：

| # | 改动 | 解决 |
|---|---|---|
| 一 | 池槽改为**会话边界归还** | 消除同一物理内存被发两次；合成器可反复启停 |
| 二 | `atomic_check` 去掉**自锁等待** | 原门控每次白等 ~420ms，拖拽时帧率塌陷 |
| 三 | vblank 网格改为**连续** | 原写法每帧白等满一整个周期才拿到 flip-done |
| 四 | 虚拟键鼠**搬进内核** | 原先设备随用户态守护进程存亡，App 一退就被销毁 → 输入永久失效 |

---

## 已知限制

- 池缓冲的导出 fd 若留在其它存活进程里，会话边界归还机制识别不了。正常整会话启停用不到。
- `vdrm-kbd` 把 `KEY_MAX` 全设了，其中含 `BTN_*`，所以 udev 会把它同时标成
  Keyboard + Mouse（系统里等于有两个指针设备）。不影响使用。

---

## 鸣谢
 Dare3141，提供了项目普适化的必要工具以及解决问题的思路
 Goldzxcbug，Superturtlee，项目引路人以及参考项目的开发者，引导我思考安卓上关于DRM master的归属问题以及解决方法
 lfdevs大佬，该项目想要拥有硬件加速需要打他的驱动
 anland项目，提供了初期思路以及大部分键鼠触控优化方法
 mesa-for-android-container项目，该项目硬件加速所需要的驱动来源于此


## 许可

GPL-2.0
