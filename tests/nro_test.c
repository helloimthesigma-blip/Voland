/**
 * NRO loading (§12 homebrew) end to end: the demo program
 * (tests/guest/hello.s, built into platform/web/public/demo/hello.nro by
 * tools/guest_asm.py) loads through emulator_load_nro and runs on the
 * interpreter - integer, FP, NEON and a second thread, each line checked.
 * Plus nro_open's rejections.
 */
#define CHECK_NAME "nro_test"
#include "check.h"

#include "emulator.h"
#include "hle/loader/nro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Emulator g_emu;
static char g_output[4096];
static size_t g_length;

static void capture(void *userdata, const char *text, size_t length) {
  (void)userdata;
  if (g_length + length + 2 >= sizeof(g_output)) return;
  memcpy(g_output + g_length, text, length);
  g_length += length;
  g_output[g_length++] = '\n';
  g_output[g_length] = '\0';
}

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  CHECK(f != NULL);
  CHECK(fseek(f, 0, SEEK_END) == 0);
  *size = (size_t)ftell(f);
  CHECK(fseek(f, 0, SEEK_SET) == 0);
  uint8_t *bytes = malloc(*size);
  CHECK(bytes != NULL && fread(bytes, 1, *size, f) == *size);
  fclose(f);
  return bytes;
}

static void test_rejections(const uint8_t *nro, size_t size) {
  uint8_t *copy = malloc(size);
  CHECK(copy != NULL);
  NSO image;
  memcpy(copy, nro, size);
  Byte_Source source = byte_source_from_memory(copy, size);
  CHECK_OK(nro_open(&source, &image));
  CHECK(image.segments[NSO_SEGMENT_TEXT].memory_size >= 0x1000 && !image.segments[0].is_compressed);

  copy[0x10] = 'X'; /* magic */
  CHECK_CODE(nro_open(&source, &image), RESULT_INVALID_ARGUMENT);
  memcpy(copy, nro, size);
  copy[0x20] = 0x10; /* .text file offset no longer 0 */
  CHECK_CODE(nro_open(&source, &image), RESULT_INVALID_ARGUMENT);
  memcpy(copy, nro, size);
  copy[0x18] = 0xFF; copy[0x19] = 0xFF; copy[0x1A] = 0xFF; /* declared size past the file */
  CHECK_CODE(nro_open(&source, &image), RESULT_INVALID_ARGUMENT);
  Byte_Source short_source = byte_source_from_memory(nro, 0x40);
  CHECK_CODE(nro_open(&short_source, &image), RESULT_INVALID_ARGUMENT);
  free(copy);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  size_t size = 0;
  uint8_t *nro = read_file(argv[1], &size);
  test_rejections(nro, size);

  CHECK_OK(emulator_create_with_backend(&g_emu, &CPU_BACKEND_INTERPRETER));
  emulator_set_debug_output(&g_emu, capture, NULL);
  const Byte_Source source = byte_source_from_memory(nro, size);
  CHECK_OK(emulator_load_nro(&g_emu, &source, 0x1234));
  CHECK(g_emu.process.npdm.program_id == EMULATOR_HOMEBREW_PROGRAM_ID);
  CHECK_CODE(emulator_load_nro(&g_emu, &source, 0), RESULT_INVALID_ARGUMENT); /* one at a time */

  Emulator_Status status = EMULATOR_RUNNING;
  for (int i = 0; i < 100000 && (status == EMULATOR_RUNNING || status == EMULATOR_IDLE); i++) {
    status = emulator_run_slice(&g_emu, 1000);
  }
  printf("%s", g_output);
  CHECK(status == EMULATOR_EXITED);
  CHECK(strstr(g_output, "Hello from Voland!") != NULL);
  CHECK(strstr(g_output, "fib(90) = 2880067194370816120") != NULL);
  CHECK(strstr(g_output, "sqrt(2) x 10^9 = 1414213562") != NULL);
  CHECK(strstr(g_output, "= 70\n") != NULL);
  CHECK(strstr(g_output, "hello from a second guest thread") != NULL);
  CHECK(strstr(g_output, "Goodbye") != NULL);

  emulator_destroy(&g_emu);
  free(nro);
  printf("[nro_test] passed\n");
  return 0;
}
