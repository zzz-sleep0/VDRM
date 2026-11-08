// SPDX-License-Identifier: GPL-2.0
/*
 * vdrm_vt.c — 虚拟 VT（major 4）：/dev/tty0..6 + /sys/class/tty/tty0/active
 *
 * 为什么需要：GKI 通常 CONFIG_VT=n，内核里根本没有 VT；KWin 的
 * LogindSession::initialize() 要求 session 的 VTNr。没有 VT 时 KWin 只能
 * 落到 NoopSession（直接 open card1），logind 也就不会给 seat。
 *
 * 这里不抄 drivers/tty/vt：只实现 logind/seatd 真正会碰的 uAPI：
 *   VT_OPENQRY / VT_GETSTATE / VT_ACTIVATE / VT_WAITACTIVE /
 *   VT_GETMODE / VT_SETMODE / VT_RELDISP / VT_ACKACQ /
 *   KDSETMODE / KDGETMODE / KDSKBMODE / KDGKBMODE / KDGKBTYPE
 * 以及 /sys/class/tty/tty0/active。图形会话是 KD_GRAPHICS，不需要
 * console/fbcon 渲染，所以 write 直接丢弃。
 *
 * 如果内核本身有 VT（CONFIG_VT=y），major 4 已被占用：tty_register_driver
 * 返回 -EBUSY，这里跳过，不影响原生 VT。
 */
#include "vdrm_vt.h"

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
#include <linux/tty_port.h>	/* 5.14 才从 tty.h 拆出来 */
#endif
#include <linux/tty_flip.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>

#define VT_NR 7
#define VT_MAJOR 4

static struct tty_driver *vt_drv;
static bool vt_registered;
static bool vt_dev_ok[VT_NR];
static bool vdrm_vt_enable = true;
module_param_named(vt, vdrm_vt_enable, bool, 0644);
MODULE_PARM_DESC(vt, "register virtual VT major 4 when the kernel has no VT (default: 1)");
static struct tty_port vt_port[VT_NR];
static int vt_active = 1;
static int vt_kbdmode[VT_NR];
static int vt_textmode[VT_NR];
static struct vt_mode vt_mode[VT_NR];
static struct pid *vt_owner[VT_NR];
static wait_queue_head_t vt_wait;
static int vt_pending = -1;
static struct device *vt0_dev;

/* 6.6 把 tty_port_operations 的这两个回调改成 bool：
 *   carrier_raised() -> bool，dtr_rts(port, bool)。5.x 还是 int。*/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static bool vt_carrier_raised(struct tty_port *port) { return true; }
static void vt_dtr_rts(struct tty_port *port, bool raise) { }
#else
static int vt_carrier_raised(struct tty_port *port) { return 1; }
static void vt_dtr_rts(struct tty_port *port, int raise) { }
#endif
static const struct tty_port_operations vt_port_ops = {
	.carrier_raised = vt_carrier_raised,
	.dtr_rts = vt_dtr_rts,
};

static int vt_install(struct tty_driver *driver, struct tty_struct *tty)
{
	if (tty->index < 0 || tty->index >= VT_NR)
		return -ENODEV;
	return tty_port_install(&vt_port[tty->index], driver, tty);
}

static int vt_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(&vt_port[tty->index], tty, filp);
}

static void vt_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(&vt_port[tty->index], tty, filp);
}

static void vt_hangup(struct tty_struct *tty)
{
	tty_port_hangup(&vt_port[tty->index]);
}

/* 6.6 起 tty_operations.write 用 ssize_t + size_t（原来是 int）。*/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static ssize_t vt_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
	return count;
}
#else
static int vt_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	return count;
}
#endif

static unsigned int vt_write_room(struct tty_struct *tty)
{
	return 4096;
}

static void vt_send_sig(int idx, int sig)
{
	struct pid *pid = vt_owner[idx];
	if (pid && sig)
		kill_pid(pid, sig, 1);
}

static void vt_complete_switch(int target)
{
	int old = vt_active;
	if (target < 1 || target >= VT_NR)
		return;
	vt_active = target;
	vt_pending = -1;
	if (vt0_dev)
		sysfs_notify(&vt0_dev->kobj, NULL, "active");
	vt_send_sig(target, vt_mode[target].acqsig);
	wake_up_interruptible_all(&vt_wait);
	pr_info("vdrm_vt: switched %d -> %d\n", old, target);
}

static int vdrm_vt_ioctl(struct tty_struct *tty, unsigned int cmd, unsigned long arg)
{
	int idx = tty->index;

	switch (cmd) {
	case VT_OPENQRY: {
		int n = 1;
		if (copy_to_user((int __user *)arg, &n, sizeof(n)))
			return -EFAULT;
		return 0;
	}
	case VT_GETSTATE: {
		struct vt_stat st = { .v_active = vt_active, .v_signal = 0 };
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	case VT_GETMODE:
		if (copy_to_user((void __user *)arg, &vt_mode[idx], sizeof(vt_mode[idx])))
			return -EFAULT;
		return 0;
	case VT_SETMODE: {
		struct vt_mode m;
		if (copy_from_user(&m, (void __user *)arg, sizeof(m)))
			return -EFAULT;
		if (m.mode != VT_AUTO && m.mode != VT_PROCESS)
			return -EINVAL;
		vt_mode[idx] = m;
		put_pid(vt_owner[idx]);
		vt_owner[idx] = (m.mode == VT_PROCESS) ? get_pid(task_tgid(current)) : NULL;
		return 0;
	}
	case VT_ACTIVATE: {
		int target = (int)arg;
		if (target < 1 || target >= VT_NR)
			return -EINVAL;
		if (target == vt_active)
			return 0;
		if (vt_mode[vt_active].mode == VT_PROCESS && vt_owner[vt_active]) {
			vt_pending = target;
			vt_send_sig(vt_active, vt_mode[vt_active].relsig);
			return 0;
		}
		vt_complete_switch(target);
		return 0;
	}
	case VT_WAITACTIVE: {
		int target = (int)arg;
		if (target < 1 || target >= VT_NR)
			return -EINVAL;
		return wait_event_interruptible(vt_wait, vt_active == target) ? -ERESTARTSYS : 0;
	}
	case VT_RELDISP:
		if (arg == 1 && vt_pending >= 0) {
			int t = vt_pending;
			vt_complete_switch(t);
		} else if (arg == 0) {
			vt_pending = -1;
		}
		return 0;
	case VT_ACKACQ:
		return 0;
	case KDSETMODE:
		vt_textmode[idx] = (int)arg;
		return 0;
	case KDGETMODE:
		if (copy_to_user((int __user *)arg, &vt_textmode[idx], sizeof(int)))
			return -EFAULT;
		return 0;
	case KDSKBMODE:
		vt_kbdmode[idx] = (int)arg;
		return 0;
	case KDGKBMODE:
		if (copy_to_user((int __user *)arg, &vt_kbdmode[idx], sizeof(int)))
			return -EFAULT;
		return 0;
	case KDGKBTYPE: {
		char t = KB_101;
		if (copy_to_user((char __user *)arg, &t, sizeof(t)))
			return -EFAULT;
		return 0;
	}
	case VT_DISALLOCATE:
	case VT_RESIZE:
	case VT_RESIZEX:
	case KDSETLED:
	case KDMAPDISP:
	case KDUNMAPDISP:
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct tty_operations vt_ops = {
	.install = vt_install,
	.open = vt_open,
	.close = vt_close,
	.hangup = vt_hangup,
	.write = vt_write,
	.write_room = vt_write_room,
	.ioctl = vdrm_vt_ioctl,
};

static ssize_t active_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "tty%d\n", vt_active);
}
static DEVICE_ATTR_RO(active);

int vdrm_vt_init(void)
{
	int i, ret;

	if (!vdrm_vt_enable)
		return 0;

	init_waitqueue_head(&vt_wait);
	for (i = 0; i < VT_NR; i++) {
		tty_port_init(&vt_port[i]);
		vt_port[i].ops = &vt_port_ops;
		vt_kbdmode[i] = K_XLATE;
		vt_textmode[i] = KD_TEXT;
		vt_mode[i].mode = VT_AUTO;
	}

	vt_drv = tty_alloc_driver(VT_NR, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(vt_drv)) {
		ret = PTR_ERR(vt_drv);
		pr_err("vdrm_vt: tty_alloc_driver failed: %d\n", ret);
		vt_drv = NULL;
		goto err_ports;
	}
	vt_drv->driver_name = "vdrm-vt";
	vt_drv->name = "tty";
	vt_drv->major = VT_MAJOR;
	vt_drv->minor_start = 0;
	vt_drv->type = TTY_DRIVER_TYPE_CONSOLE;
	vt_drv->subtype = SYSTEM_TYPE_CONSOLE;
	vt_drv->init_termios = tty_std_termios;
	vt_drv->ops = &vt_ops;

	ret = tty_register_driver(vt_drv);
	if (ret) {
		/* 内核自带 VT（CONFIG_VT=y）时 major 4 已经被占；跳过即可。 */
		pr_info("vdrm_vt: major 4 unavailable (%d), native VT assumed; skip\n", ret);
		tty_driver_kref_put(vt_drv);
		vt_drv = NULL;
		goto err_ports;
	}
	vt_registered = true;

	for (i = 0; i < VT_NR; i++) {
		struct device *dev;
		tty_port_link_device(&vt_port[i], vt_drv, i);
		dev = tty_register_device(vt_drv, i, NULL);
		if (IS_ERR(dev)) {
			pr_err("vdrm_vt: register tty%d failed: %ld\n", i, PTR_ERR(dev));
			continue;
		}
		vt_dev_ok[i] = true;
		if (i == 0) {
			vt0_dev = dev;
			device_create_file(dev, &dev_attr_active);
		}
	}
	pr_info("vdrm_vt: registered virtual VT major 4, /dev/tty0..%d active=%d\n", VT_NR - 1, vt_active);
	return 0;

err_ports:
	for (i = 0; i < VT_NR; i++)
		tty_port_destroy(&vt_port[i]);
	return 0;	/* best-effort: VT 不可用不该拖垮整个 vdrm */
}

void vdrm_vt_exit(void)
{
	int i;
	if (vt0_dev) {
		device_remove_file(vt0_dev, &dev_attr_active);
		vt0_dev = NULL;
	}
	for (i = 0; i < VT_NR; i++) {
		if (vt_dev_ok[i]) {
			tty_unregister_device(vt_drv, i);
			vt_dev_ok[i] = false;
		}
		put_pid(vt_owner[i]);
		vt_owner[i] = NULL;
		tty_port_destroy(&vt_port[i]);
	}
	if (vt_registered)
		tty_unregister_driver(vt_drv);
	tty_driver_kref_put(vt_drv);
	vt_drv = NULL;
	vt_registered = false;
	pr_info("vdrm_vt: unregistered\n");
}
