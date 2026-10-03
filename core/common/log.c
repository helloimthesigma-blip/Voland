#include "common/log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

static Log_Level g_minimum_level = LOG_LEVEL_DEBUG;

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

void log_message(Log_Level level, const char* format, ...) {
  if (level < g_minimum_level) return;
  {
    va_list args;
    va_start(args, format);
    const bool done = log_from_helper_thread(level, format, args);
    va_end(args);
    if (done) return;
  }

  fprintf(stderr, "[%s] ", level_name(level));

  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);

  fputc('\n', stderr);
}
