/**
 * Writes a synthesized, fully plaintext PROGRAM NCA to the path given as
 * argv[1], then proves the written file loads by reading it back from
 * disk through a file-backed Byte_Source and running emulator_load_program
 * over it.
 *
 * The file is the browser e2e's "decrypted NCA" (platform/web/e2e/
 * load.spec.ts): the web load path - FileReaderSync over blob.slice()
 * into linear memory (§15) - needs a real file to pick, and the repository
 * must never contain Nintendo data (§1.6). Every byte here is synthetic:
 * a tiny rtld and main NSO, a main.npdm, and an ExeFS around them, all
 * from loader_fixtures.c.
 */
#define CHECK_NAME "program_nca_fixture"
#include "check.h"

#include "emulator.h"
#include "hle/loader/exefs.h"
#include "hle/loader/nca_parse.h"
#include "loader_fixtures.h"

#include <stdio.h>
#include <string.h>

/* Must match platform/web/e2e/load.spec.ts EXPECTED_TITLE_ID. */
#define FIXTURE_PROGRAM_ID 0x0100000000042000ull

static const uint32_t k_caps[] = {0x7u | (44u << 4) | (28u << 10) | (0u << 16) | (2u << 24),
                                  0xFu | (0xFFu << 5)};
static uint8_t k_rtld_text[0x200];
static uint8_t k_main_text[0x1000];
static const char k_main_data[] = "voland-e2e-fixture";

static void build_program_nca(Fixture_Buffer *nca_image) {
  Fixture_NPDM_Params npdm_params;
  memset(&npdm_params, 0, sizeof(npdm_params));
  npdm_params.flags = 0x01 | (3 << 1); /* 64-bit, 39-bit address space */
  npdm_params.main_thread_priority = 44;
  npdm_params.main_thread_stack_size = 0x80000;
  npdm_params.name = "VolandE2E";
  npdm_params.program_id_min = FIXTURE_PROGRAM_ID;
  npdm_params.program_id_max = FIXTURE_PROGRAM_ID;
  npdm_params.acid_capabilities = k_caps;
  npdm_params.acid_capability_count = 2;
  npdm_params.program_id = FIXTURE_PROGRAM_ID;
  npdm_params.aci0_capabilities = k_caps;
  npdm_params.aci0_capability_count = 2;
  Fixture_Buffer npdm_image;
  fixture_build_npdm(&npdm_params, &npdm_image);

  for (size_t i = 0; i < sizeof(k_rtld_text); i++) k_rtld_text[i] = (uint8_t)(0xD4 + i % 5);
  Fixture_NSO_Params rtld_params;
  memset(&rtld_params, 0, sizeof(rtld_params));
  rtld_params.text = (Fixture_NSO_Segment){k_rtld_text, sizeof(k_rtld_text), 0, true, false};
  rtld_params.rodata = (Fixture_NSO_Segment){"", 0, 0x1000, false, false};
  rtld_params.data = (Fixture_NSO_Segment){"", 0, 0x1000, false, false};
  Fixture_Buffer rtld_nso_image;
  fixture_build_nso(&rtld_params, &rtld_nso_image);

  for (size_t i = 0; i < sizeof(k_main_text); i++) k_main_text[i] = (uint8_t)(i % 17);
  Fixture_NSO_Params main_params;
  memset(&main_params, 0, sizeof(main_params));
  main_params.text = (Fixture_NSO_Segment){k_main_text, sizeof(k_main_text), 0, true, false};
  main_params.rodata = (Fixture_NSO_Segment){"", 0, 0x1000, false, false};
  main_params.data = (Fixture_NSO_Segment){k_main_data, sizeof(k_main_data), 0x1000, false, false};
  main_params.bss_size = 0x100;
  Fixture_Buffer main_nso_image;
  fixture_build_nso(&main_params, &main_nso_image);

  const Fixture_File exefs_files[] = {
      {EXEFS_FILE_NPDM, npdm_image.bytes, npdm_image.size},
      {EXEFS_FILE_RTLD, rtld_nso_image.bytes, rtld_nso_image.size},
      {EXEFS_FILE_MAIN, main_nso_image.bytes, main_nso_image.size},
  };
  Fixture_Buffer exefs_image;
  fixture_build_pfs0(exefs_files, 3, &exefs_image);

  Fixture_NCA_Section sections[4];
  memset(sections, 0, sizeof(sections));
  sections[0] = (Fixture_NCA_Section){true, FIXTURE_NCA_FS_TYPE_PFS0, FIXTURE_NCA_HASH_SHA256, 3, &exefs_image, 0x200};
  fixture_build_nca(NCA_MAGIC_NCA3, 0, FIXTURE_PROGRAM_ID, sections, nca_image);

  fixture_buffer_free(&exefs_image);
  fixture_buffer_free(&main_nso_image);
  fixture_buffer_free(&rtld_nso_image);
  fixture_buffer_free(&npdm_image);
}

/* byte_source.h `read` over a stdio FILE: the native analogue of the web
 * worker's FileReaderSync hook. */
static Error file_read(void *user, uint64_t offset, void *out, uint64_t size) {
  FILE *file = (FILE *)user;
  if (fseek(file, (long)offset, SEEK_SET) != 0) return ERR(RESULT_IO_ERROR, "seek failed");
  if (fread(out, 1, (size_t)size, file) != (size_t)size) return ERR(RESULT_IO_ERROR, "short read");
  return OK;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <output.nca>\n", argv[0]);
    return 2;
  }

  Fixture_Buffer nca_image;
  build_program_nca(&nca_image);
  FILE *out = fopen(argv[1], "wb");
  CHECK(out != NULL);
  CHECK(fwrite(nca_image.bytes, 1, nca_image.size, out) == nca_image.size);
  CHECK(fclose(out) == 0);
  const uint64_t written_size = nca_image.size;
  fixture_buffer_free(&nca_image);

  /* Read it back from disk, not from the buffer it was written from. */
  FILE *in = fopen(argv[1], "rb");
  CHECK(in != NULL);
  const Byte_Source source = {.user = in, .size = written_size, .read = file_read};
  Emulator emu;
  CHECK_OK(emulator_create(&emu));
  CHECK_OK(emulator_load_program(&emu, &source, 0x5EED));
  CHECK(emu.program_loaded);
  CHECK(emu.process.npdm.program_id == FIXTURE_PROGRAM_ID);
  CHECK(emu.cpu_backend->get_pc(emu.cpu_state) == emu.process.entry_point);
  emulator_destroy(&emu);
  CHECK(fclose(in) == 0);

  printf("[program_nca_fixture] wrote and loaded %s (%llu bytes)\n", argv[1],
         (unsigned long long)written_size);
  return 0;
}
