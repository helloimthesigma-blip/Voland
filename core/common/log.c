#include "common/log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/console.h>
/* The browser drops DEBUG/TRACE by default: Emscripten's stderr goes to
 * the console one character at a time through JS, and the boot alone
 * logs tens of thousands of debug lines (§24, perf). */
#define LOG_DEFAULT_MINIMUM_LEVEL LOG_LEVEL_INFO
/* One console call per line; longer lines are truncated. */
#define LOG_LINE_MAX_BYTES 1024
#else
#define LOG_DEFAULT_MINIMUM_LEVEL LOG_LEVEL_DEBUG
#endif

static Log_Level g_minimum_level = LOG_DEFAULT_MINIMUM_LEVEL;

static const char* level_name(Log_Level level) {
  switch (level) {
    case LOG_LEVEL_TRACE: return "TRACE";
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_INFO:  return "INFO ";
    case LOG_LEVEL_WARN:  return "WARN ";
    case LOG_LEVEL_ERROR: return "ERROR";
  }
  return "?????";
}

void log_set_minimum_level(Log_Level level) {
  g_minimum_level = level;
}

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/console.h>
#include <emscripten/threading.h>

#define LOG_LINE_BYTES 1024
/* Off the main runtime thread (a parallel guest core, docs/PARALLEL.md),
 * stderr is a syscall Emscripten proxies synchronously to the CPU worker,
 * which is waiting for the slice: print from this thread instead. */
static bool log_from_helper_thread(Log_Level level, const char* format, va_list args) {
  if (emscripten_is_main_runtime_thread()) return false;
  char line[LOG_LINE_BYTES];
  int length = snprintf(line, sizeof(line), "[%s] ", level_name(level));
  if (length > 0 && length < (int)sizeof(line)) {
    const int rest = vsnprintf(line + length, sizeof(line) - (size_t)length, format, args);
    if (rest > 0) length += rest;
  }
  if (length < 0) return true;
  emscripten_errn(line, length < (int)sizeof(line) ? (size_t)length : sizeof(line) - 1u);
  return true;
}
#else
static bool log_from_helper_thread(Log_Level level, const char* format, va_list args) {
  (void)level;
  (void)format;
  (void)args;
  return false;
}
#endif

/* The recent-problems ring: slots claimed with an atomic counter, so
 * logging from several host threads at once tears at worst one line. */
static char g_recent[LOG_RECENT_LINES][LOG_RECENT_LINE_BYTES];
static void count_problem(const char *line);
static unsigned g_recent_next;

static void remember_problem(Log_Level level, const char* format, va_list args) {
  const unsigned slot = __atomic_fetch_add(&g_recent_next, 1u, __ATOMIC_RELAXED) % LOG_RECENT_LINES;
  char *line = g_recent[slot];
  const int used = snprintf(line, LOG_RECENT_LINE_BYTES, "[%s] ", level_name(level));
  if (used > 0 && (size_t)used < LOG_RECENT_LINE_BYTES)
    (void)vsnprintf(line + used, LOG_RECENT_LINE_BYTES - (size_t)used, format, args);
  count_problem(line);
}

/* Distinct problems: open addressing by FNV-1a of the line, under a spin
 * lock (warnings are rare; several host threads may log at once). */
typedef struct Distinct_Problem {
  uint32_t hash;
  uint32_t order;   /* first-seen rank + 1; 0 = empty */
  uint64_t count;
  char line[LOG_RECENT_LINE_BYTES];
} Distinct_Problem;
#define DISTINCT_SLOTS (LOG_DISTINCT_PROBLEMS * 2u)
static Distinct_Problem g_distinct[DISTINCT_SLOTS];
static uint32_t g_distinct_count;
static uint64_t g_distinct_dropped;
static bool g_distinct_lock;

static void count_problem(const char *line) {
  uint32_t hash = 0x811c9dc5u;
  for (const char *p = line; *p; p++) hash = (hash ^ (uint8_t)*p) * 0x01000193u;
  while (__atomic_test_and_set(&g_distinct_lock, __ATOMIC_ACQUIRE)) {
  }
  for (uint32_t probe = 0; probe < DISTINCT_SLOTS; probe++) {
    Distinct_Problem *slot = &g_distinct[(hash + probe) % DISTINCT_SLOTS];
    if (slot->order && slot->hash == hash && strcmp(slot->line, line) == 0) {
      slot->count++;
      break;
    }
    if (!slot->order) {
      if (g_distinct_count >= LOG_DISTINCT_PROBLEMS) {
        g_distinct_dropped++;
        break;
      }
      slot->hash = hash;
      slot->order = ++g_distinct_count;
      slot->count = 1;
      snprintf(slot->line, sizeof(slot->line), "%s", line);
      break;
    }
  }
  __atomic_clear(&g_distinct_lock, __ATOMIC_RELEASE);
}

size_t log_distinct_problems(char *out, size_t capacity) {
  if (!out || !capacity) return 0;
  size_t used = 0;
  out[0] = '\0';
  while (__atomic_test_and_set(&g_distinct_lock, __ATOMIC_ACQUIRE)) {
  }
  for (uint32_t rank = 1; rank <= g_distinct_count; rank++) {
    for (uint32_t i = 0; i < DISTINCT_SLOTS; i++) {
      if (g_distinct[i].order != rank) continue;
      const int n = snprintf(out + used, capacity - used, "%llu\t%s\n", (unsigned long long)g_distinct[i].count,
                             g_distinct[i].line);
      if (n > 0 && (size_t)n < capacity - used) used += (size_t)n;
      break;
    }
  }
  if (g_distinct_dropped) {
    const int n = snprintf(out + used, capacity - used, "%llu\t(other distinct problems not kept)\n",
                           (unsigned long long)g_distinct_dropped);
    if (n > 0 && (size_t)n < capacity - used) used += (size_t)n;
  }
  __atomic_clear(&g_distinct_lock, __ATOMIC_RELEASE);
  return used;
}

size_t log_recent_problems(char *out, size_t capacity) {
  if (!out || !capacity) return 0;
  size_t used = 0;
  out[0] = '\0';
  const unsigned next = __atomic_load_n(&g_recent_next, __ATOMIC_RELAXED);
  const unsigned count = next < LOG_RECENT_LINES ? next : LOG_RECENT_LINES;
  for (unsigned i = 0; i < count; i++) {
    const char *line = g_recent[(next - count + i) % LOG_RECENT_LINES];
    const int n = snprintf(out + used, capacity - used, "%s\n", line);
    if (n < 0 || (size_t)n >= capacity - used) break;
    used += (size_t)n;
  }
  return used;
}

void log_message(Log_Level level, const char* format, ...) {
  if (level >= LOG_LEVEL_WARN) {
    va_list args;
    va_start(args, format);
    remember_problem(level, format, args);
    va_end(args);
  }
  if (level < g_minimum_level) return;
  {
    va_list args;
    va_start(args, format);
    const bool done = log_from_helper_thread(level, format, args);
    va_end(args);
    if (done) return;
  }

#ifdef __EMSCRIPTEN__
  char line[LOG_LINE_MAX_BYTES];
  int used = snprintf(line, sizeof(line), "[%s] ", level_name(level));
  if (used < 0) return;
  va_list args;
  va_start(args, format);
  (void)vsnprintf(line + used, sizeof(line) - (size_t)used, format, args);
  va_end(args);
  emscripten_err(line);
#else
  fprintf(stderr, "[%s] ", level_name(level));

  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);

  fputc('\n', stderr);
#endif
}
