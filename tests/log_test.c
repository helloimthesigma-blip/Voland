/**
 * core/common/log: the compatibility report's distinct-problem table
 * counts each distinct warning/error line once, first seen first, and the
 * recent ring keeps the newest lines.
 */
#define CHECK_NAME "log_test"
#include "check.h"

#include "common/log.h"

#include <stdio.h>
#include <string.h>

static char g_out[64 * 1024];

int main(void) {
  log_set_minimum_level(LOG_LEVEL_ERROR); /* keep the test's stderr quiet */
  log_warn("[ipc] fsp-srv: unimplemented command %u", 61u);
  log_error("[hle] svcBreak reason=0x%x", 7u);
  log_warn("[ipc] fsp-srv: unimplemented command %u", 61u);
  log_warn("[ipc] fsp-srv: unimplemented command %u", 62u);
  log_info("not a problem");
  log_distinct_problems(g_out, sizeof(g_out));
  CHECK(strcmp(g_out,
               "2\t[WARN ] [ipc] fsp-srv: unimplemented command 61\n"
               "1\t[ERROR] [hle] svcBreak reason=0x7\n"
               "1\t[WARN ] [ipc] fsp-srv: unimplemented command 62\n") == 0);
  /* Past the table's capacity new lines are only counted. */
  for (unsigned i = 0; i < LOG_DISTINCT_PROBLEMS + 5u; i++) log_warn("distinct %u", i);
  const size_t n = log_distinct_problems(g_out, sizeof(g_out));
  CHECK(n > 0 && strstr(g_out, "8\t(other distinct problems not kept)\n") != NULL);
  /* The recent ring ends with the newest line. */
  log_recent_problems(g_out, sizeof(g_out));
  char last[64];
  snprintf(last, sizeof(last), "distinct %u\n", LOG_DISTINCT_PROBLEMS + 4u);
  CHECK(strlen(g_out) >= strlen(last) && strcmp(g_out + strlen(g_out) - strlen(last), last) == 0);
  printf("[log_test] passed\n");
  return 0;
}
