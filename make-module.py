#!/usr/bin/env python3
"""把各组件编出来的东西收进 module/，打成一个可安装的模块 zip。

    ./make-module.py           组装 vdrm-lkm-<version>.zip
    ./make-module.py --check   只检查齐不齐，不打包

组件各编各的，这里只做装配：

    module/                  模块本身（module.prop、service.sh、status.sh、
                             uninstall.sh、system/vendor/etc/…xml）—— 入库
    vdrm-ko-ddk/             .ko 源码；DDK/CI 编出六个 KMI 一批，文件名【就是 KMI】
                                 vdrm-ko-ddk/vdrm-android14-6.1.ko
                             —— 这是契约，service.sh 按它精确匹配，别改名
    vdrm-mkpool/             建池 helper；要 NDK 的 bionic clang（见那个 Makefile）
                                 vdrm-mkpool/vdrm_mkpool

后两样都是构建产物，不入库。用 python3 的 zipfile 而不是 zip/unzip：这台机器和
Termux 上都没有 zip。
"""

import pathlib
import sys
import zipfile

KMI = [
    "android13-5.10",
    "android13-5.15",
    "android14-5.15",
    "android14-6.1",
    "android15-6.6",
    "android16-6.12",
]

ROOT = pathlib.Path(__file__).resolve().parent
MOD = ROOT / "module"


def version() -> str:
    for line in (MOD / "module.prop").read_text().splitlines():
        if line.startswith("version="):
            return line.split("=", 1)[1].strip()
    return "unknown"


def main() -> int:
    kos = [ROOT / "vdrm-ko-ddk" / f"vdrm-{k}.ko" for k in KMI]
    pool = ROOT / "vdrm-mkpool" / "vdrm_mkpool"
    need = kos + [pool]

    missing = [p for p in need if not p.is_file()]
    if missing:
        print("缺：")
        for p in missing:
            print(f"    {p.relative_to(ROOT)}")
        print()
        print("  .ko     六个 KMI 全要 —— CI 产出后放进 vdrm-ko-ddk/，文件名就是 KMI")
        print("  mkpool  先在 vdrm-mkpool/ 里编出来（见那个目录的 Makefile）")
        return 1

    print(f"齐了：{len(kos)} 个 .ko + vdrm_mkpool")

    if "--check" in sys.argv:
        return 0

    out = ROOT / f"vdrm-lkm-{version()}.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        # 模块自己的文件：按模块根目录放 —— 这个 zip 解开就是模块目录
        for p in sorted(MOD.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(MOD))
        # 载荷：摊在根目录，不带路径
        for p in need:
            z.write(p, p.name)

    print(f"  → {out.name}  ({out.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
