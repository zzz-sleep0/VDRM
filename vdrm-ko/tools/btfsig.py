#!/usr/bin/env python3
"""
btfsig.py — 从设备 BTF 提取精确的 C 函数签名

用法:
    python3 btfsig.py <vmlinux.btf> drm_dev_alloc drm_dev_register ...

BTF 里每个 FUNC 都有 FUNC_PROTO（返回值 + 形参类型），所以这里拿到的
是**内核真实的 ABI**，不需要任何内核头文件。
"""
import struct, sys

K = dict(UNKN=0, INT=1, PTR=2, ARRAY=3, STRUCT=4, UNION=5, ENUM=6, FWD=7,
         TYPEDEF=8, VOLATILE=9, CONST=10, RESTRICT=11, FUNC=12, FUNC_PROTO=13,
         VAR=14, DATASEC=15, FLOAT=16, DECL_TAG=17, TYPE_TAG=18, ENUM64=19)
KV = {v: k for k, v in K.items()}


def parse(path):
    b = open(path, 'rb').read()
    magic, ver, flags, hdr_len, type_off, type_len, str_off, str_len = \
        struct.unpack_from('<HBBIIIII', b, 0)
    assert magic == 0xeb9f, 'bad BTF magic %#x' % magic
    tbase = hdr_len + type_off
    sbase = hdr_len + str_off
    strs = b[sbase:sbase + str_len]

    def s(off):
        if off == 0:
            return ''
        e = strs.find(b'\0', off)
        return strs[off:e].decode('utf-8', 'replace')

    types = [None]
    off = tbase
    end = tbase + type_len
    while off < end:
        name_off, info, sz = struct.unpack_from('<III', b, off)
        off += 12
        kind = (info >> 24) & 0x1f
        vlen = info & 0xffff
        kflag = info >> 31
        t = dict(kind=kind, name=s(name_off), size=sz, vlen=vlen, kflag=kflag)
        extra = 0
        if kind == K['INT']:
            extra = 4
        elif kind in (K['STRUCT'], K['UNION']):
            mem = []
            for i in range(vlen):
                mo, mt, moff = struct.unpack_from('<III', b, off + i * 12)
                mem.append((s(mo), mt, moff))
            t['members'] = mem
            extra = vlen * 12
        elif kind == K['ENUM']:
            extra = vlen * 8
        elif kind == K['ENUM64']:
            extra = vlen * 12
        elif kind == K['FUNC_PROTO']:
            ps = []
            for i in range(vlen):
                po, pt = struct.unpack_from('<II', b, off + i * 8)
                ps.append((s(po), pt))
            t['params'] = ps
            extra = vlen * 8
        elif kind == K['ARRAY']:
            et, it, ne = struct.unpack_from('<III', b, off)
            t['elem'], t['nelems'] = et, ne
            extra = 12
        elif kind == K['DATASEC']:
            extra = vlen * 12
        elif kind == K['VAR']:
            extra = 4
        elif kind == K['DECL_TAG']:
            extra = 4
        if kind in (K['PTR'], K['TYPEDEF'], K['VOLATILE'], K['CONST'],
                    K['RESTRICT'], K['FUNC'], K['FUNC_PROTO'], K['VAR'],
                    K['TYPE_TAG']):
            t['type'] = sz      # sz==0 表示 void，也要记下来
        types.append(t)
        off += extra
    return types


def tname(types, tid):
    """类型的"基名"，用于判断需不需要加 * / 空格"""
    t = types[tid]
    k = t['kind']
    if k == K['INT'] or k == K['FLOAT']:
        return t['name']
    if k == K['TYPEDEF']:
        return t['name']
    if k == K['STRUCT']:
        return 'struct ' + (t['name'] or '<anon>')
    if k == K['UNION']:
        return 'union ' + (t['name'] or '<anon>')
    if k == K['ENUM']:
        return 'enum ' + (t['name'] or '<anon>')
    if k == K['ENUM64']:
        return 'enum ' + (t['name'] or '<anon>')
    if k == K['FWD']:
        return ('struct ' if t['size'] == 0 else 'union ') + t['name']
    if k == K['PTR']:
        return tname(types, t['type'])
    if k == K['CONST']:
        return tname(types, t['type'])
    if k == K['VOLATILE']:
        return tname(types, t['type'])
    return '?%s' % KV.get(k, k)


def decl(types, tid, name=''):
    if tid == 0:
        return ('void ' + name).strip()
    t = types[tid]
    k = t['kind']
    if k in (K['INT'], K['FLOAT'], K['TYPEDEF'], K['STRUCT'], K['UNION'],
             K['ENUM'], K['ENUM64'], K['FWD']):
        base = tname(types, tid)
        return (base + ' ' + name).strip()
    if k == K['CONST']:
        return 'const ' + decl(types, t['type'], name)
    if k == K['VOLATILE']:
        return 'volatile ' + decl(types, t['type'], name)
    if k == K['TYPE_TAG']:          # __user / __rcu 等注解：对 C 类型透明
        return decl(types, t['type'], name)
    if k == K['PTR']:
        it = t.get('type', 0)
        if it and types[it]['kind'] == K['FUNC_PROTO']:
            return proto(types, types[it], name, as_ptr=True)
        return decl(types, it, '*' + name)
    if k == K['ARRAY']:
        return decl(types, t['elem'], '%s[%d]' % (name, t['nelems']))
    if k == K['FUNC_PROTO']:
        return proto(types, t, name)
    return '/*tid=%d kind=%s*/ %s' % (tid, KV.get(k, k), name)


def proto(types, p, name, as_ptr=False):
    ret = decl(types, p['type']) if p.get('type') else 'void'
    ps = []
    for pn, pt in p.get('params', []):
        s = decl(types, pt, pn)
        if s.startswith('void') and not pn:
            continue
        ps.append(s)
    body = ', '.join(ps) if ps else 'void'
    if as_ptr:
        return '%s (*%s)(%s)' % (ret, name, body)
    return '%s %s(%s)' % (ret, name, body)


def main():
    args = sys.argv[1:]
    table = None
    if '--table' in args:
        i = args.index('--table')
        table = args[i + 1]
        del args[i:i + 2]
    if len(args) < 2:
        print(__doc__)
        return 2
    types = parse(args[0])
    want = args[1:]
    found = {}
    for tid, t in enumerate(types):
        if t and t['kind'] == K['FUNC'] and t['name'] in want:
            p = types[t['type']]
            found.setdefault(t['name'], []).append((tid, p))
    if table:
        print('struct %s {' % table)
        for w in want:
            if w not in found:
                print('\t/* MISSING: %s */' % w)
                continue
            tid, p = found[w][0]
            print('\t' + proto(types, p, w, as_ptr=True) + ';')
        print('};')
        return 0
    for w in want:
        if w not in found:
            print('/* NOT FOUND: %s */' % w)
            continue
        for tid, p in found[w]:
            print(proto(types, p, w) + ';')
    return 0


if __name__ == '__main__':
    sys.exit(main())
