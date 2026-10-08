#!/usr/bin/env python3
"""a32-oracle.py RUNNER [CASES [SEED [KINDS]]]: the AArch32 interpreter
against Unicorn (a QEMU-based CPU emulator, used only as a black-box
reference: `pip install unicorn`, ideally in a virtualenv).

Generates random A32 instruction streams biased towards what titles run
(data processing with every shift form, multiplies, media instructions,
loads and stores into a data area the base registers point into, block
transfers, VFP arithmetic, conversions, compares and transfers), runs each
on Unicorn and on tests/a32_runner (core/cpu/backends/a32), and compares
r0-r14, the PC, CPSR.NZCVQ/GE, FPSCR, D0-D31 and the data. KINDS is a
comma-separated subset of the generator names below.
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import *

CODE = 0x100000
DATA = 0x200000
DATA_BYTES = 0x2000
D_REGS = 32
FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0, 1.5, -2.5, 100.25, 1e-3, 65504.0, 1e10, -7.75, 0.1, 3.14159]


def rnd_reg(lo=0, hi=12):
    return random.randint(lo, hi)


def cond():
    return 0xE if random.random() < 0.7 else random.randint(0, 0xE)


def float_bits(single):
    v = random.choice(FLOATS) * random.choice([1, 1, 1, 3, 0.25, -1])
    if single:
        return struct.unpack('<I', struct.pack('<f', v))[0]
    return struct.unpack('<Q', struct.pack('<d', v))[0]


# ---- generators: each returns one instruction word ----

def g_dp():
    op = random.randint(0, 15)
    s = 1 if op in (8, 9, 10, 11) else random.randint(0, 1)
    rd = 0 if op in (8, 9, 10, 11) else rnd_reg()
    rn = rnd_reg(0, 12) if op not in (13, 15) else 0
    form = random.randint(0, 2)
    base = (cond() << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12)
    if form == 0:
        return base | (1 << 25) | (random.randint(0, 15) << 8) | random.randint(0, 255)
    if form == 1:
        return base | (random.randint(0, 31) << 7) | (random.randint(0, 3) << 5) | rnd_reg()
    return base | (rnd_reg() << 8) | (random.randint(0, 3) << 5) | (1 << 4) | rnd_reg()


def g_mul():
    op = random.choice([0, 1, 3, 4, 5, 6, 7, 2])
    s = random.randint(0, 1) if op not in (2, 3) else 0
    rd_hi, rd_lo, rm, rn = random.sample(range(0, 13), 4)
    return (cond() << 28) | (op << 21) | (s << 20) | (rd_hi << 16) | (rd_lo << 12) | (rm << 8) | 0x90 | rn


def g_halfmul():
    op1 = random.randint(0, 3)
    rd, ra, rm, rn = random.sample(range(0, 13), 4)
    return (cond() << 28) | (0x10 << 20) | (op1 << 21) | (rd << 16) | (ra << 12) | (rm << 8) | 0x80 | \
        (random.randint(0, 3) << 5) | rn


def g_media():
    rd, rn, rm = rnd_reg(), rnd_reg(), rnd_reg()
    c = cond() << 28
    k = random.randint(0, 13)
    if k == 0:  # UXTB/SXTB/UXTH/SXTH (+ add forms)
        op1 = random.choice([0x6A, 0x6B, 0x6E, 0x6F, 0x68, 0x6C])
        return c | (op1 << 20) | ((random.choice([rn, 15])) << 16) | (rd << 12) | (random.randint(0, 3) << 10) | 0x70 | rm
    if k == 1:  # REV, REV16, RBIT, REVSH
        op1, op2 = random.choice([(0x6B, 1), (0x6B, 5), (0x6F, 1), (0x6F, 5)])
        return c | (op1 << 20) | (0xF << 16) | (rd << 12) | (0xF << 8) | (op2 << 5) | 0x10 | rm
    if k == 2:  # UBFX/SBFX
        lsb = random.randint(0, 31)
        width = random.randint(1, 32 - lsb)
        op1 = random.choice([0x7E, 0x7A])
        return c | (op1 << 20) | ((width - 1) << 16) | (rd << 12) | (lsb << 7) | 0x50 | rm
    if k == 3:  # BFI/BFC
        lsb = random.randint(0, 31)
        msb = random.randint(lsb, 31)
        return c | (0x7C << 20) | (msb << 16) | (rd << 12) | (lsb << 7) | 0x10 | random.choice([rm, 15])
    if k == 4:  # SSAT/USAT
        op1 = random.choice([0x6A, 0x6E])
        return c | (op1 << 20) | (random.randint(0, 31) << 16) | (rd << 12) | (random.randint(0, 31) << 7) | \
            (random.randint(0, 1) << 6) | 0x10 | rm
    if k == 5:  # CLZ
        return c | 0x016F0F10 | (rd << 12) | rm
    if k == 6:  # parallel add/sub
        op1 = random.choice([1, 2, 3, 5, 6, 7])
        op2 = random.choice([0, 1, 2, 3, 4, 7])
        return c | ((0x60 | op1) << 20) | (rn << 16) | (rd << 12) | (0xF << 8) | (op2 << 5) | 0x10 | rm
    if k == 7:  # SEL
        return c | (0x68 << 20) | (rn << 16) | (rd << 12) | (0xF << 8) | 0xB0 | rm
    if k == 8:  # PKHBT/PKHTB
        return c | (0x68 << 20) | (rn << 16) | (rd << 12) | (random.randint(0, 31) << 7) | (random.randint(0, 1) << 6) | 0x10 | rm
    if k == 9:  # SDIV/UDIV
        return c | ((0x71 if random.random() < 0.5 else 0x73) << 20) | (rd << 16) | (0xF << 12) | (rm << 8) | 0x10 | rn
    if k == 10:  # SMMUL/SMMLA/SMLAD/SMUAD
        op1 = random.choice([0x75, 0x70])
        ra = random.choice([rnd_reg(), 15])
        return c | (op1 << 20) | (rd << 16) | (ra << 12) | (rm << 8) | (random.randint(0, 3) << 5) | 0x10 | rn
    if k == 11:  # QADD/QSUB/QDADD/QDSUB
        return c | (0x10 << 20) | (random.randint(0, 3) << 21) | (rn << 16) | (rd << 12) | 0x50 | rm
    if k == 12:  # USAD8/USADA8
        return c | (0x78 << 20) | (rd << 16) | (random.choice([rnd_reg(), 15]) << 12) | (rm << 8) | 0x10 | rn
    # MOVW / MOVT
    return c | ((0x30 if random.random() < 0.5 else 0x34) << 20) | (random.randint(0, 15) << 16) | (rd << 12) | random.randint(0, 0xFFF)


def g_flags():
    k = random.randint(0, 1)
    if k == 0:  # MRS
        return (cond() << 28) | 0x010F0000 | (rnd_reg() << 12)
    mask = random.choice([8, 4, 12])
    return (cond() << 28) | 0x0120F000 | (mask << 16) | rnd_reg()  # MSR CPSR_<f|s>, Rm


# Base registers for memory: r11/r12 point into the data area (set per case).
def g_ldst():
    c = cond() << 28
    rn = random.choice([11, 12])
    rt = rnd_reg(0, 10)
    k = random.randint(0, 3)
    p = random.randint(0, 1)
    u = random.randint(0, 1)
    w = 0 if p == 0 else random.randint(0, 1)
    l = random.randint(0, 1)
    if k == 0:  # LDR/STR/LDRB/STRB immediate
        return c | (0x2 << 25) | (p << 24) | (u << 23) | (random.randint(0, 1) << 22) | (w << 21) | (l << 20) | \
            (rn << 16) | (rt << 12) | random.randint(0, 0x7F)
    if k == 1:  # register offset with a small shift
        return c | (0x3 << 25) | (p << 24) | (u << 23) | (random.randint(0, 1) << 22) | (w << 21) | (l << 20) | \
            (rn << 16) | (rt << 12) | (random.randint(0, 2) << 7) | (random.randint(0, 1) << 5) | 10  # rm = r10 (small)
    if k == 2:  # LDRH/STRH/LDRSB/LDRSH/LDRD/STRD immediate
        op2 = random.randint(1, 3)
        rt = min(rt & ~1, 8) if (l == 0 and op2 != 1) else rt  # LDRD/STRD: an even pair, not the base r11 (UNPREDICTABLE)
        imm = random.randint(0, 0x3F) & ~(1 if op2 != 2 else 0)
        return c | (p << 24) | (u << 23) | (1 << 22) | (w << 21) | (l << 20) | (rn << 16) | (rt << 12) | \
            ((imm >> 4) << 8) | 0x90 | (op2 << 5) | (imm & 0xF)
    # LDM/STM
    mode = random.randint(0, 3)
    regs = random.randint(1, 0x3FF)  # r0-r9
    return c | (0x4 << 25) | ((mode >> 1) << 24) | ((mode & 1) << 23) | (random.randint(0, 1) << 21) | (l << 20) | (rn << 16) | regs


def vreg(single):
    return random.randint(0, 31) if single else random.randint(0, 31)


def enc_d(n, single, lo_bit, field_shift):
    """Splits a register number into the 4-bit field and the extra bit."""
    if single:
        return ((n >> 1) << field_shift) | ((n & 1) << lo_bit)
    return ((n & 0xF) << field_shift) | ((n >> 4) << lo_bit)


def g_vfp():
    single = random.random() < 0.5
    sz = 0 if single else 1
    c = cond() << 28
    d, n, m = vreg(single), vreg(single), vreg(single)
    k = random.randint(0, 9)
    base = c | 0x0E000A00 | (sz << 8) | enc_d(d, single, 22, 12)
    if k <= 2:  # arithmetic: opc1 0-6 with op bit
        opc1 = random.randint(0, 6)
        op = random.randint(0, 1) if opc1 != 4 else 0
        return base | ((opc1 >> 2) << 23) | ((opc1 & 3) << 20) | enc_d(n, single, 7, 16) | (op << 6) | enc_d(m, single, 5, 0)
    if k == 3:  # VMOV reg/VABS/VNEG/VSQRT
        opc2, op7 = random.choice([(0, 0), (0, 1), (1, 0), (1, 1)])
        return base | (0xB << 20) | (opc2 << 16) | (op7 << 7) | 0x40 | enc_d(m, single, 5, 0)
    if k == 4:  # VCMP/VCMPE (register, zero) then VMRS APSR_nzcv
        opc2 = random.choice([4, 5])
        return base | (0xB << 20) | (opc2 << 16) | (random.randint(0, 1) << 7) | 0x40 | (enc_d(m, single, 5, 0) if opc2 == 4 else 0)
    if k == 5:  # VCVT int <-> fp, f32<->f64
        kind = random.randint(0, 2)
        if kind == 0:  # int -> fp (source single)
            return base | (0xB << 20) | (8 << 16) | (random.randint(0, 1) << 7) | 0x40 | enc_d(random.randint(0, 31), True, 5, 0)
        if kind == 1:  # fp -> int (destination single)
            dd = random.randint(0, 31)
            return (base & ~((0xF << 12) | (1 << 22))) | enc_d(dd, True, 22, 12) | (0xB << 20) | \
                (random.choice([12, 13]) << 16) | (random.randint(0, 1) << 7) | 0x40 | enc_d(m, single, 5, 0)
        dd = random.randint(0, 31)
        return (base & ~((0xF << 12) | (1 << 22))) | enc_d(dd, not single, 22, 12) | (0xB << 20) | (7 << 16) | 0xC0 | enc_d(m, single, 5, 0)
    if k == 6:  # VMOV immediate
        return base | (0xB << 20) | (random.randint(0, 15) << 16) | random.randint(0, 15)
    if k == 7:  # VMOV core <-> single / VMRS
        if random.random() < 0.3:
            return c | 0x0EF10A10 | (rnd_reg() << 12)  # VMRS Rt, FPSCR
        sn = random.randint(0, 31)
        return c | 0x0E000A10 | (random.randint(0, 1) << 20) | ((sn >> 1) << 16) | (rnd_reg() << 12) | ((sn & 1) << 7)
    if k == 8:  # VLDR/VSTR via r11/r12
        return c | 0x0D000A00 | (random.randint(0, 1) << 23) | (random.randint(0, 1) << 20) | (random.choice([11, 12]) << 16) | \
            (sz << 8) | enc_d(d, single, 22, 12) | random.randint(0, 0x1F)
    # VLDM/VSTM IA (with writeback) through r11/r12
    count = random.randint(1, 4)
    first = random.randint(0, 31 - count)
    words = count if single else 2 * count
    return c | 0x0C800A00 | (random.randint(0, 1) << 21) | (random.randint(0, 1) << 20) | (random.choice([11, 12]) << 16) | \
        (sz << 8) | enc_d(first, single, 22, 12) | words


def g_neon():
    """Any word in the Advanced SIMD data-processing space (Unicorn skips the undefined ones)."""
    w = 0xF2000000 | (random.randint(0, 1) << 24) | random.getrandbits(24)
    if (w & 0x01B00810) == 0x01B00000 and ((w >> 7) & 0xF) in (1, 2, 3) and ((w >> 12) & 0xF) == (w & 0xF):
        w ^= 1  # VTRN/VUZP/VZIP with Vd == Vm: UNPREDICTABLE
    return w


def g_neonls():
    """Element and structure loads and stores through r11/r12; Rm none, writeback or r10."""
    return 0xF4000000 | (random.randint(0, 1) << 23) | (random.randint(0, 1) << 22) | (random.randint(0, 1) << 21) | \
        (random.choice([11, 12]) << 16) | (random.randint(0, 15) << 12) | (random.getrandbits(8) << 4) | \
        random.choice([15, 13, 10])


def qreg_fields(word, d, n, m):
    """Even (Q) register numbers into the D:Vd, N:Vn, M:Vm fields."""
    return word | ((d >> 4) << 22) | ((d & 15) << 12) | ((n >> 4) << 7) | ((n & 15) << 16) | ((m >> 4) << 5) | (m & 15)


def g_crypto():
    d, n, m = (random.randint(0, 15) * 2 for _ in range(3))
    k = random.randint(0, 4)
    if k == 0:  # AESE/AESD/AESMC/AESIMC
        return qreg_fields(0xF3B00300 | (random.randint(0, 3) << 6), d, 0, m)
    if k == 1:  # SHA1C/P/M/SU0, SHA256H/H2/SU1
        u, size = random.choice([(0, 0), (0, 1), (0, 2), (0, 3), (1, 0), (1, 1), (1, 2)])
        return qreg_fields(0xF2000C40 | (u << 24) | (size << 20), d, n, m)
    if k == 2:  # SHA1H
        return qreg_fields(0xF3B902C0, d, 0, m)
    if k == 3:  # SHA1SU1 / SHA256SU0
        return qreg_fields(0xF3BA0380 | (random.randint(0, 1) << 6), d, 0, m)
    return qreg_fields(0xF3B00300 | (random.randint(0, 3) << 6), d, 0, m)


def g_vfp8():
    """VSEL, VMAXNM/VMINNM, VRINTA/N/P/M, VCVTA/N/P/M."""
    single = random.random() < 0.5
    sz = 0 if single else 1
    d, n, m = vreg(single), vreg(single), vreg(single)
    base = 0xFE000A00 | (sz << 8) | enc_d(d, single, 22, 12) | enc_d(m, single, 5, 0)
    k = random.randint(0, 3)
    if k == 0:
        return base | (random.randint(0, 3) << 20) | enc_d(n, single, 7, 16)
    if k == 1:
        return base | (1 << 23) | (random.randint(0, 1) << 6) | enc_d(n, single, 7, 16)
    if k == 2:
        return base | 0x00B80040 | (random.randint(0, 3) << 16)
    sd = random.randint(0, 31)
    return (base & ~((0xF << 12) | (1 << 22))) | enc_d(sd, True, 22, 12) | 0x00BC0040 | (random.randint(0, 3) << 16) | \
        (random.randint(0, 1) << 7)


ISOLATE = False
GENERATORS = {'dp': g_dp, 'mul': g_mul, 'halfmul': g_halfmul, 'media': g_media, 'flags': g_flags, 'ldst': g_ldst,
              'vfp': g_vfp, 'neon': g_neon, 'neonls': g_neonls, 'crypto': g_crypto, 'vfp8': g_vfp8}
WEIGHTS = {'dp': 6, 'mul': 2, 'halfmul': 1, 'media': 4, 'flags': 1, 'ldst': 4, 'vfp': 4, 'neon': 4, 'neonls': 2, 'crypto': 1, 'vfp8': 1}


def make_case(kinds):
    n = 1 if os.environ.get('A32_ONE') else random.randint(1, 12)
    pool = [k for k in kinds for _ in range(WEIGHTS[k])]
    code = [GENERATORS[random.choice(pool)]() for _ in range(n)]
    r = [random.choice([0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, random.getrandbits(32), random.randint(0, 100)])
         for _ in range(15)]
    r[10] = random.randint(0, 32) * 4  # small register offset
    r[11] = DATA + 0x400 + random.randint(0, 0x100) * 4
    r[12] = DATA + 0x1000 + random.randint(0, 0x100) * 4
    r[13] = DATA + 0x1F00
    cpsr = (random.getrandbits(4) << 28) | (random.getrandbits(1) << 27) | (random.getrandbits(4) << 16)
    fpscr = (random.getrandbits(4) << 28) | random.choice([0, 0, 0, 1 << 22, 2 << 22, 3 << 22, 1 << 24, 1 << 25])
    d = []
    for i in range(D_REGS):
        if random.random() < 0.5:
            d.append(float_bits(True) | (float_bits(True) << 32))
        else:
            d.append(float_bits(False))
    data = bytes(random.getrandbits(8) for _ in range(DATA_BYTES))
    return code, r, cpsr, fpscr, d, data


REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
        UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
        UC_ARM_REG_LR]
DREGS = [getattr(__import__('unicorn.arm_const', fromlist=['x']), 'UC_ARM_REG_D%d' % i) for i in range(D_REGS)]


def run_unicorn(case):
    code, r, cpsr, fpscr, d, data = case
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try:
        mu.ctl_set_cpu_model(UC_CPU_ARM_MAX)
    except Exception:
        pass
    mu.mem_map(CODE, 0x1000)
    mu.mem_map(DATA, DATA_BYTES)
    mu.mem_write(CODE, struct.pack('<%dI' % len(code), *code))
    mu.mem_write(DATA, data)
    mu.reg_write(UC_ARM_REG_C1_C0_2, mu.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))  # CPACR: cp10, cp11
    mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    mu.reg_write(UC_ARM_REG_CPSR, 0x10 | cpsr)  # user mode, ARM state, A/I/F clear: first, r13/r14 are banked
    for reg, v in zip(REGS, r):
        mu.reg_write(reg, v)
    mu.reg_write(UC_ARM_REG_FPSCR, fpscr)
    for reg, v in zip(DREGS, d):
        mu.reg_write(reg, v)
    exit_code = 0
    try:
        mu.emu_start(CODE, CODE + 4 * len(code), count=len(code))
    except UcError as e:
        exit_code = 1 if 'INSN_INVALID' in str(e) or 'EXCEPTION' in str(e) else 2
    pc = mu.reg_read(UC_ARM_REG_PC) - CODE
    regs = [mu.reg_read(reg) & 0xFFFFFFFF for reg in REGS]
    out_cpsr = mu.reg_read(UC_ARM_REG_CPSR) & 0xF80F0000
    out_fpscr = mu.reg_read(UC_ARM_REG_FPSCR)
    out_d = [mu.reg_read(reg) for reg in DREGS]
    out_data = bytes(mu.mem_read(DATA, DATA_BYTES))
    return exit_code, pc, regs, out_cpsr, out_fpscr, out_d, out_data


def run_unicorn_isolated(case):
    """run_unicorn in a child process: Unicorn 2.1.4 dies (SIGILL) on a few
    valid encodings (64-bit VSHL on some hosts); such a case is skipped."""
    import pickle
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(r)
        try:
            data = pickle.dumps(run_unicorn(case))
        except Exception:
            data = b''
        with os.fdopen(w, 'wb') as out:
            out.write(data)
        os._exit(0)
    os.close(w)
    with os.fdopen(r, 'rb') as inp:
        data = inp.read()
    os.waitpid(pid, 0)
    if not data:
        return (3, 0, [], 0, 0, [], b'')
    return pickle.loads(data)


def write_cases(path, cases):
    with open(path, 'wb') as f:
        f.write(b'A32C' + struct.pack('<I', len(cases)))
        for code, r, cpsr, fpscr, d, data in cases:
            f.write(struct.pack('<I', len(code)) + struct.pack('<%dI' % len(code), *code))
            f.write(struct.pack('<15I', *r) + struct.pack('<II', cpsr, fpscr))
            f.write(struct.pack('<%dQ' % D_REGS, *d) + data)


def read_results(path, count):
    out = []
    with open(path, 'rb') as f:
        assert f.read(4) == b'A32R'
        for _ in range(count):
            exit_code, pc = struct.unpack('<II', f.read(8))
            regs = list(struct.unpack('<15I', f.read(60)))
            cpsr, fpscr = struct.unpack('<II', f.read(8))
            d = list(struct.unpack('<%dQ' % D_REGS, f.read(8 * D_REGS)))
            data = f.read(DATA_BYTES)
            out.append((exit_code, pc, regs, cpsr & 0xF80F0000, fpscr, d, data))
    return out


def disasm(words):
    return ' '.join('%08x' % w for w in words)


def main():
    runner = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    kinds = sys.argv[4].split(',') if len(sys.argv) > 4 else list(GENERATORS)
    random.seed(seed)
    global ISOLATE
    ISOLATE = any(k.startswith('neon') or k == 'crypto' for k in kinds)
    cases = [make_case(kinds) for _ in range(count)]
    with tempfile.TemporaryDirectory() as t:
        cp, rp = os.path.join(t, 'cases.bin'), os.path.join(t, 'results.bin')
        write_cases(cp, cases)
        subprocess.run([runner, cp, rp], check=True)
        ours = read_results(rp, count)
    failures = 0
    skipped = 0
    groups = {}
    accepted = {}  # Unicorn refused, we ran (single-instruction mode)
    for i, case in enumerate(cases):
        ref = run_unicorn_isolated(case) if ISOLATE else run_unicorn(case)
        if ref[0] != 0:  # Unicorn refused the stream (an encoding it treats as undefined): no reference
            skipped += 1
            if os.environ.get('A32_ONE') and ours[i][0] == 0:
                accepted.setdefault(case[0][0] & 0x0FF000F0, []).append(case[0][0])
            continue
        mine = ours[i]
        names = ['exit', 'pc', 'regs', 'cpsr', 'fpscr', 'd', 'data']
        bad = [names[k] for k in range(7) if mine[k] != ref[k]]
        if bad and os.environ.get('A32_RESULTS_ONLY') and mine[0] == 1:
            continue  # we call it UNDEFINED: an ARMv8.1+ encoding Unicorn's "max" CPU has (reviewed by group)
        if bad:
            failures += 1
            if os.environ.get('A32_ONE'):
                groups.setdefault(case[0][0] & 0x0FF000F0, []).append(case[0][0])
            if failures <= 8:
                print('MISMATCH case %d (%s): %s' % (i, ','.join(bad), disasm(case[0])))
                if 'regs' in bad:
                    for k in range(15):
                        if mine[2][k] != ref[2][k]:
                            print('   r%d: ours %08x unicorn %08x (in %08x)' % (k, mine[2][k], ref[2][k], case[1][k]))
                if 'cpsr' in bad:
                    print('   cpsr: ours %08x unicorn %08x (in %08x)' % (mine[3], ref[3], case[2]))
                if 'fpscr' in bad:
                    print('   fpscr: ours %08x unicorn %08x (in %08x)' % (mine[4], ref[4], case[3]))
                if 'd' in bad:
                    for k in range(D_REGS):
                        if mine[5][k] != ref[5][k]:
                            print('   d%d: ours %016x unicorn %016x (in %016x)' % (k, mine[5][k], ref[5][k], case[4][k]))
                if 'exit' in bad or 'pc' in bad:
                    print('   exit/pc: ours %d/%x unicorn %d/%x' % (mine[0], mine[1], ref[0], ref[1]))
    for key, words in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print('group %08x: %d (%s)' % (key, len(words), disasm(words[:4])))
    for key, words in sorted(accepted.items(), key=lambda kv: -len(kv[1])):
        print('accepted %08x: %d (%s)' % (key, len(words), disasm(words[:4])))
    print('%d cases, %d compared, %d skipped (Unicorn refused), %d mismatches' % (count, count - skipped, skipped, failures))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
