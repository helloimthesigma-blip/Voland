#!/usr/bin/env python3
"""insn_mix.py MODULES_DIR pcsamples.bin [threads,...] - instruction-class mix
of sampled stop PCs per thread (scratch tool; clang + llvm-objdump)."""
import collections, os, re, struct, subprocess, sys, tempfile
d, path = sys.argv[1], sys.argv[2]
want = set(int(x) for x in sys.argv[3].split(",")) if len(sys.argv) > 3 else None
mods = []
for line in open(os.path.join(d, "modules.txt")):
    n, b, t, s = line.split(); mods.append((n, int(b, 16), int(t, 16), int(s, 16)))
texts = {n: open(os.path.join(d, n + ".text"), "rb").read() for n, *_ in mods}
raw = open(path, "rb").read()
samples = [struct.unpack_from("<IIQ", raw, i) for i in range(0, len(raw) - 15, 16)]
def word(pc):
    for n, b, t, s in mods:
        if t <= pc < t + s: return struct.unpack_from("<I", texts[n], pc - t)[0]
    return None
pcs = sorted({pc for slot, _, pc in samples})
words = {pc: word(pc) for pc in pcs}
valid = [pc for pc in pcs if words[pc] is not None]
with tempfile.TemporaryDirectory() as t:
    open(os.path.join(t, "c.s"), "w").write(".text\n" + "".join(".inst 0x%08x\n" % words[pc] for pc in valid))
    subprocess.run(["clang", "-target", "aarch64-none-elf", "-march=armv8.2-a+fp16+crypto", "-c", os.path.join(t, "c.s"), "-o", os.path.join(t, "c.o")], check=True)
    out = subprocess.run([os.environ.get("LLVM_OBJDUMP", "llvm-objdump"), "-d", "--no-show-raw-insn", os.path.join(t, "c.o")], capture_output=True, text=True).stdout
insns = [l.split("\t", 1)[1].strip() for l in out.splitlines() if re.match(r"\s+[0-9a-f]+:\s", l) and "\t" in l]
text = dict(zip(valid, insns))
def cls(i):
    m = i.split()[0] if i else "?"
    ops = i[len(m):]
    vec = bool(re.search(r"v\d+\.\d*[bhsd]", ops))
    byel = bool(re.search(r"v\d+\.[bhsd]\[\d+\]", ops)) and m not in ("mov", "ins", "dup", "umov", "smov", "ld1", "st1", "ld1r")
    fpreg = bool(re.search(r"\b[hsdq]\d+\b", ops))
    if m in ("scvtf", "ucvtf", "fcvtzs", "fcvtzu", "fcvt", "fcvtns", "fcvtms", "fcvtps", "fcvtas", "fcvtau", "fcvtmu", "fcvtnu", "frintm", "frintp", "frintz", "frinta", "frintn", "frintx", "frinti"): return "FP convert/round (scvtf, fcvtzs, frint*)"
    if m.startswith("ld") or m.startswith("st"):
        if vec or m in ("ld1", "st1", "ld2", "st2", "ld3", "st3", "ld4", "st4", "ld1r"): return "SIMD vector load/store"
        if fpreg: return "FP/SIMD scalar load/store (ldr/str s,d,q)"
        if m.startswith("ldax") or m.startswith("ldxr") or m.startswith("stlx") or m.startswith("stxr") or m.startswith("ldar") or m.startswith("stlr"): return "exclusive/acquire-release"
        return "integer load/store"
    if byel: return "SIMD by-element (fmla v.s[i] ...)"
    if m.startswith("f") and m not in ("fmov",):
        if vec: return "SIMD vector FP arith"
        if m in ("fmadd", "fmsub", "fnmadd", "fnmsub"): return "FP scalar fused (fmadd/fmsub)"
        if m in ("fcmp", "fcmpe", "fccmp", "fcsel", "fmin", "fmax", "fminnm", "fmaxnm", "fabs", "fneg"): return "FP scalar compare/select/abs/minmax"
        return "FP scalar arith (fadd/fsub/fmul/fdiv/fsqrt)"
    if m == "fmov" or (vec and m in ("mov", "dup", "ins", "umov", "smov", "movi", "mvni")): return "FP/SIMD moves (fmov, dup, ins, umov)"
    if vec: return "SIMD vector integer/other"
    if m in ("b", "bl", "br", "blr", "ret", "cbz", "cbnz", "tbz", "tbnz") or m.startswith("b."): return "branches"
    if m in ("ubfx", "sbfx", "ubfiz", "sbfiz", "bfi", "bfxil", "lsl", "lsr", "asr", "ror", "extr", "rev", "rbit", "clz", "uxtb", "uxth", "sxtb", "sxth", "sxtw"): return "integer bitfield/shift"
    if m in ("mul", "madd", "msub", "smull", "umull", "smaddl", "umaddl", "smulh", "umulh", "sdiv", "udiv", "mneg"): return "integer multiply/divide"
    if m in ("svc", "dmb", "dsb", "isb", "mrs", "msr", "nop", "hint", "yield", "clrex"): return "system/barriers"
    return "integer ALU (add/sub/logic/mov/cmp/csel)"
by_thread = collections.defaultdict(collections.Counter); total = collections.Counter()
for slot, _, pc in samples:
    if want is not None and slot not in want: continue
    c = cls(text.get(pc, ""))
    by_thread[slot][c] += 1; total[c] += 1
n = sum(total.values())
print("%d samples" % n)
for c, k in total.most_common(): print("  %5.1f%%  %s" % (100 * k / n, c))
for slot in sorted(by_thread, key=lambda s: -sum(by_thread[s].values()))[:6]:
    cnt = by_thread[slot]; m = sum(cnt.values())
    print("thread slot %d: %d samples: %s" % (slot, m, ", ".join("%s %.0f%%" % (c.split(" (")[0], 100 * k / m) for c, k in cnt.most_common(6))))
