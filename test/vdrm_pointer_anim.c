/*
 * vdrm_pointer_anim.c — 显示/输入链路压测用：以固定节拍注入相对位移
 *
 * 目的：让 niri 的指针持续移动，从而产生稳定的 damage → 提交 → pipe_update
 * → /dev/vdrm 帧事件 → App 上屏，用来测端到端帧率/抖动。
 *
 * freestanding aarch64，无 libc：
 *   clang --target=aarch64-linux-gnu -nostdlib -static -O1 -ffreestanding \
 *         -fno-builtin -fno-stack-protector -o vdrm_pointer_anim vdrm_pointer_anim.c
 *   adb push vdrm_pointer_anim /data/local/tmp/
 *   su -c '/data/local/tmp/vdrm_pointer_anim 30'
 *
 * 参数：秒数（默认 30）。节拍 8ms（约 125Hz），每拍 8px 的方波轨迹。
 */
typedef unsigned int u32;
typedef int s32;
typedef long i64;

#define SYS_openat 56
#define SYS_close 57
#define SYS_ioctl 29
#define SYS_nanosleep 101
#define SYS_exit_group 94

static inline long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
}

struct vdrm_input_event {
	unsigned short type;
	unsigned short code;
	s32 value;
};

#define VDRM_INPUT_BATCH_MAX 16
struct vdrm_input_batch {
	u32 count;
	u32 flags;
	struct vdrm_input_event events[VDRM_INPUT_BATCH_MAX];
};

#define VDRM_IOC(dir, type, nr, size) \
	((unsigned int)(((unsigned int)(dir) << 30) | \
			((unsigned int)(size) << 16) | \
			((unsigned int)(type) << 8) | (unsigned int)(nr)))
#define VDRM_IOCTL_INPUT_BATCH \
	VDRM_IOC(1, 'V', 0x0b, sizeof(struct vdrm_input_batch))

#define EV_REL 0x02
#define REL_X  0x00
#define REL_Y  0x01

struct timespec {
	i64 tv_sec;
	i64 tv_nsec;
};

static int open_dev(const char *path)
{
	return (int)sc(SYS_openat, -100, (long)path, 2 /* O_RDWR */, 0, 0, 0);
}

static void sleep_ms(i64 ms)
{
	struct timespec ts;

	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000;
	sc(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
}

static void exit_now(int code)
{
	sc(SYS_exit_group, code, 0, 0, 0, 0, 0);
	for (;;)
		;
}

static const char msg_ok[] = "pointer_anim: running\n";
static const char msg_err[] = "pointer_anim: open /dev/vdrm failed\n";

static void write_str(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	sc(64 /* write */, 1, (long)s, n, 0, 0, 0);
}

void _start(void)
{
	int fd;
	struct vdrm_input_batch b;
	int i, phase = 0, step = 0;
	int seconds = 30;

	fd = open_dev("/dev/vdrm");
	if (fd < 0) {
		write_str(msg_err);
		exit_now(1);
	}
	write_str(msg_ok);

	/* 30s，8ms 一拍 → 3750 拍；方波：右/下/左/上 各 200 拍，每拍 8px */
	for (i = 0; i < seconds * 125; i++) {
		int dx = 0, dy = 0;

		step++;
		if (step >= 200) {
			step = 0;
			phase = (phase + 1) & 3;
		}
		switch (phase) {
		case 0: dx =  8; break;
		case 1: dy =  8; break;
		case 2: dx = -8; break;
		default: dy = -8; break;
		}

		b.count = 2;
		b.flags = 0;
		b.events[0].type = EV_REL;
		b.events[0].code = REL_X;
		b.events[0].value = dx;
		b.events[1].type = EV_REL;
		b.events[1].code = REL_Y;
		b.events[1].value = dy;
		sc(SYS_ioctl, fd, VDRM_IOCTL_INPUT_BATCH, (long)&b, 0, 0, 0);

		sleep_ms(8);
	}
	sc(SYS_close, fd, 0, 0, 0, 0, 0);
	exit_now(0);
}
