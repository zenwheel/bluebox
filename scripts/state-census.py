"""Inventory of the mutable static and global state of Blue Box's C targets,
read from the DWARF of a debug build's object files (the survey for
hibernating a program: what a snapshot of the Toolbox's state holds).

usage: python3 -I scripts/state-census.py OUT.tsv TARGET=dir [TARGET=dir ...]
  e.g. after `swift build`, with O=.build/out/Intermediates.noindex/bluebox.build/Debug:
  python3 -I scripts/state-census.py state.tsv Toolbox=$O/Toolbox-t.build/Objects-normal/arm64 \
      CCore=$O/CCore-t.build/Objects-normal/arm64 ...

One row per variable that is not const at its top: file, symbol (a static
local as function.name), scope (global, static, local, tls), type, size in
bytes and the kinds of field it holds, counted per element of arrays:
  fn    function pointers (into the binary)
  cstr  const char pointers (string literals, mostly)
  cptr  other pointers to const (tables in the binary, mostly)
  heap  pointers to mutable data (malloc'd, mostly)
  vptr  void pointers (a host callback's context, mostly)
  host  library handles: FILE, CF and Objective-C objects, sqlite, CGL ...
  fd    integers named as descriptors (fd, sock ...)
  sync  pthread mutexes and condition variables
  data  everything else (a byte or word array counts once)
  reach_*  a kind found in a struct reached through a heap pointer
"""
import re, subprocess, sys, os, collections

DIE = re.compile(r'^0x([0-9a-f]+):(\s+)(DW_TAG_\w+|NULL)')
ATTR = re.compile(r'^\s+(DW_AT_\w+)\s+\((.*)\)\s*$')
REF = re.compile(r'^0x([0-9a-f]+)(?: "(.*)")?$')


def parse(path):
    out = subprocess.run(['dwarfdump', '--debug-info', path], capture_output=True, text=True).stdout
    dies = {}
    stack = []  # (indent, die)
    cur = None
    for line in out.splitlines():
        m = DIE.match(line)
        if m:
            off, ind, tag = int(m.group(1), 16), len(m.group(2)), m.group(3)
            if tag == 'NULL':
                while stack and stack[-1][0] >= ind:
                    stack.pop()
                cur = None
                continue
            while stack and stack[-1][0] >= ind:
                stack.pop()
            d = {'off': off, 'tag': tag, 'kids': [], 'parent': stack[-1][1] if stack else None}
            if stack:
                stack[-1][1]['kids'].append(d)
            stack.append((ind, d))
            dies[off] = d
            cur = d
            continue
        if cur is not None:
            a = ATTR.match(line)
            if a:
                cur[a.group(1)] = a.group(2)
    return dies


def tref(d):
    v = d.get('DW_AT_type')
    if not v:
        return None
    m = REF.match(v)
    return int(m.group(1), 16) if m else None


def name(d):
    n = d.get('DW_AT_name')
    return n.strip('"') if n else None


HOST_NAMES = re.compile(
    r'(__sFILE|^FILE$|^DIR$|__CF|CF\w+Ref|_opaque_pthread|pthread_t$|dispatch_(queue|source|group|semaphore|object|data|io)|OS_dispatch|'
    r'AudioComponent|OpaqueAudio|AudioQueue|AURenderCallback|ComponentInstance|AudioUnit|'
    r'sqlite3|objc_object|^id$|NSObject|MTL|CGContext|CGImage|CGColor|CGPath|CVPixel|CVBuffer|'
    r'VTDecompression|CMFormat|CMSample|CMBlock|mach_port|io_object|IONotification|'
    r'z_stream|internal_state|UConverter|UBreak|ifaddrs|addrinfo|xpc_|NSWindow|NSView|_IO_FILE|'
    r'CGL\w+Obj|_CGLContext|_CGLPixel|_CGLPBuffer|SecTrust|SSL|regex_t|iconv_t|_xmlDoc)')
SYNC_NAMES = re.compile(r'(pthread_mutex|pthread_cond|pthread_once|pthread_rwlock|os_unfair_lock|'
                        r'_opaque_pthread_mutex_t|_opaque_pthread_cond_t|dispatch_once|semaphore_t|'
                        r'_opaque_pthread_once|_opaque_pthread_rwlock)')
FD_FIELD = re.compile(r'((^|_)(sock|socket|sockfd|kq|pipe)(s|_|$))|fds?$|^fd', re.I)


class Classifier:
    def __init__(self, dies):
        self.dies = dies
        self.size_cache = {}

    def strip(self, off):
        """Follow typedef/cv chains; return (die, typedef names seen, is_const)."""
        names, const = [], False
        seen = 0
        while off is not None and seen < 64:
            seen += 1
            d = self.dies.get(off)
            if d is None:
                return None, names, const
            t = d['tag']
            if t == 'DW_TAG_typedef':
                names.append(name(d) or '')
                off = tref(d)
            elif t == 'DW_TAG_const_type':
                const = True
                off = tref(d)
            elif t in ('DW_TAG_volatile_type', 'DW_TAG_restrict_type', 'DW_TAG_atomic_type'):
                off = tref(d)
            else:
                return d, names, const
        return None, names, const

    def size(self, off):
        d, _, _ = self.strip(off)
        if d is None:
            return 0
        if 'DW_AT_byte_size' in d:
            return int(d['DW_AT_byte_size'], 16)
        if d['tag'] == 'DW_TAG_array_type':
            return self.size(tref(d)) * self.count(d)
        if d['tag'] in ('DW_TAG_pointer_type', 'DW_TAG_subroutine_type'):
            return 8
        return 0

    def count(self, arr):
        n = 1
        for k in arr['kids']:
            if k['tag'] == 'DW_TAG_subrange_type':
                c = k.get('DW_AT_count')
                if c is None and 'DW_AT_upper_bound' in k:
                    c = hex(int(k['DW_AT_upper_bound'], 16) + 1)
                n *= int(c, 16) if c else 0
        return n

    def typename(self, off, depth=0):
        if off is None:
            return 'void'
        d = self.dies.get(off)
        if d is None or depth > 8:
            return '?'
        t = d['tag']
        if t == 'DW_TAG_typedef' or t == 'DW_TAG_base_type':
            return name(d)
        if t == 'DW_TAG_const_type':
            return 'const ' + self.typename(tref(d), depth + 1)
        if t in ('DW_TAG_volatile_type', 'DW_TAG_atomic_type', 'DW_TAG_restrict_type'):
            return self.typename(tref(d), depth + 1)
        if t == 'DW_TAG_pointer_type':
            return self.typename(tref(d), depth + 1) + '*'
        if t == 'DW_TAG_structure_type':
            return 'struct ' + (name(d) or '<anon>')
        if t == 'DW_TAG_union_type':
            return 'union ' + (name(d) or '<anon>')
        if t == 'DW_TAG_enumeration_type':
            return 'enum ' + (name(d) or '<anon>')
        if t == 'DW_TAG_array_type':
            return self.typename(tref(d), depth + 1) + ''.join(
                '[%d]' % (int(k.get('DW_AT_count', '0'), 16) if 'DW_AT_count' in k else
                          int(k.get('DW_AT_upper_bound', '-1'), 16) + 1)
                for k in d['kids'] if k['tag'] == 'DW_TAG_subrange_type')
        if t == 'DW_TAG_subroutine_type':
            return 'fn()'
        return t

    def kinds(self, off, field, acc, mult=1, depth=0):
        """Accumulate pointer-kind counts for a value of type off."""
        if depth > 12:
            return
        d, names, _ = self.strip(off)
        if d is None:
            acc['data'] += mult
            return
        for n in names:
            if SYNC_NAMES.search(n):
                acc['sync'] += mult
                return
            if HOST_NAMES.search(n):
                acc['host'] += mult
                return
        t = d['tag']
        nm = name(d) or ''
        if t in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
            if SYNC_NAMES.search(nm):
                acc['sync'] += mult
                return
            if HOST_NAMES.search(nm):
                acc['host'] += mult
                return
            mems = [k for k in d['kids'] if k['tag'] == 'DW_TAG_member']
            if t == 'DW_TAG_union_type':
                sub = collections.Counter()
                for k in mems:
                    self.kinds(tref(k), name(k) or '', sub, mult, depth + 1)
                # a union of pointer and data counts once, as its widest pointer kind
                if sub:
                    for kk in ('fn', 'host', 'heap', 'cstr', 'cptr', 'vptr'):
                        if sub[kk]:
                            acc[kk] += mult
                            acc['union'] += mult
                            return
                    acc['data'] += mult
                return
            if not mems:
                acc['data'] += mult
            for k in mems:
                self.kinds(tref(k), name(k) or '', acc, mult, depth + 1)
            return
        if t == 'DW_TAG_array_type':
            n = self.count(d)
            et, _, _ = self.strip(tref(d))
            if et is not None and et['tag'] == 'DW_TAG_base_type':
                acc['data'] += mult  # a byte/word buffer counts as one data field
                return
            self.kinds(tref(d), field, acc, mult * max(n, 1), depth + 1)
            return
        if t == 'DW_TAG_pointer_type':
            pt = tref(d)
            p, pnames, pconst = self.strip(pt)
            if p is None:
                acc['vptr'] += mult  # void *
                return
            for n in pnames:
                if HOST_NAMES.search(n):
                    acc['host'] += mult
                    return
            pn = name(p) or ''
            if p['tag'] == 'DW_TAG_subroutine_type':
                acc['fn'] += mult
            elif HOST_NAMES.search(pn) or p['tag'] == 'DW_TAG_ptr_to_member_type':
                acc['host'] += mult
            elif p['tag'] == 'DW_TAG_base_type' and pn in ('char', 'signed char', 'unsigned char'):
                acc['cstr' if pconst else 'heap'] += mult
            elif p['tag'] in ('DW_TAG_structure_type',) and p.get('DW_AT_declaration') and not pnames \
                    and pn.startswith('_'):
                acc['host'] += mult  # an opaque struct pointer from a library
            elif pconst:
                acc['cptr'] += mult
            else:
                acc['heap'] += mult
                if p['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type') and p['off'] not in self.visiting:
                    self.visiting.add(p['off'])
                    sub = collections.Counter()
                    self.kinds(pt, field, sub, 1, depth + 1)
                    for kk, vv in sub.items():
                        if kk in ('fn', 'host', 'fd', 'sync', 'heap'):
                            self.reach[kk] += 1
                        if kk.startswith('r_'):
                            self.reach[kk[2:]] += vv
                    self.visiting.discard(p['off'])
            return
        if t == 'DW_TAG_base_type' or t == 'DW_TAG_enumeration_type':
            if FD_FIELD.search(field) and t == 'DW_TAG_base_type' and 'int' in nm:
                acc['fd'] += mult
            else:
                acc['data'] += mult
            return
        acc['data'] += mult


def location_kind(v):
    loc = v.get('DW_AT_location', '')
    if 'DW_OP_addr' in loc:
        return 'static'
    if 'tls' in loc.lower() or 'DW_OP_const8u' in loc:
        return 'tls'
    return None


def main():
    out = sys.argv[1]
    rows = []
    for arg in sys.argv[2:]:
        target, d = arg.split('=', 1)
        for f in sorted(os.listdir(d)):
            if not f.endswith('.o'):
                continue
            dies = parse(os.path.join(d, f))
            c = Classifier(dies)
            for v in dies.values():
                if v['tag'] != 'DW_TAG_variable' or v.get('DW_AT_declaration'):
                    continue
                lk = location_kind(v)
                if lk is None or not name(v):
                    continue
                df = v.get('DW_AT_decl_file', '').strip('"')
                if '/Sources/' not in df:
                    continue
                td, _, tconst = c.strip(tref(v))
                if tconst:
                    continue  # const at the top: lives in __TEXT/__DATA_CONST
                # an array of const elements is const too
                if td is not None and td['tag'] == 'DW_TAG_array_type':
                    _, _, econst = c.strip(tref(td))
                    if econst:
                        continue
                scope = 'global' if v.get('DW_AT_external') else 'static'
                p = v['parent']
                if p is not None and p['tag'] in ('DW_TAG_subprogram', 'DW_TAG_lexical_block'):
                    scope = 'local'
                    while p is not None and p['tag'] != 'DW_TAG_subprogram':
                        p = p['parent']
                    fn = name(p) if p else '?'
                    sym = '%s.%s' % (fn, name(v))
                else:
                    sym = name(v)
                if lk == 'tls':
                    scope = 'tls'
                acc = collections.Counter()
                c.visiting = set()
                c.reach = collections.Counter()
                c.kinds(tref(v), name(v) or '', acc)
                for kk, vv in c.reach.items():
                    acc['reach_' + kk] = 1
                kinds = ' '.join('%s:%d' % (k, acc[k]) for k in
                                 ('fn', 'cstr', 'cptr', 'heap', 'vptr', 'host', 'fd', 'sync', 'union', 'data', 'reach_fn', 'reach_host', 'reach_fd', 'reach_sync') if acc[k])
                rel = df.split('/Sources/', 1)[1]
                rows.append((rel, str(sym), scope, str(c.typename(tref(v))), c.size(tref(v)), kinds,
                             v.get('DW_AT_decl_line', '')))
    seen = set()
    with open(out, 'w') as fh:
        fh.write('file\tsymbol\tscope\ttype\tsize\tpointer_kinds\tline\n')
        for r in sorted(rows):
            key = (r[0], r[1])
            if key in seen:
                continue
            seen.add(key)
            fh.write('\t'.join(str(x) for x in r) + '\n')
    print(len(seen), 'variables')


if __name__ == '__main__':
    main()
