"""Turn dumpbin /exports output for ntoskrnl.exe into a .def file.

The public tree ships no WDK, so the import library the linker needs is
generated from the running kernel's own export table. The names are the real
one, which is also what a manual mapper resolves against at load time.

usage: make_importlib.py <dumpbin-exports.txt> <out.def>
"""
import re
import sys

LINE = re.compile(r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+([A-Za-z_][A-Za-z0-9_@?$]*)\s*$")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    names = []
    seen = set()
    with open(src, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = LINE.match(line)
            if not m:
                continue
            name = m.group(1)
            if name in seen:
                continue
            seen.add(name)
            names.append(name)
    if not names:
        print("no exports parsed from", src)
        return 1
    with open(dst, "w", encoding="ascii", newline="\r\n") as fh:
        fh.write("LIBRARY ntoskrnl.exe\nEXPORTS\n")
        for name in names:
            fh.write("    %s\n" % name)
    print("wrote %s with %d exports" % (dst, len(names)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
