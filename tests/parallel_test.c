/**
 * Parallel guest threads (docs/PARALLEL.md), with real ARM64 programs on
 * the interpreter (the programs in tests/guest):
 *
 *   1. Serial equivalence: one core (one host thread besides the driver)
 *      gives exactly the serial scheduler's result - the same slices,
 *      SVCs, virtual time and output - at every budget.
 *   2. Two and three cores: every program still completes correctly,
 *      including tests/guest/atomics.s, whose three workers increment
 *      shared counters with LDXR/STXR loops while really running at
 *      once (a lost update fails it), and repeated rounds of the
 *      condvar producer/consumer (a lost wakeup deadlocks it).
 *   3. Host calls from a core reach the driver thread (parallel_on_driver).
 */
#define CHECK_NAME "parallel_test"
#include "check.h"

#include "guest_fixture.h"
#include "guest/atomics.inc"
#include "guest/condvar.inc"
#include "guest/events.inc"
#include "guest/threads.inc"
#include "hle/kernel/parallel.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static Guest_Run g_run; /* Emulator is large; keep it off the stack */

typedef struct Program {
  const char *name;
  const uint8_t *code;
  size_t size;
  const char *expected;
} Program;

static const Program k_programs[] = {
    {"threads", k_guest_threads, sizeof(k_guest_threads), "counter 2000, sleep ok"},
    {"condvar", k_guest_condvar, sizeof(k_guest_condvar), "sum 125250, timeouts ok"},
    {"events", k_guest_events, sizeof(k_guest_events), "signal, reset, timeout, close ok"},
    {"atomics", k_guest_atomics, sizeof(k_guest_atomics), "counters 150000 ok"},
};
#define PROGRAM_COUNT (sizeof(k_programs) / sizeof(k_programs[0]))
#define MAX_SLICES 20000000ull

typedef struct Outcome {
  Emulator_Status status;
  uint64_t slices, svcs, ticks;
  char output[GUEST_OUTPUT_BYTES];
} Outcome;

static void run_program(const Program *program, uint32_t cores, uint64_t budget, Outcome *out) {
  guest_boot(&g_run, &CPU_BACKEND_INTERPRETER, program->code, program->size);
  if (cores) CHECK(emulator_set_host_cores(&g_run.emu, cores) == cores);
  out->status = guest_run(&g_run, budget, MAX_SLICES);
  out->slices = g_run.slices;
  out->svcs = g_run.emu.hle.svc_call_count;
  out->ticks = g_run.emu.scheduler.ticks;
  memcpy(out->output, g_run.output, sizeof(out->output));
  guest_shutdown(&g_run);
}

static void check_completes(const Program *program, uint32_t cores, uint64_t budget, const Outcome *o) {
  if (o->status == EMULATOR_EXITED && strstr(o->output, program->expected) != NULL) return;
  fprintf(stderr, "%s, %u core(s), budget %llu: status %d after %llu slices, output: %s\n", program->name, cores,
          (unsigned long long)budget, (int)o->status, (unsigned long long)o->slices, o->output);
  CHECK(false);
}

static void test_serial_equivalence(void) {
  static const uint64_t budgets[] = {7, 31, 100, 997, 100000};
  static Outcome serial, one;
  for (size_t p = 0; p < PROGRAM_COUNT; p++)
  for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
    run_program(&k_programs[p], 0, budgets[i], &serial);
    run_program(&k_programs[p], 1, budgets[i], &one);
    check_completes(&k_programs[p], 0, budgets[i], &serial);
    if (serial.slices != one.slices || serial.svcs != one.svcs || serial.ticks != one.ticks ||
        strcmp(serial.output, one.output) != 0) {
      fprintf(stderr, "%s budget %llu: serial %llu slices %llu SVCs %llu ticks, one core %llu / %llu / %llu\n",
              k_programs[p].name, (unsigned long long)budgets[i], (unsigned long long)serial.slices,
              (unsigned long long)serial.svcs, (unsigned long long)serial.ticks, (unsigned long long)one.slices,
              (unsigned long long)one.svcs, (unsigned long long)one.ticks);
      CHECK(false);
    }
  }
  printf("[parallel_test] one core == serial for %zu programs x %zu budgets\n", PROGRAM_COUNT,
         sizeof(budgets) / sizeof(budgets[0]));
}

static void test_multicore(void) {
  static const uint64_t budgets[] = {31, 997, 100000};
  static Outcome o;
  for (uint32_t cores = 2; cores <= 3; cores++)
  for (size_t p = 0; p < PROGRAM_COUNT; p++)
  for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
    run_program(&k_programs[p], cores, budgets[i], &o);
    check_completes(&k_programs[p], cores, budgets[i], &o);
    printf("[parallel_test] %-8s %u cores budget %6llu: %llu slices, %llu SVCs, virtual time %llu ticks\n",
           k_programs[p].name, cores, (unsigned long long)budgets[i], (unsigned long long)o.slices,
           (unsigned long long)o.svcs, (unsigned long long)o.ticks);
  }
}

/* Races are rare events: give them chances. atomics.s checks exclusives
 * (a lost update fails it); condvar.s hands 500 items between two threads
 * through a mutex and two condition variables, so a lost wakeup in the
 * kernel's condvar/mutex handling (svc_thread.c) deadlocks it. */
static void test_stress(void) {
  static Outcome o;
  static const struct { size_t program; uint32_t cores; uint64_t budget; } runs[] = {
      {3, 3, 50000}, {1, 2, 997}, {1, 3, 997}, {1, 2, 100000},
  };
  for (size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
    for (uint32_t round = 0; round < 20u; round++) {
      run_program(&k_programs[runs[k].program], runs[k].cores, runs[k].budget, &o);
      check_completes(&k_programs[runs[k].program], runs[k].cores, runs[k].budget, &o);
    }
    printf("[parallel_test] %s: 20 rounds on %u cores (budget %llu), no lost update or wakeup\n",
           k_programs[runs[k].program].name, runs[k].cores, (unsigned long long)runs[k].budget);
  }
}

static pthread_t g_driver;
static bool g_ran_on_driver;
static void note_thread(void *ctx) {
  (void)ctx;
  g_ran_on_driver = pthread_equal(pthread_self(), g_driver);
}

/* A host call made from a core runs on the driver: hook it into an SVC
 * by way of the debug-output callback, which runs inside
 * svcOutputDebugString on a core thread. */
static void proxy_output(void *userdata, const char *text, size_t length) {
  Guest_Run *run = (Guest_Run *)userdata;
  if (parallel_on_core_thread()) parallel_on_driver(note_thread, NULL);
  if (run->output_length + length + 1 < sizeof(run->output)) {
    memcpy(run->output + run->output_length, text, length);
    run->output_length += length;
    run->output[run->output_length++] = '\n';
    run->output[run->output_length] = '\0';
  }
}

static void test_host_calls(void) {
  g_driver = pthread_self();
  g_ran_on_driver = false;
  guest_boot(&g_run, &CPU_BACKEND_INTERPRETER, k_guest_threads, sizeof(k_guest_threads));
  CHECK(emulator_set_host_cores(&g_run.emu, 2) == 2);
  emulator_set_debug_output(&g_run.emu, proxy_output, &g_run);
  CHECK(guest_run(&g_run, 997, MAX_SLICES) == EMULATOR_EXITED);
  CHECK(strstr(g_run.output, "counter 2000") != NULL);
  CHECK(g_ran_on_driver);
  guest_shutdown(&g_run);
  printf("[parallel_test] host calls from a core run on the driver\n");
}

int main(void) {
  if (!parallel_supported()) {
    printf("[parallel_test] no host threads in this build; skipped\n");
    return 0;
  }
  test_serial_equivalence();
  test_multicore();
  test_stress();
  test_host_calls();
  printf("[parallel_test] passed\n");
  return 0;
}
