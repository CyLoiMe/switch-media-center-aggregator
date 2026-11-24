#!/usr/bin/env python3
"""Map an Atmosphère crash report's return addresses to function names.

    python3 scripts/crash-decode.py <crash-report.log> <SMCA.elf>

The report gives every frame as `SMCA + 0xNNNN` (offset from the module base), and
the ELF carries the symbol table, so a crash can be read without guessing: the top
frame is where the fault happened. Get the matching ELF from the SAME build (CI
uploads it as the `SMCA-dev-elf-*` artifact) — offsets shift between builds, and a
mismatched ELF produces confidently wrong names.

No third-party modules: the ELF64 symbol table is parsed directly.
"""
import bisect
import re
import struct
import sys


def load_symbols(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 2:
        raise SystemExit(f"{path}: not a 64-bit ELF")
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_shentsize, = struct.unpack_from('<H', data, 0x3A)
    e_shnum, = struct.unpack_from('<H', data, 0x3C)
    e_shstrndx, = struct.unpack_from('<H', data, 0x3E)

    secs = []
    for i in range(e_shnum):
        secs.append(struct.unpack_from('<IIQQQQIIQQ', data, e_shoff + i * e_shentsize))

    symtab = next((s for s in secs if s[1] == 2), None)   # SHT_SYMTAB
    if symtab is None:
        raise SystemExit(f"{path}: stripped ELF — use the CI debug artifact")
    strtab = secs[symtab[6]]

    out = []
    for i in range(symtab[5] // symtab[9]):
        off = symtab[4] + i * symtab[9]
        st_name, st_info, _other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', data, off)
        if st_shndx == 0 or st_size == 0:
            continue
        if (st_info & 0xf) not in (0, 2):                  # FUNC or NOTYPE
            continue
        start = strtab[4] + st_name
        end = data.index(b'\0', start)
        out.append((st_value, st_size, data[start:end].decode('latin1')))
    out.sort()
    return out


def resolve(syms, addr):
    addrs = [s[0] for s in syms]
    i = bisect.bisect_right(addrs, addr) - 1
    for j in range(max(0, i - 8), i + 1):
        start, size, name = syms[j]
        if start <= addr < start + size:
            return f"{name} + 0x{addr - start:x}"
    return f"<no symbol at 0x{addr:x}>"


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    report, elf = sys.argv[1], sys.argv[2]
    syms = load_symbols(elf)

    frames = []
    for line in open(report, encoding='utf-8', errors='replace'):
        m = re.search(r'ReturnAddress\[\d+\]:\s+([0-9a-f]+)\s+\(SMCA \+ (0x[0-9a-f]+)\)', line)
        if m:
            frames.append(int(m.group(2), 16))
        if line.strip().startswith('Stack Dump'):   # end of the crashed thread's trace
            break
        if line.strip().startswith('Threads['):     # crash reports put the others after
            break

    if not frames:
        raise SystemExit("no 'ReturnAddress' lines for the crashed thread")

    print(f"{len(frames)} frame(s), innermost first:")
    for i, off in enumerate(frames):
        print(f"  [{i:02d}] SMCA+0x{off:x}  {resolve(syms, off)}")


if __name__ == '__main__':
    main()
