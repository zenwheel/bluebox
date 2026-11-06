"""The pointer map a snapshot needs (Sources/Toolbox/statemap_gen.c): for
every mutable static of the C targets that a snapshot saves, and for every
type a tracked block is tagged with (BB_TAG and BB_HEAD in the sources),
where its pointers are.  Read from the DWARF of a build's object files and
their symbol tables (which section each static is in).

usage: python3 -I scripts/state-map.py [--check] [BUILD_DIR]
  BUILD_DIR is a configuration's intermediates, by default the debug
  build's: .build/out/Intermediates.noindex/bluebox.build/Debug (after
  `swift build` or `swift test`).  A debug build has every static (a
  release build drops some it does not need); the layouts are the same.  --check writes nothing, and fails if the file would
  change or a guard below fails.

Each field is one of
  P  a pointer to data: a tracked block, a static, the binary, the guest's
     memory (which, is told by its value when saved)
  F  a function pointer (the binary, or a library)
  U  a pointer in a union with data: a pointer only if its value is one
  H  a library's handle (FILE, CF, sqlite, Metal ...): the host's state
  Y  a lock or condition variable (the host's)
  D  a file descriptor (the host's)
  I  an integer as wide as a pointer: data, but where a pointer the map
     does not know of could hide (the scan looks there)
H, Y and D belong in the host's section (BB_HOST); in a saved static they
fail the check unless scripts/state-allow.txt names the static, with why.
The check also fails for a static the compiler left in plain __data, __bss
or __common (a file that missed the state header), and for a tag whose
type the DWARF does not have.
"""
import collections, importlib.util, os, re, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
TARGETS = ['CCore', 'CPU68K', 'CPUPPC', 'ForkStore', 'Toolbox']
OUT = os.path.join(ROOT, 'Sources', 'Toolbox', 'statemap_gen.c')
ALLOW = os.path.join(ROOT, 'scripts', 'state-allow.txt')

spec = importlib.util.spec_from_file_location('census', os.path.join(ROOT, 'scripts', 'state-census.py'))
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)
name, tref = census.name, census.tref

SAVED = {'__bbdata', '__bbbss'}
HOSTSECT = {'__bbhost', '__bbhostz'}
PLAIN = {'__data', '__bss', '__common'}
NMLINE = re.compile(r'^([0-9a-f]+) \((\w+),(\w+)\) (?:non-)?external (?:\[[^\]]*\] )?(\S+)')


def symbols(obj):
    """address -> (section, symbol) for the object's data symbols."""
    out = subprocess.run(['nm', '-m', obj], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        m = NMLINE.match(line)
        if m and m.group(2) == '__DATA':
            syms[int(m.group(1), 16)] = (m.group(3), m.group(4))
    return syms


def section_ends(obj):
    """section -> its end address, for the object's __DATA sections."""
    lc = subprocess.run(['otool', '-l', obj], capture_output=True, text=True).stdout
    ends = {}
    for m in re.finditer(r'sectname (\w+)\s+segname __DATA\s+addr 0x([0-9a-f]+)\s+size 0x([0-9a-f]+)', lc):
        ends[m.group(1)] = int(m.group(2), 16) + int(m.group(3), 16)
    return ends


class Layouts:
    """Flattens a type into runs: (offset, kind, count, stride, count2, stride2)."""

    # A struct or union by tag and name -> (Layouts, offset) of a complete
    # definition: with clang modules an object may have only a declaration
    # of a type from a module's header, the definition being elsewhere.
    complete = {}

    def __init__(self, dies):
        self.dies = dies
        self.c = census.Classifier(dies)
        self.memo = {}

    def index(self):
        for d in self.dies.values():
            if d['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type') and name(d) and \
                    not d.get('DW_AT_declaration') and 'DW_AT_byte_size' in d:
                Layouts.complete.setdefault((d['tag'], name(d)), (self, d['off']))
            elif d['tag'] == 'DW_TAG_typedef' and name(d):
                t, _, _ = self.c.strip(tref(d))
                if t is not None and t['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type') and \
                        not t.get('DW_AT_declaration') and 'DW_AT_byte_size' in t:
                    Layouts.complete.setdefault(('typedef', name(d)), (self, t['off']))

    def resolve(self, off):
        """(Layouts, offset) of the type at off, a declaration replaced by
        its definition (by its tag's name, or an anonymous one's typedef's)."""
        d, names, _ = self.c.strip(off)
        if d is not None and d['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type') and \
                d.get('DW_AT_declaration'):
            other = Layouts.complete.get((d['tag'], name(d))) if name(d) else None
            for n in names:
                other = other or Layouts.complete.get(('typedef', n))
            if other:
                return other
        return self, off

    def size(self, off):
        lay, o = self.resolve(off)
        return lay.c.size(o)

    def runs(self, off, field='', depth=0):
        key = (off, field if field and census.FD_FIELD.search(field) else '')
        if key in self.memo:
            return self.memo[key]
        r = self._runs(off, field, depth)
        self.memo[key] = r
        return r

    def _runs(self, off, field, depth):
        if depth > 16:
            return []
        d, names, _ = self.c.strip(off)
        if d is None:
            return []
        if d['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type') and d.get('DW_AT_declaration'):
            lay, o = self.resolve(off)
            return lay.runs(o, field, depth + 1) if lay is not self else []
        for n in names:
            if census.SYNC_NAMES.search(n):
                return [(0, 'Y', 1, 0, 1, 0)]
            if census.HOST_NAMES.search(n):
                return [(0, 'H', 1, 0, 1, 0)]
        t = d['tag']
        nm = name(d) or ''
        if t in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
            if census.SYNC_NAMES.search(nm):
                return [(0, 'Y', 1, 0, 1, 0)]
            if census.HOST_NAMES.search(nm):
                return [(0, 'H', 1, 0, 1, 0)]
            out = []
            for k in d['kids']:
                if k['tag'] != 'DW_TAG_member' or 'DW_AT_bit_size' in k:
                    continue
                loc = k.get('DW_AT_data_member_location', '0x0')
                try:
                    mo = int(loc, 16)
                except ValueError:
                    continue
                for r in self.runs(tref(k), name(k) or '', depth + 1):
                    kind = r[1]
                    if t == 'DW_TAG_union_type' and kind in ('P', 'F'):
                        kind = 'U'
                    out.append((r[0] + mo, kind) + r[2:])
            return sorted(set(out))
        if t == 'DW_TAG_array_type':
            n = self.c.count(d)
            et, _, _ = self.c.strip(tref(d))
            if n == 0 or et is None or et['tag'] == 'DW_TAG_enumeration_type':
                return []
            if et['tag'] == 'DW_TAG_base_type':
                return [(0, 'I', n, 8, 1, 0)] if wide_int(et) else []
            esz = self.size(tref(d))
            inner = self.runs(tref(d), field, depth + 1)
            out = []
            for r in inner:
                if r[2] == 1 and r[4] == 1:
                    out.append((r[0], r[1], n, esz, 1, 0))
                elif r[4] == 1:
                    out.append((r[0], r[1], n, esz, r[2], r[3]))
                else:
                    for i in range(n):
                        out.append((r[0] + i * esz,) + r[1:])
            return out
        if t == 'DW_TAG_pointer_type':
            p, pnames, _ = self.c.strip(tref(d))
            if p is None:
                return [(0, 'P', 1, 0, 1, 0)]  # void *
            for n in pnames:
                if census.HOST_NAMES.search(n):
                    return [(0, 'H', 1, 0, 1, 0)]
            pn = name(p) or ''
            if p['tag'] == 'DW_TAG_subroutine_type':
                return [(0, 'F', 1, 0, 1, 0)]
            if census.HOST_NAMES.search(pn):
                return [(0, 'H', 1, 0, 1, 0)]
            if p['tag'] == 'DW_TAG_structure_type' and p.get('DW_AT_declaration') and not pnames \
                    and pn.startswith('_'):
                return [(0, 'H', 1, 0, 1, 0)]  # a library's opaque struct
            return [(0, 'P', 1, 0, 1, 0)]
        if t == 'DW_TAG_base_type':
            if field and census.FD_FIELD.search(field) and 'int' in nm:
                return [(0, 'D', 1, 0, 1, 0)]
            return [(0, 'I', 1, 0, 1, 0)] if wide_int(d) else []
        return []


def wide_int(d):
    """An integer wide enough to hold a pointer (long, uint64_t, uintptr_t ...)."""
    return d.get('DW_AT_byte_size') == '0x08' and d.get('DW_AT_encoding') in ('DW_ATE_signed', 'DW_ATE_unsigned')


def cu_file(dies):
    for d in dies.values():
        if d['tag'] == 'DW_TAG_compile_unit':
            n = (name(d) or '')
            return n.split('/Sources/', 1)[1] if '/Sources/' in n else n
    return None


def var_symbol(v):
    p = v['parent']
    if p is not None and p['tag'] in ('DW_TAG_subprogram', 'DW_TAG_lexical_block'):
        while p is not None and p['tag'] != 'DW_TAG_subprogram':
            p = p['parent']
        return '%s.%s' % (name(p) if p else '?', name(v))
    return name(v)


def find_type(objs, file, tname):
    """The DIE offset and object of the type a tag names, looked up in the
    file's own object first."""
    m = re.match(r'^(struct|union|enum)\s+(\w+)$', tname)
    want_tag = {'struct': 'DW_TAG_structure_type', 'union': 'DW_TAG_union_type',
                'enum': 'DW_TAG_enumeration_type'}.get(m.group(1)) if m else None
    want = m.group(2) if m else tname
    own = [o for o in objs if o['file'] and os.path.basename(o['file']) == file]
    rest = [o for o in objs if o not in own]
    for o in own + rest:
        for d in o['dies'].values():
            if name(d) != want or d.get('DW_AT_declaration'):
                continue
            if want_tag and d['tag'] == want_tag:
                return o, d['off']
            if not want_tag and d['tag'] in ('DW_TAG_typedef', 'DW_TAG_base_type'):
                return o, d['off']
    return None, None


def tag_runs(objs, file, tname):
    """(size, runs) for a tag's type, or None."""
    t = tname.strip()
    if re.search(r'\(\s*\*', t):
        return 8, [(0, 'F', 1, 0, 1, 0)]
    if t.endswith('*'):
        base = t.rstrip('*').strip()
        if census.HOST_NAMES.search(base):
            return 8, [(0, 'H', 1, 0, 1, 0)]
        return 8, [(0, 'P', 1, 0, 1, 0)]
    m = re.match(r'^(.*?)\s*\[(\d+)\]$', t)
    if m:
        r = tag_runs(objs, file, m.group(1))
        if r is None:
            return None
        esz, inner = r
        n = int(m.group(2))
        out = []
        for x in inner:
            if x[2] == 1 and x[4] == 1:
                out.append((x[0], x[1], n, esz, 1, 0))
            else:
                for i in range(n):
                    out.append((x[0] + i * esz,) + x[1:])
        return esz * n, out
    o, off = find_type(objs, file, t)
    if o is None:
        return None
    lay = o['layouts']
    return lay.size(off), lay.runs(off)


TAGRE = re.compile(r'\bBB_(TAG|HEAD)\(((?:[^()]|\([^()]*\))*)\)')


def source_tags():
    """(file basename, type) for every BB_TAG and BB_HEAD in the sources."""
    tags = set()
    bases = collections.Counter()
    for t in TARGETS:
        for dp, _, fn in os.walk(os.path.join(ROOT, 'Sources', t)):
            for f in fn:
                if not f.endswith(('.c', '.h', '.m')):
                    continue
                bases[f] += 1
                if f == 'bbmem.h':
                    continue
                with open(os.path.join(dp, f), encoding='latin-1') as fh:
                    text = fh.read()
                for m in TAGRE.finditer(text):
                    tags.add((f, re.sub(r'\s+', ' ', m.group(2).strip())))
    dup = sorted(b for b, n in bases.items() if n > 1)
    return tags, dup


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    build = args[0] if args else os.path.join(ROOT, '.build', 'out', 'Intermediates.noindex',
                                                 'bluebox.build', 'Debug')
    problems = []
    objs = []
    for t in TARGETS:
        d = os.path.join(build, '%s-t.build' % t, 'Objects-normal')
        arch = sorted(os.listdir(d))[0] if os.path.isdir(d) else None
        if arch is None:
            sys.exit('state-map: no objects in %s (build first)' % d)
        od = os.path.join(d, arch)
        for f in sorted(os.listdir(od)):
            if f.endswith('.o'):
                dies = census.parse(os.path.join(od, f))
                objs.append({'target': t, 'obj': f, 'dies': dies, 'file': cu_file(dies),
                             'syms': symbols(os.path.join(od, f)), 'ends': section_ends(os.path.join(od, f)),
                             'layouts': Layouts(dies)})
    for o in objs:
        o['layouts'].index()

    allow = {}
    if os.path.exists(ALLOW):
        with open(ALLOW) as fh:
            for line in fh:
                line = line.rstrip('\n')
                if line and not line.startswith('#'):
                    parts = line.split('\t')
                    allow[(parts[0], parts[1])] = parts[2] if len(parts) > 2 else ''

    statics = {}  # (file, symbol) -> (size, runs)
    nhost = 0
    for o in objs:
        for v in o['dies'].values():
            if v['tag'] != 'DW_TAG_variable' or v.get('DW_AT_declaration') or not name(v):
                continue
            loc = v.get('DW_AT_location', '')
            m = re.search(r'DW_OP_addr 0x([0-9a-f]+)', loc)
            if not m:
                continue
            addr = int(m.group(1), 16)
            if addr not in o['syms']:
                continue  # constant, or not ours
            sect, msym = o['syms'][addr]
            sym = var_symbol(v)
            file = o['file']
            if sect in PLAIN:
                problems.append('%s: %s is in plain %s (the state header was not included)' % (file, sym, sect))
                continue
            if sect in HOSTSECT:
                nhost += 1
                continue
            if sect not in SAVED:
                continue
            size = o['layouts'].size(tref(v))
            if not size:
                # A type the DWARF has nowhere (a system header's): as far
                # as the next symbol in the section.
                later = sorted(a for a, (sc, _) in o['syms'].items() if sc == sect and a > addr)
                size = (later[0] if later else o['ends'].get(sect, addr)) - addr
                if not size:
                    problems.append('%s: the size of %s is not known' % (file, sym))
            runs = o['layouts'].runs(tref(v), name(v))
            key = (file, sym)
            if key in statics and statics[key] != (size, runs):
                problems.append('%s: two statics named %s with different types: rename one' % (file, sym))
            statics[key] = (size, runs)
            host = sorted({r[1] for r in runs} & {'H', 'Y', 'D'})
            if host and key not in allow:
                problems.append('%s: %s holds the host\'s state (%s) but is saved: mark it BB_HOST, '
                                'or list it in scripts/state-allow.txt' % (file, sym, ''.join(host)))

    tags, dupbases = source_tags()
    typed = {}
    for f, t in sorted(tags):
        if f in dupbases:
            problems.append('%s: two source files have this name; a tag cannot tell them apart' % f)
        r = tag_runs(objs, f, t)
        if r is None:
            problems.append('%s: the DWARF has no type %s (tagged with BB_TAG/BB_HEAD)' % (f, t))
            continue
        typed[(f, t)] = r

    # Layouts, shared.
    layouts = {(): 0}
    order = [()]

    def lay(runs):
        k = tuple(runs)
        if k not in layouts:
            layouts[k] = len(order)
            order.append(k)
        return layouts[k]

    srows = [(f, s, size, lay(runs)) for (f, s), (size, runs) in sorted(statics.items())]
    trows = [(f, t, size, lay(runs)) for (f, t), (size, runs) in sorted(typed.items())]

    out = ['/* Generated by scripts/state-map.py from the DWARF of a build: do not edit.',
           ' * Where the pointers are in each saved static and each tagged type',
           ' * (hibernate.c); hb_check_map reports a map older than the code. */',
           '#include "hibernate_map.h"', '',
           'static const hb_run runs[] = {']
    first = []
    n = 0
    for k in order:
        first.append(n)
        for r in k:
            out.append('    { %d, \'%s\', %d, %d, %d, %d },' % r)
            n += 1
    if n == 0:
        out.append('    { 0, 0, 0, 0, 0, 0 },')
    out.append('};')
    out.append('')
    out.append('const hb_layout hb_layouts[] = {')
    for i, k in enumerate(order):
        out.append('    { %d, %d },' % (first[i], len(k)))
    out.append('};')
    out.append('const hb_run *const hb_runs = runs;')
    out.append('const unsigned hb_nlayouts = %d;' % len(order))
    out.append('')
    out.append('/* By file and symbol (a static local as function.name). */')
    out.append('const hb_static hb_statics[] = {')
    for f, s, size, li in srows:
        out.append('    { %s, %s, %d, %d },' % (cstr(f), cstr(s), size, li))
    out.append('};')
    out.append('const unsigned hb_nstatics = %d;' % len(srows))
    out.append('')
    out.append('/* By the file the tag is written in and the type as written. */')
    out.append('const hb_typemap hb_types[] = {')
    for f, t, size, li in trows:
        out.append('    { %s, %s, %d, %d },' % (cstr(f), cstr(t), size, li))
    out.append('};')
    out.append('const unsigned hb_ntypes = %d;' % len(trows))
    text = '\n'.join(out) + '\n'

    kinds = collections.Counter()
    for _, (size, runs) in statics.items():
        for r in runs:
            kinds[r[1]] += r[2] * r[4]
    print('state-map: %d saved statics (%d with pointers, %s), %d in the host\'s sections, '
          '%d tagged types, %d layouts' % (len(srows), sum(1 for v in statics.values() if any(r[1] in 'PFUH' for r in v[1])),
                                          ' '.join('%s:%d' % kv for kv in sorted(kinds.items())),
                                          nhost, len(trows), len(order)), file=sys.stderr)
    for p in problems:
        print('state-map: ' + p, file=sys.stderr)
    old = open(OUT).read() if os.path.exists(OUT) else ''
    if check:
        if old != text:
            print('state-map: %s is out of date (run scripts/state-map.py)' % os.path.relpath(OUT, ROOT),
                  file=sys.stderr)
            sys.exit(1)
        sys.exit(1 if problems else 0)
    if old != text:
        with open(OUT, 'w') as fh:
            fh.write(text)
    sys.exit(1 if problems else 0)


main()
