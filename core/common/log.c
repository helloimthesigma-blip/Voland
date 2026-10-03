#include "common/log.h"

#include <stdarg.h>
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

void log_message(Log_Level level, const char* format, ...) {
  if (level < g_minimum_level) return;

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
