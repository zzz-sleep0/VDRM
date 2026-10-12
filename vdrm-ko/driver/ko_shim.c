/* ko_shim.c —— 手工造模块元数据（全部取自本机）
 *   .modinfo      : vermagic 从本机 /vendor 模块抄
 *   __this_module : 布局取自本机 BTF（ko_types.h）
 *   __versions    : 见 versions.c（本机内核 ksymtab + 本机模块）
 */
#include "ko_types.h"

/* arm64 内核要求模块必须有这三个段（kbuild 会自动生成，我们手工加）*/
__asm__(".pushsection .plt,\"ax\"\n.byte 0\n.popsection\n");
__asm__(".pushsection .init.plt,\"ax\"\n.byte 0\n.popsection\n");
__asm__(".pushsection .text.ftrace_trampoline,\"ax\"\n.byte 0\n.popsection\n");

#define KO_NAME "vdrm"

#define MODINFO(tag, val) \
	static const char __mi_##tag[] \
		__attribute__((section(".modinfo"), used, aligned(1))) = #tag "=" val

MODINFO(name,    KO_NAME);
MODINFO(license, "GPL");
MODINFO(depends, "");
MODINFO(import_ns, "DMA_BUF");
MODINFO(vermagic, "6.1.141-android14-11-g5de16d278e6c SMP preempt mod_unload modversions aarch64");

extern int  init_module(void);
extern void cleanup_module(void);

struct module __this_module
	__attribute__((section(".gnu.linkonce.this_module"), visibility("default"))) = {
	.name = KO_NAME,
	.init = init_module,
	.exit = cleanup_module,
};
