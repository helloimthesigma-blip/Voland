#define CHECK_NAME "guest_fixture"
#include "guest_fixture.h"

#include "check.h"
#include "hle/loader/exefs.h"
#include "hle/loader/nca_parse.h"
#include "loader_fixtures.h"

#include <string.h>

#define GUEST_PROGRAM_ID 0x0100000000045000ull
#define GUEST_STACK_BYTES 0x100000u
#define GUEST_PRIORITY 44u

static const uint32_t k_caps[] = {0x7u | (44u << 4) | (28u << 10) | (0u << 16) | (2u << 24),
                                  0xFu | (0xFFu << 5)};

static void capture(void *userdata, const char *text, size_t length) {
  Guest_Run *run = (Guest_Run *)userdata;
  if (run->output_length + length + 2u >= GUEST_OUTPUT_BYTES) return;
  memcpy(run->output + run->output_length, text, length);
  run->output_length += length;
  run->output[run->output_length++] = '\n';
  run->output[run->output_length] = '\0';
}

void guest_boot(Guest_Run *run, const CPU_Backend *backend, const uint8_t *code, size_t size) {
  memset(run, 0, sizeof(*run));
  Fixture_NSO_Params nso;
  memset(&nso, 0, sizeof(nso));
  const size_t text_bytes = (size + 0xFFFu) & ~(size_t)0xFFF;
  nso.text = (Fixture_NSO_Segment){code, size, 0, false, false};
  nso.rodata = (Fixture_NSO_Segment){"", 0, (uint32_t)text_bytes, false, false};
  nso.data = (Fixture_NSO_Segment){"", 0, (uint32_t)text_bytes, false, false};
  Fixture_Buffer nso_image;
  fixture_build_nso(&nso, &nso_image);

  Fixture_NPDM_Params npdm;
  memset(&npdm, 0, sizeof(npdm));
  npdm.flags = 0x01 | (3 << 1);
  npdm.main_thread_priority = GUEST_PRIORITY;
  npdm.main_thread_stack_size = GUEST_STACK_BYTES;
  npdm.name = "GuestTest";
  npdm.program_id_min = GUEST_PROGRAM_ID;
  npdm.program_id_max = GUEST_PROGRAM_ID;
  npdm.acid_capabilities = k_caps;
  npdm.acid_capability_count = 2;
  npdm.program_id = GUEST_PROGRAM_ID;
  npdm.aci0_capabilities = k_caps;
  npdm.aci0_capability_count = 2;
  Fixture_Buffer npdm_image;
  fixture_build_npdm(&npdm, &npdm_image);

  const Fixture_File files[] = {
      {EXEFS_FILE_NPDM, npdm_image.bytes, npdm_image.size},
      {EXEFS_FILE_MAIN, nso_image.bytes, nso_image.size},
  };
  Fixture_Buffer exefs_image;
  fixture_build_pfs0(files, 2, &exefs_image);
  Fixture_NCA_Section sections[4];
  memset(sections, 0, sizeof(sections));
  sections[0] = (Fixture_NCA_Section){true, FIXTURE_NCA_FS_TYPE_PFS0, FIXTURE_NCA_HASH_SHA256, 3, &exefs_image, 0x200};
  Fixture_Buffer nca_image;
  fixture_build_nca(NCA_MAGIC_NCA3, 0, GUEST_PROGRAM_ID, sections, &nca_image);

  CHECK_OK(emulator_create_with_backend(&run->emu, backend));
  emulator_set_debug_output(&run->emu, capture, run);
  const Byte_Source source = byte_source_from_memory(nca_image.bytes, nca_image.size);
  CHECK_OK(emulator_load_program(&run->emu, &source, 0));

  fixture_buffer_free(&nca_image);
  fixture_buffer_free(&exefs_image);
  fixture_buffer_free(&npdm_image);
  fixture_buffer_free(&nso_image);
}

Emulator_Status guest_run(Guest_Run *run, uint64_t budget, uint64_t max_slices) {
  Emulator_Status status = EMULATOR_RUNNING;
  while (run->slices < max_slices) {
    status = emulator_run_slice(&run->emu, budget);
    run->slices++;
    if (status != EMULATOR_RUNNING && status != EMULATOR_IDLE) break;
  }
  run->final_status = status;
  return status;
}

void guest_shutdown(Guest_Run *run) { emulator_destroy(&run->emu); }
