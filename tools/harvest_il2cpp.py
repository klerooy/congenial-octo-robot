"""IL2CPP metadata harvester for ROB RUN (Unity 6000.3 / metadata v39).

Independent of the runtime overlay: reads global-metadata.dat + GameAssembly.dll
and resolves type field layouts, method tables and native code addresses.

Usage:
  python harvest_il2cpp.py find <substr> [...]        # list matching type names
  python harvest_il2cpp.py dump <exact.type.Name>    # fields + methods with RVA
  python harvest_il2cpp.py strings <substr> [...]    # string literals
  python harvest_il2cpp.py methods <substr> [...]    # method names across types
  python harvest_il2cpp.py bulk <out.json> <regex> [...]
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

GAME = Path(r'C:\Program Files (x86)\Steam\steamapps\common\Rob Run')
META = GAME / 'ROB RUN_Data/il2cpp_data/Metadata/global-metadata.dat'
DLL = GAME / 'GameAssembly.dll'

KIND = {1: 'void', 2: 'bool', 3: 'char', 4: 'sbyte', 5: 'byte', 6: 'short', 7: 'ushort',
        8: 'int', 9: 'uint', 10: 'long', 11: 'ulong', 12: 'float', 13: 'double',
        14: 'string', 15: 'ptr', 16: 'byref', 17: 'valuetype', 18: 'class',
        19: 'var', 20: 'array', 21: 'generic', 22: 'typedbyref', 24: 'intptr',
        25: 'uintptr', 27: 'fnptr', 28: 'object', 29: 'szarray'}


class Meta:
    def __init__(self):
        self.m = META.read_bytes()
        self.dll = DLL.read_bytes()
        magic, ver = struct.unpack_from('<II', self.m)
        assert (magic, ver) == (0xFAB11BAF, 39), (hex(magic), ver)
        self.so, self.ss = struct.unpack_from('<II', self.m, 0x20)
        self.to, _, self.tn = struct.unpack_from('<III', self.m, 0xEC)
        self.fo, _, self.fn = struct.unpack_from('<III', self.m, 0x8C)
        self.mo, _, self.mn = struct.unpack_from('<III', self.m, 0x44)
        self.strs = self.m[self.so:self.so + self.ss]
        self._pe = struct.unpack_from('<I', self.dll, 0x3C)[0]
        self._op = self._pe + 24
        self.imagebase = struct.unpack_from('<Q', self.dll, self._op + 24)[0]
        self.imagesize = struct.unpack_from('<I', self.dll, self._op + 56)[0]
        self.sections = []
        self.nsec = struct.unpack_from('<H', self.dll, self._pe + 6)[0]
        base = self._op + struct.unpack_from('<H', self.dll, self._pe + 20)[0]
        for i in range(self.nsec):
            p = base + i * 40
            vsize, va, rsize, roff = struct.unpack_from('<IIII', self.dll, p + 8)
            self.sections.append((va, max(vsize, rsize), roff))
        self._reg = self._find_registration()
        self._types_count, self._types_array = struct.unpack_from('<QQ', self.dll, self._reg + 48)
        self._fields_array = struct.unpack_from('<Q', self.dll, self._reg + 88)[0]
        self._methods_count, self._methods_array = struct.unpack_from('<QQ', self.dll, self._reg + 64)
        self._mods = self._find_codegen_modules()

    # ---- address helpers -------------------------------------------------
    def fileoff(self, va):
        off = va - self.imagebase
        for va0, size, roff in self.sections:
            if va0 <= off < va0 + size:
                return roff + off - va0
        raise ValueError(hex(va))

    def q(self, va):
        return struct.unpack_from('<Q', self.dll, self.fileoff(va))[0]

    def i32(self, va):
        return struct.unpack_from('<i', self.dll, self.fileoff(va))[0]

    def _find_registration(self):
        """Locate Il2CppCodeRegistration by its unique 0x10 stride signature."""
        want = struct.pack('<8i', 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48)
        pos = 0
        tn = self.tn
        needle = struct.pack('<Q', tn)
        while True:
            pos = self.dll.find(needle, pos)
            if pos < 0:
                raise RuntimeError('registration not found')
            if struct.unpack_from('<Q', self.dll, pos + 16)[0] == tn:
                try:
                    ptr = struct.unpack_from('<Q', self.dll, pos + 8)[0]
                    offs = self.q(ptr + 4312 * 8)
                    if struct.unpack_from('<8i', self.dll, self.fileoff(offs)) == (0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48):
                        return pos - 80
                except (ValueError, struct.error):
                    pass
            pos += 1

    def _find_codegen_modules(self):
        """moduleName -> (methodPointerCount, methodPointers[]) for each module."""
        out = {}
        for probe in (b'Assembly-CSharp.dll\x00', b'UnityEngine.CoreModule.dll\x00'):
            hit = self.dll.find(probe)
            if hit < 0:
                continue
            va = self.imagebase + hit
            pat = struct.pack('<Q', va)
            pos = 0
            while True:
                pos = self.dll.find(pat, pos)
                if pos < 0:
                    break
                try:
                    cnt = struct.unpack_from('<I', self.dll, pos + 8)[0]
                    arr = struct.unpack_from('<Q', self.dll, pos + 16)[0]
                    fo = self.fileoff(arr)
                    if 0 < cnt < 400000 and fo + cnt * 8 <= len(self.dll):
                        name = self.dll[hit:self.dll.index(b'\x00', hit)].decode()
                        first = struct.unpack_from('<Q', self.dll, fo)[0]
                        if self.imagebase <= first < self.imagebase + self.imagesize:
                            out.setdefault(name, (cnt, arr))
                except (ValueError, struct.error, IndexError):
                    pass
                pos += 8
        return out

    # ---- metadata helpers ------------------------------------------------
    def text(self, i):
        if i < 0 or i >= len(self.strs):
            return ''
        return self.strs[i:self.strs.index(b'\x00', i)].decode('utf-8', 'replace')

    def type_name(self, idx):
        p = self.to + idx * 82
        ni, ns = struct.unpack_from('<II', self.m, p)
        ns, ni = self.text(ns), self.text(ni)
        return f'{ns}.{ni}' if ns else ni

    def type_record(self, idx):
        p = self.to + idx * 82
        return self.m[p:p + 82]

    def type_index_by_name(self):
        if not hasattr(self, '_byname'):
            self._byname = {}
            for i in range(self.tn):
                self._byname.setdefault(self.type_name(i), i)
        return self._byname

    def fields(self, idx):
        rec = self.type_record(idx)
        start = struct.unpack_from('<I', rec, 0x1A)[0]
        count = struct.unpack_from('<H', rec, 0x3E)[0]
        offs = self.q(self._fields_array + idx * 8)
        rows = []
        for j in range(count):
            ni, ty, tok = struct.unpack_from('<III', self.m, self.fo + (start + j) * 12)
            rows.append({'name': self.text(ni), 'offset': self.i32(offs + j * 4),
                         'type': self.typedesc(self.q(self._types_array + ty * 8))})
        return rows

    def methods(self, idx):
        rec = self.type_record(idx)
        start = struct.unpack_from('<I', rec, 0x1E)[0]
        count = struct.unpack_from('<H', rec, 0x3A)[0]
        rows = []
        for j in range(count):
            p = self.mo + (start + j) * 32
            ni, tok = struct.unpack_from('<I', self.m, p)[0], struct.unpack_from('<I', self.m, p + 0x14)[0]
            flags, iflags, slot, pcount = struct.unpack_from('<HHHH', self.m, p + 0x18)
            rows.append({'name': self.text(ni), 'token': tok, 'flags': flags,
                         'params': pcount, 'rva': self.method_rva(tok)})
        return rows

    def method_rva(self, token):
        if (token >> 24) != 6:
            return None
        row = (token & 0xFFFFFF) - 1
        for name, (cnt, arr) in self._mods.items():
            if row < cnt:
                try:
                    va = self.q(arr + row * 8)
                except ValueError:
                    continue
                if self.imagebase <= va < self.imagebase + self.imagesize:
                    return va
        return None

    def typedesc(self, va, depth=0):
        try:
            data, bits = struct.unpack_from('<QI', self.dll, self.fileoff(va))
        except (ValueError, struct.error):
            return {'kind': '?'}
        kind = (bits >> 16) & 0xFF
        out = {'kind': KIND.get(kind, hex(kind))}
        if kind in (17, 18) and data < self.tn:
            out['name'] = self.type_name(data)
        if kind == 21 and depth < 3:
            try:
                core, inst = struct.unpack_from('<QQ', self.dll, self.fileoff(data))
                out['definition'] = self.typedesc(core, depth + 1)
                argc = struct.unpack_from('<I', self.dll, self.fileoff(inst))[0]
                args = self.q(inst + 8)
                if argc < 16:
                    out['arguments'] = [self.typedesc(self.q(args + 8 * i), depth + 1) for i in range(argc)]
            except (ValueError, struct.error):
                out['generic'] = True
        return out

    def string_literals(self):
        """Printable runs of the UTF-8 literal blob.

        The literal offset table is a 4-byte-per-entry index into a blob with
        NUL separators; entry alignment differs between builds, so runs are
        recovered by scanning for printable sequences instead.
        """
        _, _, lc = struct.unpack_from('<III', self.m, 0x08)
        do, dsz, dc = struct.unpack_from('<III', self.m, 0x14)
        if lc == 0 or dc == 0:
            return []
        blob = self.m[do:do + dsz]
        out = []
        for run in re.findall(rb'[\x20-\x7e]{3,}', blob):
            out.append(run.decode('utf-8', 'replace'))
        return out


def fmt_type(t):
    if 'name' in t:
        return t['name']
    if 'definition' in t:
        d = fmt_type(t['definition'])
        a = ', '.join(fmt_type(x) for x in t.get('arguments', []))
        return f'{d}<{a}>'
    return t['kind']


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'find'
    args = sys.argv[2:]
    meta = Meta()
    if cmd == 'find':
        for pat in args:
            rx = re.compile(pat, re.I)
            hits = [meta.type_name(i) for i in range(meta.tn) if rx.search(meta.type_name(i))]
            print(f'=== {pat}: {len(hits)} types')
            for h in hits:
                print(' ', h)
    elif cmd == 'dump':
        byname = meta.type_index_by_name()
        for name in args:
            idx = byname.get(name)
            if idx is None:
                print('missing', name)
                continue
            print(f'=== {name} (type {idx})')
            for f in meta.fields(idx):
                print(f'  +0x{f["offset"]:03X}  {fmt_type(f["type"]):42s} {f["name"]}')
            for m in meta.methods(idx):
                rva = f'0x{m["rva"]:X}' if m['rva'] else '-'
                print(f'  {rva:10s} params={m["params"]} flags=0x{m["flags"]:04X}  {m["name"]}')
    elif cmd == 'methods':
        for pat in args:
            rx = re.compile(pat, re.I)
            print(f'=== method names /{pat}/')
            for i in range(meta.tn):
                ms = [m for m in meta.methods(i) if rx.search(m['name'])]
                if ms:
                    print(' ', meta.type_name(i))
                    for m in ms:
                        rva = f'0x{m["rva"]:X}' if m['rva'] else '-'
                        print(f'    {rva:10s} {m["name"]}')
    elif cmd == 'bulk':
        out_path = Path(args[0])
        pats = args[1:]
        rxs = [re.compile(p) for p in pats]
        report = {'game_assembly_sha256': hashlib.sha256(meta.dll).hexdigest(),
                  'metadata_version': 39, 'patterns': pats, 'types': {}}
        for i in range(meta.tn):
            name = meta.type_name(i)
            if not any(r.search(name) for r in rxs):
                continue
            report['types'][name] = {
                'type_index': i,
                'fields': [{'name': f['name'], 'offset': f['offset'], 'type': fmt_type(f['type'])}
                           for f in meta.fields(i)],
                'methods': [{'name': m['name'], 'token': m['token'], 'params': m['params'],
                             'flags': m['flags'],
                             'va': f"0x{m['rva']:X}" if m['rva'] else None}
                            for m in meta.methods(i)],
            }
        out_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'{len(report["types"])} types -> {out_path}')
    elif cmd == 'strings':
        lits = meta.string_literals()
        blob = '\n'.join(lits)
        print(f'# {len(lits)} literal runs')
        for pat in args:
            rx = re.compile(pat, re.I)
            seen = set()
            print(f'=== /{pat}/')
            for s in lits:
                for m in rx.finditer(s):
                    a = max(0, m.start() - 60)
                    frag = s[a:m.end() + 60].replace('\n', ' ')
                    if frag in seen:
                        continue
                    seen.add(frag)
                    print(' ', frag)
                    if len(seen) >= 40:
                        break
                if len(seen) >= 40:
                    break
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
