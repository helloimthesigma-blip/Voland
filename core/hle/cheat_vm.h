/**
 * Cheats (docs/DESIGN.md §19, "cheats" mod type): Atmosphere's cheat
 * format and virtual machine, reimplemented from its public format
 * description. A cheat file is plain text:
 *
 *   {Master code}          always runs first (setup the others rely on)
 *   04000000 01234567 0000270F
 *   [Infinite health]      a toggleable cheat
 *   ...hex words...
 *
 * Each cheat is a list of 32-bit opcode words run by a small VM with 16
 * 64-bit registers, conditionals, loops and memory access relative to the
 * main module, heap, alias region or the ASLR base. It runs at
 * CHEAT_VM_HZ (as Atmosphere does), on the emulator thread between
 * slices. Memory access goes through vmm only; a write to a read-only
 * page (code patches) goes through a host pointer and bumps the code
 * generation so translated code is retranslated.
 *
 * Opcodes implemented: 0 store static, 1 memory conditional, 2 end/else,
 * 3 loop, 4 load register, 5 load from memory, 6 store to register
 * address, 7 legacy arithmetic, 8 key conditional, 9 arithmetic, A store
 * register, C0 register conditional, C1/C2 save/restore, C3 static
 * registers, C4 extended key conditional, FF0/FF1 pause/resume (ignored),
 * FFF debug log (ignored). Unknown opcodes stop the cheat for this run.
 */
#ifndef SWITCH_HLE_CHEAT_VM_H
#define SWITCH_HLE_CHEAT_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/vmm.h"

#define CHEAT_MAX_ENTRIES 128u
#define CHEAT_MAX_OPCODES 0x100u /* words per cheat (Atmosphere's limit) */
#define CHEAT_NAME_BYTES 64u
#define CHEAT_REGISTERS 16u
#define CHEAT_STATIC_REGISTERS 0x100u
#define CHEAT_VM_HZ 12u
/* Bound on opcodes run per cheat per tick: a runaway loop stops here. */
#define CHEAT_MAX_STEPS 0x10000u

typedef struct Cheat_Entry {
  char name[CHEAT_NAME_BYTES];
  bool master;
  bool enabled;
  uint32_t count;
  uint32_t ops[CHEAT_MAX_OPCODES];
} Cheat_Entry;

typedef struct Cheat_Env {
  VMM_Context *vmm;
  uint64_t main_base;  /* memory type 0 */
  uint64_t heap_base;  /* 1 */
  uint64_t alias_base; /* 2 */
  uint64_t aslr_base;  /* 3 */
  uint32_t keys;       /* held buttons, HidNpadButton bits (= INPUT_BUTTON_*) */
} Cheat_Env;

typedef struct Cheat_VM {
  Cheat_Entry entries[CHEAT_MAX_ENTRIES];
  uint32_t count;
  uint64_t static_registers[CHEAT_STATIC_REGISTERS];
} Cheat_VM;

void cheat_vm_reset(Cheat_VM *vm);

/* Appends the cheats in `text` (`length` bytes). Cheats past
 * CHEAT_MAX_ENTRIES or words past CHEAT_MAX_OPCODES are dropped.
 * Toggleable cheats start enabled (Atmosphere's default). Returns the
 * number of cheats added. */
uint32_t cheat_vm_parse(Cheat_VM *vm, const char *text, size_t length);

/* Runs the master cheat, then every enabled cheat, once. */
void cheat_vm_run(Cheat_VM *vm, const Cheat_Env *env);

#endif /* SWITCH_HLE_CHEAT_VM_H */
