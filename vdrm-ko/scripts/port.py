#!/usr/bin/env python3
"""Explicit prepare / finish stages; never load or deploy a kernel module."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ANCHORS = {
    'vdrm_vblank_timer_fn': 'hrtimer_wakeup',
    'vdrm_dumb_create': 'drm_gem_shmem_dumb_create',
    'vdrm_atomic_check': 'drm_atomic_helper_check',
    'vdrm_misc_ioctl': 'drm_ioctl', 'vdrm_misc_read': 'drm_read',
    'vdrm_misc_poll': 'drm_poll', 'vdrm_misc_release': 'drm_release',
    'vdrm_dumb_map_offset': 'drm_gem_dumb_map_offset',
    'vdrm_pool_mmap': 'drm_gem_shmem_mmap',
}
FLAGS = ['--target=aarch64-linux-gnu', '-std=gnu11', '-O2', '-ffreestanding',
         '-fno-builtin', '-fno-pic', '-fno-pie', '-fno-asynchronous-unwind-tables',
         '-fno-stack-protector', '-mno-outline-atomics', '-mgeneral-regs-only',
         '-Werror=incompatible-function-pointer-types', '-Werror=incompatible-pointer-types']

def run(*args):
    return subprocess.check_output([str(x) for x in args], text=True)

def table(path):
    text = Path(path).read_text()
    if text and not text.endswith('\n'):
        raise ValueError('truncated table: ' + str(path))
    out = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) != 2 or not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', fields[0]) or not re.fullmatch(r'0x[0-9a-fA-F]{8}', fields[1]):
            raise ValueError('invalid table row: ' + line)
        name, value = fields
        if name in out:
            raise ValueError('duplicate symbol: ' + name)
        out[name] = int(value, 16)
    return out

def versions(path):
    return {n: int(c, 16) for c, n in (l.split() for l in run('modprobe', '--show-modversions', path).splitlines())}

def need(path):
    return sorted({l.split()[-1] for l in run('nm', '-u', path).splitlines() if l.strip()} | {'module_layout'})

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def config(path):
    return set(Path(path).read_text().splitlines())

def flags(cfg):
    for key in ['CONFIG_MODULE_SIG_FORCE=y', 'CONFIG_MODULE_REL_CRCS=y']:
        if key in cfg:
            raise ValueError('Unsupported configuration: ' + key)
    if 'CONFIG_MODVERSIONS=y' not in cfg or 'CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=y' not in cfg:
        raise ValueError('This kit requires MODVERSIONS and PREL32 exports')
    result = list(FLAGS)
    if 'CONFIG_CFI_CLANG=y' in cfg:
        result.append('-fsanitize=kcfi')
    if 'CONFIG_ARM64_BTI_KERNEL=y' in cfg:
        result.append('-mbranch-protection=bti')
    return result

def includes(kp, build):
    return [f'-I{p}' for p in [build, ROOT/'driver', kp/'kernel', kp/'kernel/include',
        kp/'kernel/patch/include', kp/'kernel/linux/include', kp/'kernel/linux/arch/arm64/include']]

def normalized(path):
    text = Path(path).read_text()
    for old, new in [('uint64_t', 'unsigned long long'), ('int64_t', 'long long'),
                     ('uint32_t', 'unsigned int'), ('int32_t', 'int')]:
        text = re.sub(r'\b'+old+r'\b', new, text)
    return '\n'.join(l for l in text.splitlines() if l.strip() not in
        ['typedef s32 int;', 'typedef u32 unsigned int;', 'typedef u64 unsigned long long;']) + '\n'

def prepare(args):
    b = args.build.resolve()
    if b.exists():
        raise ValueError('Use a new build directory: ' + str(b))
    b.mkdir(parents=True)
    cfg = config(args.target/'config.txt')
    compiler_flags = flags(cfg)
    magic = (args.target/'vermagic.txt').read_text().strip()
    if not magic or '\n' in magic or not magic.startswith('6.1.'):
        raise ValueError('This driver kit currently supports Android ARM64 6.1 only')
    if 'aarch64' not in magic:
        raise ValueError('Target vermagic is not ARM64')
    if args.reference:
        for name in ['vdrm_types.h', 'ko_types.h']:
            (b/name).write_bytes((ROOT/'examples/reference'/name).read_bytes())
    else:
        # Render BTF via an ARM64 ELF container, without a kernel source tree.
        (b/'empty.c').write_text('/* BTF carrier */\n')
        run('clang', '--target=aarch64-linux-gnu', '-c', b/'empty.c', '-o', b/'btf.elf')
        run(args.objcopy, '--add-section', f'.BTF={args.target / "vmlinux.btf"}', b/'btf.elf')
        (b/'all_types.h').write_text(run('pahole', '-F', 'btf', '--compile', b/'btf.elf'))
        roots = ' '.join(l.strip() for l in (ROOT/'tools/roots.txt').read_text().splitlines() if l.strip() and not l.lstrip().startswith('#'))
        common = ['python3', ROOT/'tools/btf2hdr.py', '--btf', args.target/'vmlinux.btf', '--all', b/'all_types.h']
        run(*common, '--roots', roots, '--exclude', ROOT/'tools/exclude.txt', '--out', b/'vdrm_types.h')
        run(*common, '--roots', 'module module_kobject module_attribute mod_arch_specific kobject kref list_head mutex', '--out', b/'ko_types.h')
        for name in ['vdrm_types.h', 'ko_types.h']:
            (b/name).write_text(normalized(b/name))
    # Include source from build directory so generated headers take precedence.
    (b/'vdrm_ko.c').write_bytes((ROOT/'driver/vdrm_ko.c').read_bytes())
    (b/'vdrm_exts.h').write_text(normalized(ROOT/'driver/vdrm_exts.h'))
    shim = (ROOT/'driver/ko_shim.c').read_text()
    shim = re.sub(r'MODINFO\(vermagic,\s*"[^"]*"\);', 'MODINFO(vermagic, '+json.dumps(magic)+');', shim)
    (b/'ko_shim.c').write_text(shim)
    run('clang', *compiler_flags, *includes(args.kp, b), '-c', b/'vdrm_ko.c', '-o', b/'vdrm_ko.o')
    names = need(b/'vdrm_ko.o')
    # module_layout is synthesized by the module loader, not a normal callable function;
    # it is normally present in the export table. A missing entry is fatal.
    anchors = ['fget', 'mutex_lock', 'drm_crtc_vblank_get', 'drm_crtc_vblank_put', 'drm_crtc_send_vblank_event']
    request = sorted(set(names) | set(anchors))
    hdr = 'static const char *names[] = {\n'+''.join('    '+json.dumps(n)+',\n' for n in request)+'};\n'
    hdr += '#define COLLECT_KCFI '+str(int('CONFIG_CFI_CLANG=y' in cfg))+'\n'
    hdr += 'static const char *hash_names[] = {\n'+''.join('    '+json.dumps(n)+',\n' for n in sorted(set(ANCHORS.values())))+'};\n'
    (b/'request.h').write_text(hdr)
    run('clang', *FLAGS, *includes(args.kp, b), '-c', ROOT/'kpm/collector.c', '-o', b/'collector.o')
    run('ld.lld', '-r', '-o', b/'collector.kpm', b/'collector.o')
    rel = run('readelf', '-rW', b/'collector.kpm')
    if 'GOT' in rel or 'JUMP_SLOT' in rel:
        raise ValueError('Unsupported KPM relocation')
    manifest = {'need': names, 'request': request, 'flags': compiler_flags, 'vermagic': magic,
        'object_sha256': digest(b/'vdrm_ko.o'), 'reference': args.reference,
        'target_release': (args.target/'kernel-release.txt').read_text().strip()}
    (b/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('Prepared', b, '\nRun host.sh collect with this directory, then finish.')

def callback_hashes(path):
    d = Path(path).read_bytes()
    shoff = struct.unpack_from('<Q', d, 40)[0]
    ents, count = struct.unpack_from('<HH', d, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', d, shoff+i*ents) for i in range(count)]
    result = {}
    for s in sections:
        if s[1] != 2: continue
        strings = sections[s[6]]
        sd = d[strings[4]:strings[4]+strings[5]]
        for off in range(s[4], s[4]+s[5], 24):
            name, info, other, idx, value, size = struct.unpack_from('<IBBHQQ', d, off)
            if info & 15 != 2 or not idx or idx >= count or value < 4: continue
            nm = sd[name:sd.index(b'\0', name)].decode()
            result[nm] = struct.unpack_from('<I', d, sections[idx][4]+value-4)[0]
    return result

def finish(args):
    b = args.build.resolve()
    m = json.loads((b/'manifest.json').read_text())
    if digest(b/'vdrm_ko.o') != m['object_sha256'] or need(b/'vdrm_ko.o') != m['need']:
        raise ValueError('Object changed since request generation')
    if (args.capture/'kernel-release.txt').read_text().strip() != m['target_release']:
        raise ValueError('Capture from a different kernel release')
    crc = table(args.capture/'crcs.tsv')
    missing = set(m['request']) - crc.keys()
    if missing: raise ValueError('Missing CRCs: '+', '.join(sorted(missing)))
    # At least fget and mutex_lock must match a real target module's __versions.
    anchors = table(args.capture/'anchors.tsv')
    for name in ['module_layout', 'fget', 'mutex_lock']:
        if name not in anchors or crc.get(name) != anchors[name]:
            raise ValueError('Target anchor mismatch: '+name)
    for name in anchors.keys() & crc.keys():
        if anchors[name] != crc[name]: raise ValueError('CRC conflict: '+name)
    rows = ''.join(f'    {{ 0x{crc[n]:08x}UL, "{n}" }},\n' for n in m['need'])
    (b/'versions.c').write_text('struct modversion_info { unsigned long crc; char name[56]; };\n'
        'static const struct modversion_info versions[]\n'
        '__attribute__((section("__versions"), used, aligned(8))) = {\n'+rows+'};\n')
    run('clang', *m['flags'], '-c', b/'ko_shim.c', '-o', b/'ko_shim.o')
    run('clang', *FLAGS, '-c', b/'versions.c', '-o', b/'versions.o')
    output = b/'vdrm.ko'
    run('ld.lld', '-r', '-o', output, b/'vdrm_ko.o', b/'versions.o', b/'ko_shim.o')
    if versions(output) != {n: crc[n] for n in m['need']}: raise ValueError('Linked CRC table mismatch')
    if run('modinfo', '-F', 'vermagic', output).strip() != m['vermagic']: raise ValueError('vermagic mismatch')
    rel = run('readelf', '-rW', output)
    if 'GOT' in rel or 'JUMP_SLOT' in rel: raise ValueError('Forbidden relocation')
    sections = run('readelf', '-SW', output)
    for name in ['__versions', '.modinfo', '.gnu.linkonce.this_module', '.plt', '.init.plt', '.text.ftrace_trampoline']:
        if name not in sections: raise ValueError('Missing section: '+name)
    if any(n.startswith('kf_') for n in need(output)): raise ValueError('KPatch function pointer dependency')
    if '-fsanitize=kcfi' in m['flags']:
        got = callback_hashes(output)
        hashes = table(args.capture/'hashes.tsv')
        for cb, anchor in ANCHORS.items():
            if cb not in got or anchor not in hashes or got[cb] != hashes[anchor]:
                raise ValueError('kCFI mismatch or missing baseline: '+cb)
        for cb, value in [('init_module', 0x36b1c5a6), ('cleanup_module', 0xa540670c)]:
            if got.get(cb) != value: raise ValueError('kCFI builtin prototype mismatch: '+cb)
    print('Built', output, 'CRC count', len(m['need']), 'SHA256', digest(output))
    print('Static subset checked. Other callbacks, BTF ABI and runtime require target validation; no automatic insmod.')

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='stage', required=True)
    a = sub.add_parser('prepare')
    a.add_argument('--kp', type=Path, required=True)
    a.add_argument('--target', type=Path, required=True)
    a.add_argument('--build', type=Path, required=True)
    a.add_argument('--objcopy', default='objcopy')
    a.add_argument('--reference', action='store_true', help='ONLY same-device reproduction using supplied headers')
    a = sub.add_parser('finish')
    a.add_argument('--build', type=Path, required=True)
    a.add_argument('--capture', type=Path, required=True)
    args = p.parse_args()
    try:
        prepare(args) if args.stage == 'prepare' else finish(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        p.exit(1, str(exc)+'\n')
