#include "hle/cheat_vm.h"

#include <string.h>

#include "common/log.h"

void cheat_vm_reset(Cheat_VM *vm) {
  memset(vm, 0, sizeof(*vm));
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

uint32_t cheat_vm_parse(Cheat_VM *vm, const char *text, size_t length) {
  uint32_t added = 0;
  Cheat_Entry *current = NULL;
  size_t i = 0;
  while (i < length) {
    const char c = text[i];
    if (c == '[' || c == '{') {
      const char close = c == '[' ? ']' : '}';
      size_t end = i + 1;
      while (end < length && text[end] != close && text[end] != '\n') end++;
      current = NULL;
      if (vm->count < CHEAT_MAX_ENTRIES) {
        current = &vm->entries[vm->count++];
        memset(current, 0, sizeof(*current));
        size_t n = end - (i + 1);
        if (n >= CHEAT_NAME_BYTES) n = CHEAT_NAME_BYTES - 1;
        memcpy(current->name, text + i + 1, n);
        current->master = c == '{';
        current->enabled = true;
        added++;
      }
      i = end + 1;
      continue;
    }
    if (hex_digit(c) >= 0) {
      /* A word: up to 8 hex digits. */
      uint32_t word = 0, digits = 0;
      while (i < length && hex_digit(text[i]) >= 0 && digits < 8) {
        word = word << 4 | (uint32_t)hex_digit(text[i]);
        i++;
        digits++;
      }
      if (current && current->count < CHEAT_MAX_OPCODES) current->ops[current->count++] = word;
      continue;
    }
    i++;
  }
  return added;
}

/* ------------------------------------------------------------------ */
/* Execution.                                                          */
/* ------------------------------------------------------------------ */

typedef struct Run {
  const Cheat_Env *env;
  Cheat_VM *vm;
  const Cheat_Entry *cheat;
  uint64_t reg[CHEAT_REGISTERS];
  uint64_t saved[CHEAT_REGISTERS];
  uint32_t loop_top[CHEAT_REGISTERS];
  uint32_t pc;
} Run;

static uint64_t region_base(const Cheat_Env *env, uint32_t type) {
  switch (type) {
    case 0: return env->main_base;
    case 1: return env->heap_base;
    case 2: return env->alias_base;
    case 3: return env->aslr_base;
    default: return 0;
  }
}

static bool valid_width(uint32_t w) { return w == 1 || w == 2 || w == 4 || w == 8; }

static uint64_t width_mask(uint32_t w) { return w == 8 ? ~0ull : ((1ull << (w * 8)) - 1); }

static bool mem_read(const Cheat_Env *env, uint64_t gva, uint32_t width, uint64_t *out) {
  uint8_t b[8] = {0};
  if (!error_is_ok(vmm_read_block(env->vmm, gva, b, width))) return false;
  uint64_t v = 0;
  for (uint32_t i = 0; i < width; i++) v |= (uint64_t)b[i] << (8 * i);
  *out = v;
  return true;
}

static void mem_write(const Cheat_Env *env, uint64_t gva, uint32_t width, uint64_t value) {
  uint8_t b[8];
  for (uint32_t i = 0; i < width; i++) b[i] = (uint8_t)(value >> (8 * i));
  if (error_is_ok(vmm_write_block(env->vmm, gva, b, width))) return;
  /* Read-only (code, rodata): the debugger path dmnt uses ignores page
   * permissions. Writes to executable pages retranslate. */
  VMM_Region_Info info;
  if (!error_is_ok(vmm_query(env->vmm, gva, &info)) || !info.is_mapped || (info.perms & VMM_PERM_R) == 0) return;
  void *host = NULL;
  vmm_borrow_scope_begin(env->vmm);
  if (error_is_ok(vmm_guest_to_host(env->vmm, gva, width, VMM_PERM_R, &host))) {
    memcpy(host, b, width);
    if (info.perms & VMM_PERM_X) vmm_bump_generation();
  }
  vmm_borrow_scope_end(env->vmm);
}

static bool compare(uint32_t cond, uint64_t a, uint64_t b) {
  switch (cond) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    default: return false;
  }
}

static uint32_t op_at(const Run *r, uint32_t index) {
  return index < r->cheat->count ? r->cheat->ops[index] : 0;
}

/* Words an opcode occupies, from its first word (for skipping). */
static uint32_t op_length(const Run *r, uint32_t pc) {
  const uint32_t w = op_at(r, pc);
  const uint32_t type = w >> 28;
  const uint32_t t = (w >> 24) & 0xF;
  switch (type) {
    case 0x0:
    case 0x1: return t == 8 ? 4 : 3;
    case 0x2: return 1;
    case 0x3: return t == 0 ? 2 : 1; /* 300R0000 VVVVVVVV start, 310R0000 end */
    case 0x4: return 3;
    case 0x5: return 2;
    case 0x6: return 3;
    case 0x7: return 2;
    case 0x8: return 1;
    case 0x9: return (w >> 8 & 0xF) == 1 ? (t == 8 ? 3 : 2) : 1;
    case 0xA: {
      const uint32_t o = (w >> 8) & 0xF;
      return (o == 2 || o == 4 || o == 5) ? 2 : 1;
    }
    case 0xC: {
      const uint32_t sub = (w >> 24) & 0xF;
      if (sub == 0) {
        const uint32_t x = (w >> 8) & 0xF;
        const uint32_t ct = (w >> 20) & 0xF;
        if (x == 0 || x == 2) return 2;
        if (x == 4) return ct == 8 ? 3 : 2;
        return 1;
      }
      if (sub == 4) return 3;
      return 1;
    }
    default: return 1;
  }
}

static bool is_begin(uint32_t w) {
  const uint32_t type = w >> 28;
  return type == 0x1 || type == 0x8 || (type == 0xC && ((w >> 24) & 0xF) == 0) || (type == 0xC && ((w >> 24) & 0xF) == 4);
}

/* After a false condition: skip to the matching else (resume after it)
 * or end. */
static void skip_conditional(Run *r, bool stop_at_else) {
  uint32_t depth = 0;
  while (r->pc < r->cheat->count) {
    const uint32_t w = op_at(r, r->pc);
    const uint32_t len = op_length(r, r->pc);
    r->pc += len;
    if (is_begin(w)) {
      depth++;
    } else if ((w >> 28) == 0x2) {
      const bool is_else = ((w >> 24) & 0xF) == 1;
      if (depth == 0) {
        if (!is_else || stop_at_else) return;
      } else if (!is_else) {
        depth--;
      }
    }
  }
}

static uint64_t value_at(const Run *r, uint32_t index, uint32_t width) {
  if (width == 8) return (uint64_t)op_at(r, index) << 32 | op_at(r, index + 1);
  return op_at(r, index) & width_mask(width);
}

static uint64_t arith(uint32_t op, uint64_t a, uint64_t b) {
  switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b >= 64 ? 0 : a << b;
    case 4: return b >= 64 ? 0 : a >> b;
    case 5: return a & b;
    case 6: return a | b;
    case 7: return ~a;
    case 8: return a ^ b;
    case 9: return a;
    default: return a;
  }
}

static void apply_saved(Run *r, uint32_t op, uint32_t dst, uint32_t src) {
  switch (op) {
    case 0: r->reg[dst] = r->saved[src]; break;
    case 1: r->saved[dst] = r->reg[src]; break;
    case 2: r->saved[dst] = 0; break;
    case 3: r->reg[dst] = 0; break;
    default: break;
  }
}

static void run_cheat(Run *r) {
  r->pc = 0;
  for (uint32_t steps = 0; r->pc < r->cheat->count && steps < CHEAT_MAX_STEPS; steps++) {
    const uint32_t start = r->pc;
    const uint32_t w = op_at(r, start);
    const uint32_t type = w >> 28;
    const uint32_t t = (w >> 24) & 0xF;
    r->pc += op_length(r, start);
    switch (type) {
      case 0x0: { /* 0TMR00AA AAAAAAAA V.. */
        if (!valid_width(t)) return;
        const uint32_t m = (w >> 20) & 0xF, reg = (w >> 16) & 0xF;
        const uint64_t addr = region_base(r->env, m) + r->reg[reg] + ((uint64_t)(w & 0xFF) << 32 | op_at(r, start + 1));
        mem_write(r->env, addr, t, value_at(r, start + 2, t));
        break;
      }
      case 0x1: { /* 1TMC00AA AAAAAAAA V.. */
        if (!valid_width(t)) return;
        const uint32_t m = (w >> 20) & 0xF, cond = (w >> 16) & 0xF;
        const uint64_t addr = region_base(r->env, m) + ((uint64_t)(w & 0xFF) << 32 | op_at(r, start + 1));
        uint64_t v = 0;
        if (!mem_read(r->env, addr, t, &v) || !compare(cond, v, value_at(r, start + 2, t))) skip_conditional(r, true);
        break;
      }
      case 0x2: /* 20000000 end; 21000000 else - reached while executing the true branch */
        if (t == 1) skip_conditional(r, false);
        break;
      case 0x3: { /* 300R0000 VVVVVVVV / 310R0000 */
        const uint32_t reg = (w >> 16) & 0xF;
        if (t == 0) {
          r->reg[reg] = op_at(r, start + 1);
          r->loop_top[reg] = r->pc;
        } else if (r->reg[reg] > 1) {
          r->reg[reg]--;
          r->pc = r->loop_top[reg];
        } else {
          r->reg[reg] = 0;
        }
        break;
      }
      case 0x4: /* 400R0000 VVVVVVVV VVVVVVVV */
        r->reg[(w >> 16) & 0xF] = (uint64_t)op_at(r, start + 1) << 32 | op_at(r, start + 2);
        break;
      case 0x5: { /* 5TMRI0AA AAAAAAAA */
        if (!valid_width(t)) return;
        const uint32_t m = (w >> 20) & 0xF, reg = (w >> 16) & 0xF, from_reg = (w >> 12) & 0xF;
        const uint64_t rel = (uint64_t)(w & 0xFF) << 32 | op_at(r, start + 1);
        const uint64_t addr = from_reg ? r->reg[reg] + rel : region_base(r->env, m) + rel;
        uint64_t v = 0;
        if (mem_read(r->env, addr, t, &v)) r->reg[reg] = v;
        break;
      }
      case 0x6: { /* 6T0RIor0 VVVVVVVV VVVVVVVV */
        if (!valid_width(t)) return;
        const uint32_t reg = (w >> 16) & 0xF, inc = (w >> 12) & 0xF, use_off = (w >> 8) & 0xF, off = (w >> 4) & 0xF;
        const uint64_t addr = r->reg[reg] + (use_off ? r->reg[off] : 0);
        mem_write(r->env, addr, t, ((uint64_t)op_at(r, start + 1) << 32 | op_at(r, start + 2)) & width_mask(t));
        if (inc) r->reg[reg] += t;
        break;
      }
      case 0x7: { /* 7T0RC000 VVVVVVVV */
        if (!valid_width(t)) return;
        const uint32_t reg = (w >> 16) & 0xF, op = (w >> 12) & 0xF;
        if (op > 4) return;
        r->reg[reg] = arith(op, r->reg[reg], op_at(r, start + 1)) & width_mask(t);
        break;
      }
      case 0x8: /* 8kkkkkkk */
        if ((r->env->keys & (w & 0x0FFFFFFF)) != (w & 0x0FFFFFFF)) skip_conditional(r, true);
        break;
      case 0x9: { /* 9TCRS0s0 / 9TCRS100 V.. */
        if (!valid_width(t)) return;
        const uint32_t op = (w >> 20) & 0xF, dst = (w >> 16) & 0xF, src = (w >> 12) & 0xF;
        const uint64_t rhs = ((w >> 8) & 0xF) == 1 ? value_at(r, start + 1, t) : r->reg[(w >> 4) & 0xF];
        r->reg[dst] = arith(op, r->reg[src], rhs) & width_mask(t);
        break;
      }
      case 0xA: { /* ATSRIOxa (aaaaaaaa) */
        if (!valid_width(t)) return;
        const uint32_t src = (w >> 20) & 0xF, base = (w >> 16) & 0xF, inc = (w >> 12) & 0xF;
        /* Atmosphere packs I and O as separate nibbles: ATSRIOxa. */
        const uint32_t ofs_type = (w >> 8) & 0xF, x = (w >> 4) & 0xF, a = w & 0xF;
        const uint64_t imm = (uint64_t)a << 32 | op_at(r, start + 1);
        uint64_t addr = 0;
        switch (ofs_type) {
          case 0: addr = r->reg[base]; break;
          case 1: addr = r->reg[base] + r->reg[a]; break;
          case 2: addr = r->reg[base] + imm; break;
          case 3: addr = region_base(r->env, x) + r->reg[base]; break;
          case 4: addr = region_base(r->env, x) + imm; break;
          case 5: addr = region_base(r->env, x) + r->reg[base] + imm; break;
          default: return;
        }
        mem_write(r->env, addr, t, r->reg[src] & width_mask(t));
        if (inc) r->reg[base] += t;
        break;
      }
      case 0xC: {
        const uint32_t sub = t;
        if (sub == 0) { /* C0TcSX## */
          const uint32_t width = (w >> 20) & 0xF, cond = (w >> 16) & 0xF, src = (w >> 12) & 0xF;
          const uint32_t x = (w >> 8) & 0xF, p = (w >> 4) & 0xF, q = w & 0xF;
          if (!valid_width(width)) return;
          uint64_t rhs = 0;
          bool ok = true;
          switch (x) {
            case 0: ok = mem_read(r->env, region_base(r->env, p) + ((uint64_t)q << 32 | op_at(r, start + 1)), width, &rhs); break;
            case 1: ok = mem_read(r->env, region_base(r->env, p) + r->reg[q], width, &rhs); break;
            case 2: ok = mem_read(r->env, r->reg[p] + ((uint64_t)q << 32 | op_at(r, start + 1)), width, &rhs); break;
            case 3: ok = mem_read(r->env, r->reg[p] + r->reg[q], width, &rhs); break;
            case 4: rhs = value_at(r, start + 1, width); break;
            case 5: rhs = r->reg[p]; break;
            default: return;
          }
          if (!ok || !compare(cond, r->reg[src] & width_mask(width), rhs & width_mask(width))) skip_conditional(r, true);
        } else if (sub == 1) { /* C10D0Sx0 */
          apply_saved(r, (w >> 4) & 0xF, (w >> 16) & 0xF, (w >> 8) & 0xF);
        } else if (sub == 2) { /* C2x0XXXX */
          const uint32_t op = (w >> 20) & 0xF;
          for (uint32_t i = 0; i < CHEAT_REGISTERS; i++) {
            if (w & (1u << i)) apply_saved(r, op, i, i);
          }
        } else if (sub == 3) { /* C3000XXx */
          const uint32_t index = (w >> 4) & 0xFF, reg = w & 0xF;
          if (index < 0x80) r->reg[reg] = r->vm->static_registers[index];
          else r->vm->static_registers[index] = r->reg[reg];
        } else if (sub == 4) { /* C4r00000 kkkkkkkk kkkkkkkk */
          const uint64_t mask = (uint64_t)op_at(r, start + 1) << 32 | op_at(r, start + 2);
          if ((r->env->keys & mask) != mask) skip_conditional(r, true);
        } else {
          return;
        }
        break;
      }
      case 0xF: /* FF0 pause / FF1 resume / FFF log: no-ops here */
        break;
      default:
        return;
    }
  }
}

void cheat_vm_run(Cheat_VM *vm, const Cheat_Env *env) {
  for (int pass = 0; pass < 2; pass++) {
    for (uint32_t i = 0; i < vm->count; i++) {
      const Cheat_Entry *cheat = &vm->entries[i];
      if (cheat->master != (pass == 0) || !cheat->enabled) continue;
      Run r;
      memset(&r, 0, sizeof(r));
      r.env = env;
      r.vm = vm;
      r.cheat = cheat;
      run_cheat(&r);
    }
  }
}
