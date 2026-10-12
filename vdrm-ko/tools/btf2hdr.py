#!/usr/bin/env python3
"""
btf2hdr.py -- 从设备 BTF 精确导出内核类型 C 头

思路（关键）:
  - 依赖关系: 直接解析 BTF 二进制，每个成员/参数都有精确的 type id → 零误伤
  - C 文本  : 仍用 `pahole -F btf --compile <elf>` 的输出（它把函数指针/位域等渲染得正确）
  - 发射顺序: 按 pahole 输出原顺序过滤 → 天然保持全局依赖序

用法:
  python3 btf2hdr.py --btf vmlinux.btf --all all_types.h \
      --roots "drm_driver drm_device ..." [--exclude names.txt] --out vdrm_types.h
"""
import re, struct, sys, argparse


# ---------------- BTF 二进制解析 ----------------
K_INT, K_PTR, K_ARRAY, K_STRUCT, K_UNION, K_ENUM, K_FWD, K_TYPEDEF = 1, 2, 3, 4, 5, 6, 7, 8
K_VOLATILE, K_CONST, K_RESTRICT, K_FUNC, K_FUNC_PROTO, K_VAR, K_DATASEC = 9, 10, 11, 12, 13, 14, 15
K_FLOAT, K_DECL_TAG, K_TYPE_TAG, K_ENUM64 = 16, 17, 18, 19


def parse_btf(path):
    data = open(path, 'rb').read()
    magic, ver, flags, hdr_len, t_off, t_len, s_off, s_len = struct.unpack_from('<HBBIIIII', data, 0)
    if magic != 0xeb9f:
        raise SystemExit('bad BTF magic %#x' % magic)
    t_start = hdr_len + t_off
    t_end = t_start + t_len
    s_start = hdr_len + s_off

    def s(off):
        if not off:
            return ''
        e = data.index(b'\0', s_start + off)
        return data[s_start + off:e].decode('utf-8', 'replace')

    types = [None]  # id 0 = void
    off = t_start
    while off < t_end:
        name_off, info, size_type = struct.unpack_from('<III', data, off)
        off += 12
        vlen = info & 0xffff
        kind = (info >> 24) & 0x1f
        name = s(name_off)
        refs = []
        if kind == K_INT:
            off += 4
        elif kind == K_PTR:
            refs = [size_type]
        elif kind == K_ARRAY:
            t, it, ne = struct.unpack_from('<III', data, off); off += 12
            refs = [t, it]
        elif kind in (K_STRUCT, K_UNION):
            for _ in range(vlen):
                mn, mt, mo = struct.unpack_from('<III', data, off); off += 12
                refs.append(mt)
        elif kind == K_ENUM:
            off += 8 * vlen
        elif kind == K_FWD:
            pass
        elif kind in (K_TYPEDEF, K_VOLATILE, K_CONST, K_RESTRICT, K_TYPE_TAG, K_FUNC):
            refs = [size_type]
        elif kind == K_FUNC_PROTO:
            refs = [size_type]
            for _ in range(vlen):
                pn, pt = struct.unpack_from('<II', data, off); off += 8
                refs.append(pt)
        elif kind == K_VAR:
            off += 4
            refs = [size_type]
        elif kind == K_DATASEC:
            for _ in range(vlen):
                t, o, z = struct.unpack_from('<III', data, off); off += 12
                refs.append(t)
        elif kind == K_FLOAT:
            pass
        elif kind == K_DECL_TAG:
            off += 4
            refs = [size_type]
        elif kind == K_ENUM64:
            off += 12 * vlen
        else:
            raise SystemExit('unknown BTF kind %d at id %d' % (kind, len(types)))
        types.append({'kind': kind, 'name': name, 'refs': refs})
    return types


# ---------------- pahole 文本解析 ----------------
DEF_START = re.compile(r'^(typedef\s+)?(struct|union|enum)\s+([A-Za-z_]\w*)?\s*\{')
FWD_DECL = re.compile(r'^(struct|union|enum)\s+([A-Za-z_]\w*)\s*;$')


def parse_pahole(text):
    lines = text.split('\n')
    items = []
    i, n = 0, len(lines)
    while i < n:
        st = lines[i].strip()
        if not st:
            i += 1
            continue
        m = DEF_START.match(st)
        if m:
            depth, buf = 0, []
            while i < n:
                l = lines[i]
                buf.append(l)
                depth += l.count('{') - l.count('}')
                i += 1
                if depth <= 0:
                    break
            block = '\n'.join(buf)
            if m.group(1):
                mm = re.search(r'\}\s*([A-Za-z_]\w*)\s*;', block)
                items.append(('typedef', mm.group(1) if mm else None, block, m.group(3)))
            else:
                items.append(('def', m.group(3), block, None))
            continue
        m2 = FWD_DECL.match(st)
        if m2:
            items.append(('fwd', m2.group(2), st, None)); i += 1; continue
        if st.startswith('typedef') and st.endswith(';') and '{' not in st:
            # 函数指针 typedef: typedef void (*work_func_t)(...);
            mfp = re.search(r'\(\s*\*?\s*(?:const\s+|volatile\s+)*([A-Za-z_]\w*)\s*\)', st)
            if mfp:
                nm = mfp.group(1)
            else:
                m4 = re.match(r'^typedef\s+.*?([A-Za-z_]\w*)\s*;$', st)
                nm = m4.group(1) if m4 else None
            if nm:
                items.append(('typedef', nm, st, None))
            i += 1; continue
        i += 1
    return items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--btf', required=True)
    ap.add_argument('--all', required=True)
    ap.add_argument('--roots', required=True)
    ap.add_argument('--exclude', default=None)
    ap.add_argument('--out', required=True)
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()

    types = parse_btf(a.btf)
    name2ids = {}
    for tid in range(1, len(types)):
        nm = types[tid]['name']
        if nm:
            name2ids.setdefault(nm, []).append(tid)

    exclude = set()
    if a.exclude:
        for ln in open(a.exclude):
            ln = ln.strip()
            if ln and not ln.startswith('#'):
                exclude.add(ln)

    roots = [r for r in a.roots.split() if r]
    missing = [r for r in roots if r not in name2ids]
    if missing:
        print('WARN roots absent in BTF: ' + ' '.join(missing), file=sys.stderr)

    # 两级闭包: 结构体"按值"使用需要完整定义(DEF); "指针目标 / typedef 底层"只需名字(NAME)
    # 否则 struct device 之类的指针会把整个 PM/调度子系统拖进来
    NEED_NAME, NEED_DEF = 1, 2
    visited = {}
    stack = []

    def push(tid, lvl):
        if tid == 0 or tid >= len(types):
            return
        if types[tid]['kind'] in (K_ENUM, K_ENUM64):
            lvl = NEED_DEF          # C 里 enum 不能前置声明，必须给全定义
        if visited.get(tid, 0) >= lvl:
            return
        visited[tid] = lvl
        stack.append(tid)

    for r in roots:
        for tid in name2ids.get(r, []):
            push(tid, NEED_DEF)

    while stack:
        tid = stack.pop()
        t = types[tid]
        k, nm = t['kind'], t['name']
        if nm and nm in exclude:
            continue
        lvl = visited[tid]
        if k == K_PTR:
            push(t['refs'][0], NEED_NAME)
        elif k in (K_STRUCT, K_UNION):
            # 匿名结构体总是要展开成员：它的 body 会随包裹它的
            # typedef / 父结构体一起被输出，成员名字必须齐全。
            if lvl == NEED_DEF or not nm:
                for r in t['refs']:
                    push(r, NEED_DEF)
        elif k in (K_TYPEDEF, K_CONST, K_VOLATILE, K_RESTRICT, K_TYPE_TAG):
            push(t['refs'][0], lvl)
        elif k == K_ARRAY:
            push(t['refs'][0], NEED_DEF)
        elif k in (K_INT, K_FLOAT, K_FWD, K_ENUM, K_ENUM64):
            pass
        else:
            for r in t['refs']:
                push(r, lvl)

    inc = set(visited)
    # typedef / enum 必须输出定义行（C 不能像 struct 标签那样隐式声明）
    inc_names = set(types[t]['name'] for t in visited
                    if types[t]['name']
                    and (visited[t] == NEED_DEF or types[t]['kind'] in (K_TYPEDEF, K_ENUM, K_ENUM64)))
    if a.verbose:
        print('closure ids=%d names=%d' % (len(inc), len(inc_names)), file=sys.stderr)

    items = parse_pahole(open(a.all, encoding='utf-8', errors='replace').read())
    # 哪些名字有"真定义"
    has_def = set()
    for kind, name, block, tag in items:
        if kind in ('def', 'typedef'):
            if name:
                has_def.add(name)
            if tag:
                has_def.add(tag)

    out = ['/* AUTO-GENERATED by btf2hdr.py — 设备 BTF 精确类型，勿手改 */',
           '/* source: /sys/kernel/btf/vmlinux */',
           '#ifndef _VDRM_BTF_TYPES_H_', '#define _VDRM_BTF_TYPES_H_', '']

    # 先收集 body，再据 body 文本推断需要的前置声明（保证函数指针原型里的标签全局可见）
    union_names = set(types[t]['name'] for t in inc
                      if types[t]['kind'] == K_UNION and types[t]['name'])
    body = []
    n_emit = 0
    for kind, name, block, tag in items:
        nm = name if name else tag
        if not nm or nm in exclude or nm not in inc_names:
            continue
        if kind == 'fwd' and nm in has_def:
            continue          # 有定义就不需要前置声明
        body.append(block)
        n_emit += 1

    body_text = '\n'.join(body)
    tags = sorted(set(m.group(2)
                      for m in re.finditer(r'\b(struct|union)\s+([A-Za-z_]\w*)', body_text)))
    fwd = ['%s %s;' % ('union' if t in union_names else 'struct', t) for t in tags]
    if fwd:
        out.append('/* ---- forward declarations ---- */')
        out.extend(fwd)
        out.append('')
    out.extend(body)
    out.append('#endif')
    open(a.out, 'w').write('\n'.join(out) + '\n')
    print('emitted=%d lines=%d -> %s' % (n_emit, len(out), a.out))


if __name__ == '__main__':
    main()
