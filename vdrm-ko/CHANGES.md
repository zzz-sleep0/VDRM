# 本版本与原始基线的差异

> 这里记的是**为什么这么改**：每一项的根因、证据、修法。
> 只想把模块跑起来的话看 `README.md` 就够了，不必读这份。

## 本版本第一项改动：池槽的会话边界归还

驱动维护一个由 App 注册的显示缓冲池，合成器通过 `CREATE_DUMB` 从池里取缓冲。此前版本有一个缺陷：

- `RETURN`（App 显示完毕）会把槽置为 FREE，但合成器仍持有该缓冲的 GEM handle／导出 fd／GPU 导入；
- 下一次 `CREATE_DUMB` 会把同一物理内存再发一次 → 两个合成器 BO 指向同一片内存（别名），表现为启动阶段卡顿、局部画面错乱。

本版本的处理：

1. 分配成功后该槽记 `allocation_reserved`，**会话内不再发放**（消除别名）；
2. 归还**只发生在合成器的 drm_file 关闭时**（`drm_driver.postclose`）：解除占用，TAKEN 的置 FREE，DISPL 的仍由 App 的 `RETURN` 决定；
3. 不再用 `handle_count == 0` 推断占用结束，也不在池满时回退非池缓冲（改为返回 `-ENOSPC`）。

效果（已在参考设备实测）：首次启动流畅；合成器退出后再次启动能重新取到缓冲，可反复启停；分配日志中同一槽跨会话复用时 `handle_count` 均为 0，无别名。

## 本版本第二项改动：`atomic_check` 不再阻塞提交

上一版的 `vdrm_atomic_check` 在发现本次提交要用的池缓冲仍处于「已上屏、App 未 `RETURN`」时，会这样做：

```c
while (g_pool[idx].state == VDRM_ST_DISPL && w < 50) {   /* 最多 50 次 */
        msleep(1);
        w++;
}
```

这是个**自锁的等待**。App 的 `RETURN` 挂在 Android 的 release fence 上，而 SF 只有在收到更新的 buffer 之后才会释放旧 buffer；更新的那一帧由本次提交的 `pipe_update` 推出。串起来是：

```text
gate 等 RETURN(N)
      ← SF 释放 N（release fence 触发）
      ← App 提交 N+1
      ← App 收到帧事件 N+1
      ← pipe_update(N+1) 推事件
      ← 本次提交完成 —— 而它正被 gate 挡着
```

也就是说，它等的事情只有它先放行才可能发生，必然空等到超时。

实测（HZ=120 时 `msleep(1)` 实际睡满一个 jiffy ≈ 8.33ms，50 轮 ≈ 420ms）：日志里**每一次都跑满 50 轮才放行，没有一次提前退出**。

后果：合成器每做一次包含池缓冲的提交就被冻结约 420ms。拖拽鼠标时每次移动都触发一次提交，于是光标只能每约 0.5 秒跳一格 —— 看上去「只显示起点和终点」，同时桌面整体帧率塌到约 1/3。

**本版去掉这个等待**，只保留计数。同一台设备、同一段拖拽实测：

| 指标 | 改动前 | 改动后 |
|---|---|---|
| 提交间隔 p50 | 31.8 ms | 31.8 ms |
| 提交间隔 p99 | **465 ms** | **34.7 ms** |
| 拖拽中 >150ms 的停顿 | **46 次** | 9 次（基本落在起手/收手边界） |

基础节奏（约 31ms，由 App 渲染回路决定）两边完全一致 —— 差别全在那几十次 420ms 冻结。

去掉它不损失语义：该缓冲本来就会在 `pipe_update` 里被重新置 `DISPL` 并补推帧事件，提交路径不需要在此阻断。

## 本版本第三项改动：vblank 网格改为连续

虚拟显示没有真实扫描，vblank 是驱动用 hrtimer 造的。旧写法有两个问题：

```c
/* enable_vblank：每次取引用都从“现在”重新起算一整个周期 */
g_vblank_on = 1;
hrtimer_start_range_ns(&g_vblank_timer, g_vblank_period_ns, 0, HRTIMER_MODE_REL);

/* disable_vblank：放引用时把它取消 */
g_vblank_on = 0;
hrtimer_try_to_cancel(&g_vblank_timer);
```

合成器每提交一帧都走 `get → arm → put` 一轮（日志里 `vblank on` / `vblank off`
各 1055 条，和 1055 次提交一一对应）。所以**每一次提交都要白等整整一个周期
（60Hz 下 16.67ms）才收到 flip-done** —— 不管它落在 vblank 周期里的哪个位置。
真实显示器的下一个 vblank 在 `[0, 16.67)` 内任意位置，我们这里永远是满值。

实测：提交间隔**恒为 31.8ms**（`16.67 等待 + ~15 渲染`），纹丝不动。表现就是
拖动鼠标时每 31.8ms 光标才落一个新位置，看着有残影。

修法：vblank 网格**连续**。

- `enable_vblank` 只在网格没跑时启动，之后不再重起（相位不被打断）；
- `disable_vblank` 只放引用，**不**停定时器；
- 定时器回调持续自转发；没人持引用时不调 `drm_crtc_handle_vblank()`
  （此时 `vblank->enabled` 为 false，调了会 WARN）；
- 网格由 `pipe disable`（`vdrm_pipe_disable`）和模块卸载统一停。

不引入任何新的内核符号（`hrtimer_start_range_ns` / `hrtimer_forward` /
`hrtimer_try_to_cancel` / `drm_crtc_handle_vblank` 本来就在引用表里），
所以 CRC 采集结果不用重做。

效果：参考设备上拖拽变顺滑（使用者确认）。另外留了一个诊断旋钮：

```c
#define VDRM_VBLANK_HZ_FORCE 0   /* 非 0 时强制虚拟显示的帧率(Hz)，0 = 按 mode 反推 */
```

用来分辨“合成器跟不上”到底是 vblank 定拍造成的，还是它自己渲染慢：
把它设成 30 或 120 重编一次，看提交间隔跟着变不变即可。默认 0，不改变行为。

## 本版本第四项改动：虚拟键鼠改成内核里的 input 设备

早前版本里，虚拟键鼠是 App 用 `socketpair` fork 一个 root 守护进程（`vdrm_inputd`），
由它打开 `/dev/uinput` 造设备、再把事件写进去。

这个结构有个致命问题：**设备属于那个用户态进程**。App 一退出守护进程随之退出，内核销毁
设备；App 再进来设备被**重新创建**。而容器里的合成器**没有任何办法知道设备换过了** ——
容器 udev 收不到宿主 netns 发出的 uevent（见下节），一旦设备被销毁，libinput 会永远抱着
那个已经死掉的 fd。表现为"App 重启一次，输入永久失效"。

本版本把设备搬进驱动：由 KO 直接注册两个 `input_dev`，事件经 `VDRM_IOCTL_INPUT` 注入。

```
App ──ioctl(/dev/vdrm, VDRM_IOCTL_INPUT, {type,code,value})──► KO ──input_event()──► evdev
```

设备生命周期从此**绑在模块上**，App 反复开关都不动它。顺带两个好处：

- **`vdrm_inputd` 整个消失** —— 少一个进程、少一条部署路径
- **节点由内核 / ueventd 管** —— 注册时建、注销时删，不会留下悬空节点

亚像素累积与滚轮折算从守护进程上移进了 App。新增的内核引用 6 个（采集时会自动校验）：

```
input_allocate_device  input_free_device   input_register_device
input_unregister_device  input_event       input_set_capability
```

> `input_sync` 在本内核里**没有导出符号**，所以同步改用
> `input_event(dev, EV_SYN, SYN_REPORT, 0)`。

设备名固定为 `vdrm-kbd` / `vdrm-ptr`。它们仍然是**内核级**输入设备，Android 侧也会认到，
所以 `/vendor/etc/excluded-input-devices.xml` 里那两条**仍然是必需的**。
