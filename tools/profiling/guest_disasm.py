#!/usr/bin/env python3
"""guest_disasm.py MODULES_DIR module+0xOFF [BEFORE AFTER]: disassembles guest
code around a PC-profile block (needs clang + llvm-objdump; see README.md)."""
import os, subprocess, sys, tempfile
d, spec = sys.argv[1], sys.argv[2]
before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x100
after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x200
mods = {}
for line in open(os.path.join(d, "modules.txt")):
    name, base, text, size = line.split()
    mods[name] = (int(base, 16), int(text, 16), int(size, 16))
name, off = spec.split("+")
base, text, size = mods[name]
pc = base + int(off, 16)
start = max(text, pc - before)
end = min(text + size, pc + after)
data = open(os.path.join(d, name + ".text"), "rb").read()[start - text:end - text]
with tempfile.TemporaryDirectory() as t:
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data) - 3, 4)]
    open(os.path.join(t, "c.s"), "w").write(".text\n" + "".join(".inst 0x%08x\n" % w for w in words))
    subprocess.run(["clang", "-target", "aarch64-none-elf", "-c", os.path.join(t, "c.s"), "-o", os.path.join(t, "c.o")], check=True)
    out = subprocess.run([os.environ.get("LLVM_OBJDUMP", "llvm-objdump"), "-d", "--no-show-raw-insn",
                          "--adjust-vma=0x%x" % (start - base), os.path.join(t, "c.o")], capture_output=True, text=True).stdout
for line in out.splitlines():
    if line.strip() and not line.startswith("/") and "file format" not in line:
        print(line)
