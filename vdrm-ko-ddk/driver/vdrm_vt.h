/* SPDX-License-Identifier: GPL-2.0 */
/* vdrm_vt.h — 虚拟 VT（major 4 / /dev/tty0..6）的初始化入口。
 *
 * GKI 的 CONFIG_VT=n，内核里没有 VT 层；而 KWin 6.7 的 LogindSession 仍然要
 * 一个 VTNr。这里只实现 VT 的 uAPI（tty 驱动 + /sys/class/tty/tty0/active +
 * VT_ / KD_ 这一类 ioctl），不做 console/fbcon 渲染 —— 图形会话用
 * KD_GRAPHICS，不需要文本。
 */
#ifndef VDRM_VT_H
#define VDRM_VT_H

int vdrm_vt_init(void);
void vdrm_vt_exit(void);

#endif
