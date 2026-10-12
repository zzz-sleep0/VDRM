# 将 VDRM 移植到你的 Android 内核

从这份源码生成适配**你这台设备**的 `vdrm.ko`。不需要下载内核源码，不需要手填 CRC。

---

## 速通

在一台已 root 的 Android 手机上的 **Termux** 里：

```sh
mkdir -p ~/vdrm-port
tar -xzf /path/to/vdrm-ko-porting-kit-updated.tar.gz -C ~/vdrm-port
cd ~/vdrm-port/porting-kit-ko
sh scripts/quickstart.sh
```

就这样。脚本自己装依赖、自己挑参考模块、自己调 `su` 做需要 root 的两步，
最后把 `vdrm.ko` 放到 `/data/local/tmp/`。

```sh
sh scripts/quickstart.sh --load        # 顺手加载
VDRM_REF_KO=/path/x.ko sh scripts/quickstart.sh   # 手动指定参考模块
```

**前提**：设备已 root；装了 [KPatch-Next](https://github.com/KernelSU-Next/KPatch-Next)
（只在采集 CRC 时用一下，最终产物是普通 `.ko`）。

想搞清楚每一步在干什么、或者脚本在哪一步断了要接着走，看下面的分步说明。

---

## 与原始基线的差异

只看结论。每项的根因、证据、修法在 **`CHANGES.md`**。

只看结论。每项的根因、证据、修法在 **`CHANGES.md`**。

| # | 改了什么 | 解决 |
|---|---|---|
| 一 | 池槽改为**会话边界归还** | `RETURN` 不再等于"分配已归还"；复用只发生在合成器 `drm_file` 关闭时。消除别名，合成器可反复启停 |
| 二 | `atomic_check` 去掉**自锁等待** | 原门控等的是一个"只有它放行才可能发生"的事件，每次白等 ~420ms，拖拽时帧率塌陷 |
| 三 | vblank 网格改为**连续** | 原写法每次取引用都从"现在"重算一整个周期，于是每次提交都白等满 16.67ms 才拿到 flip-done |
| 四 | 虚拟键鼠**搬进内核** | 原先设备属于用户态守护进程，App 一退设备即被销毁，容器里的合成器察觉不到 → 输入永久失效 |

## 容器侧的前置条件（跑起来之前必须满足）

**① 容器必须让 `/sys` 可写**

systemd 的冷插拔（`systemd-udev-trigger.service` → `udevadm trigger`）靠**逐个往
`/sys/<设备>/uevent` 写字符串**来重放事件。容器 `/sys` 只读时这一步**全部失败**
（`Failed to write 'change' to '.../uevent': Read-only file system`），冷插拔等于没做。

Droidspaces 的容器设置里打开这两项：

| 设置 | 值 | 说明 |
|---|---|---|
| 完整硬件访问 | 开（`enable_hw_access=1`） | |
| 特权模式 | `nomask` | 去掉对 `/sys` 的遮蔽，让它成为真正可写的 sysfs |

**② 为什么非它不可**

容器里的 udev 只有两条路能认识一个设备：

- **真实 uevent** —— 收不到（宿主 netns 发出的 uevent 不进容器）
- **冷插拔** —— 需要可写 `/sys`

两条都堵死时，**容器启动前就存在的设备，udev 永远不知道**。而"设备先于容器存在"正是
下面第 ③ 条所要求的。结果就是 libinput 一个设备都拿不到，合成器**连光标都不画**。

> 这跟**文件权限无关**。libinput 的 udev 后端只认 udev 数据库里的 `ID_INPUT` 属性，
> 挂着 root 跑合成器也绕不过去 —— 这一点很容易误判，实测过。

**③ 容器按节点逐个绑定**

```
bind_mounts=/dev/dri/card1:/dev/dri/card1,/dev/input/event10:/dev/input/event10,/dev/input/event11:/dev/input/event11
```

**启动顺序是死的**：`insmod` → 起容器 → 起合成器。
逐节点绑是绑**文件**，源节点必须在容器启动那一刻就存在。

> 代价：模块重载时内核会删除并重建 `/dev/input/eventN`，容器里那个 bind 会指向
> **已删除的 inode**，变成一个打不开的死节点。所以**模块一重载就得重启容器**。

## 仍未解决

- 若合成器退出时把池缓冲的导出 fd 留在仍存活的其它进程里，本机制无法识别。要彻底关闭需"每次分配精确生命周期"。正常整会话启停的使用方式不会遇到。

驱动中保留有界的诊断日志（`DIAG_ALLOC`／`DIAG_RETURN`，各限 32 条），用于核对分配与归还；不影响逻辑。

## 先确认条件

- ARM64 Android 6.1 内核，设备已 root。
- 已安装且可用的 KPatch-Next。它只用于读取内核的 CRC 和类型哈希，最终驱动是普通 `.ko`。
- 内核存在 `/sys/kernel/btf/vmlinux` 和 `/proc/config.gz`。
- 内核开启 MODVERSIONS、PREL32 导出表；未强制模块签名，未使用相对 CRC 表。脚本会检查这些条件，不满足时停止。
- 你已经有能使用这个驱动的 App/helper 和 DRM 合成器。本包只构建内核驱动，不包含完整的显示客户端安装流程。

## 1. 解压到固定目录

以下命令在 **普通 Termux shell** 执行。不要在共享存储里编译；共享存储仅用于传递压缩包。

```sh
mkdir -p "$HOME/vdrm-port"
tar -xzf /storage/emulated/0/vdrm-ko-porting-kit-updated.tar.gz -C "$HOME/vdrm-port"
cd "$HOME/vdrm-port/porting-kit-ko"
pkg install clang lld python binutils dwarves kmod curl tar
curl -fL 'https://gh-proxy.org/https://github.com/KernelSU-Next/KPatch-Next/archive/refs/heads/main.tar.gz' -o KPatch-Next.tar.gz
tar -xzf KPatch-Next.tar.gz
```

执行完的目录：

```text
~/vdrm-port/porting-kit-ko/
  driver/                 驱动源码
  scripts/                后续要执行的脚本
  tools/                  类型生成工具
  kpm/                    采集器源码
  KPatch-Next-main/        编译需要的头文件
```

若 Termux 仓库没有某项依赖，先解决依赖，勿跳过：需要 `clang`、`ld.lld`、`python3`、`nm`、`readelf`、`modprobe`、`modinfo`、`pahole` 和支持 ARM64 ELF 的 `objcopy`。某些环境的 objcopy 名称是 `gobjcopy`，第 3 步可指定 `--objcopy gobjcopy`。

## 2. 读取你的设备信息

仍在包根目录，执行 `pwd` 并记住显示的绝对路径，然后进入 **Android root shell**：

```sh
pwd
su
```

进入 root 后显式回到刚才的路径。以下以标准 Termux 路径为例，若你的安装路径不同，替换这一个路径：

```sh
cd /data/data/com.termux/files/home/vdrm-port/porting-kit-ko
sh scripts/host.sh snapshot "$PWD/target" /vendor_dlkm/lib/modules/msm_drm.ko
exit
```

`exit` 返回普通 Termux。生成的 `target/` 中包含：

| 文件 | 用途 |
|---|---|
| `vmlinux.btf` | 生成这个内核的结构体布局与成员偏移 |
| `config.txt` | 决定编译和校验选项 |
| `kernel-release.txt` | 防止混用其他内核的采集结果 |
| `vermagic.txt` | 写入模块的版本信息 |
| `anchor.ko` | 从目标设备现成模块交叉核对 CRC |

如果设备没有 `msm_drm.ko`，替换成目标设备上已知能加载、且引用 `module_layout`、`fget`、`mutex_lock` 的模块。后续 anchors 脚本会检查它是否满足条件。

快照目录必须是新的；重做时用 `target-2` 等新名称，后续命令一起替换。

## 3. 自动生成头文件和采集器

在 **普通 Termux shell**、包根目录执行：

```sh
python3 scripts/port.py prepare \
  --kp "$PWD/KPatch-Next-main" \
  --target "$PWD/target" \
  --build "$PWD/build"
```

这一步自动完成：

1. 用目标 BTF 生成 `build/vdrm_types.h` 和 `build/ko_types.h`。结构体成员偏移体现在这些 C 头的布局中，**没有额外的偏移文件需要手工放入源码**。
2. 规范化整数类型，编译 `build/vdrm_ko.o`。
3. 从对象提取全部需要的内核符号，生成 `build/request.h` 和 `build/manifest.json`。
4. 编译 `build/collector.kpm`，它按上述清单一次采集所需 CRC 和类型哈希。

所有文件都保留在 `build/`，不要移动头文件、修改 manifest 或手填 request。prepare 报错时不要继续 collect；重做使用新的 build 目录。

**跨设备 ABI 检查不能跳过。** 当前驱动仍有少量目标相关固定值：`driver/vdrm_ko.c` 的 `VDRM_GFP_KERNEL_ZERO`，以及 `vdrm_do_setmode()` 中访问 master/open 的固定偏移（88、216）。这些没有被脚本自动生成。移植前必须根据目标 BTF/内核定义核对；需要修改时在 driver 中修改，然后用新 build 目录重新 prepare。外部函数原型 `driver/vdrm_exts.h` 也须与目标 BTF 一致；编译类型检查不能替代完整 ABI 检查。其他内核分支不属于本包的自动支持范围。

## 4. 一次采集全部 CRC

从包根目录进入 **Android root shell**：

```sh
su
cd /data/data/com.termux/files/home/vdrm-port/porting-kit-ko
sh scripts/host.sh collect "$PWD/build" "$PWD/capture"
exit
```

脚本加载独立的 `vdrm_port_crc` 采集器，读取后卸载它，**不会卸载、替换或加载 vdrm 驱动**。

生成：

```text
capture/crcs.tsv          所有所需导出符号的 CRC
capture/hashes.tsv        kCFI 基准（目标开启 kCFI 时）
capture/kernel-release.txt
```

采集器在 `/data/local/tmp/vdrm-port-session/` 保存工作文件。再次采集前，先保留或重命名这个旧目录和 capture 目录；脚本不覆盖旧结果。

若 KPatch 安装路径不同，root shell 执行 collect 前设置：

```sh
export KPATCH_BIN=/你的路径/kpatch
```

## 5. 校验并构建模块

在 **普通 Termux shell**、包根目录执行：

```sh
python3 scripts/anchors.py target/anchor.ko capture/anchors.tsv
python3 scripts/port.py finish --build "$PWD/build" --capture "$PWD/capture"
```

finish 自动核对采集值，生成 **`build/versions.c`**（模块链接所需的 `__versions` 表），编译并链接 shim 和版本表，然后检查 CRC、vermagic、部分回调 kCFI、必需 ELF 段与重定位。

你不需要编辑或搬运 versions.c，它已自动参与链接。

最终文件是：

```text
~/vdrm-port/porting-kit-ko/build/vdrm.ko
```

工具通过的是静态检查，不是运行保证。其余回调类型、结构布局和目标相关固定值仍须完成 ABI 核对；不得用别的设备采集结果让检查“通过”。

## 6. 在目标设备加载

先停止使用 vdrm 的 App/helper 和合成器。进入 Android root shell：

```sh
su
cp /data/data/com.termux/files/home/vdrm-port/porting-kit-ko/build/vdrm.ko /data/local/tmp/vdrm.ko
```

如果已有旧 vdrm，先运行 `rmmod vdrm`，**成功后**再加载：

```sh
insmod /data/local/tmp/vdrm.ko
dmesg | grep -i vdrm
```

首次没有加载旧模块时不需要 rmmod。加载失败立即停止，不使用 `insmod -f`。加载成功后按你的 App/helper 流程注册 buffer 池、启动合成器，再测试显示、glmark2-wayland、RETURN 和卸载/重载。

## 使用容器编译

执行步骤不变，区别仅是位置：第 2、4、6 步在 Android 宿主，其他步骤在容器。将 target 目录整体带到容器；prepare 后将整个 build 目录放到宿主可访问路径执行 collect；把 capture 目录整体带回容器执行 finish。不要只传 crcs.tsv 而遗漏身份和 kCFI 文件。

容器里的 `/sys/kernel/btf/vmlinux` 不一定是你准备加载模块的目标，必须使用宿主 snapshot 导出的 target。容器无需挂载全部 Android 分区，也不负责加载模块。

## 出错时看这里

| 报错/现象 | 处理 |
|---|---|
| 找不到 pahole/objcopy/modprobe | 安装依赖；使用支持 ARM64 ELF 的工具 |
| snapshot 提取不到 vermagic | 确认参考模块是未压缩 `.ko` 且属于目标设备 |
| anchors 缺 fget/mutex_lock | 更换同时引用这两个符号的目标参考模块 |
| Unsupported configuration/table layout | 目标不在当前采集器支持范围；不要修改 CRC 数值绕过 |
| missing export | 内核未导出这个符号，CRC 无法解决，需要调整实现或内核 |
| CRC conflict/anchor mismatch | 采集器布局或设备资料不一致，检查快照和参考模块来源 |
| kCFI mismatch/missing baseline | 核对目标函数原型与整数类型；不要禁用 kCFI |
| Object changed | 用新的 build 目录重新 prepare 和 collect |
| 已存在 target/build/capture | 保留旧数据，用新目录名重做 |
| 加载成功但没有画面 | 检查 App/helper 注册池、合成器 DRM 设备和 buffer RETURN；本包不是完整显示客户端 |

## 包内文件

| 路径 | 用途 |
|---|---|
| `scripts/quickstart.sh` | **速通**：一条命令跑完下面 1–6 步 |
| `scripts/port.py` `anchors.py` `host.sh` | 分步执行时用 |
| `driver/vdrm_ko.c` `vdrm_exts.h` | 驱动源码（`vdrm_types.h` 由 prepare 生成，不在包里） |
| `kpm/` `tools/` | 采集器源码 / 类型与符号工具 |
| `CHANGES.md` | 与原始基线的差异：每项的根因、证据、修法 |
| `tests/` | 本包的单元测试（`python3 -m unittest discover -s tests`） |
| `examples/reference/` | 特定已验证设备的复现资料，**新设备不要加 `--reference`** |

正常移植不需要读 `CHANGES.md`，也不需要从 `examples/reference/` 复制任何东西。
