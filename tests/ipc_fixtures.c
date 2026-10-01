#define CHECK_NAME "ipc_fixtures"
#include "ipc_fixtures.h"

#include "check.h"
#include "hle/loader/exefs.h"
#include "hle/loader/nca_parse.h"
#include "loader_fixtures.h"

#include <string.h>

#define FIXTURE_PROGRAM_ID 0x0100000000044000ull
#define SVC_SEND_SYNC_REQUEST 0x21u
#define SVC_CONNECT_TO_NAMED_PORT 0x1Fu
#define NAME_SCRATCH_OFFSET 0x100u /* into the main-thread stack */

/* libnx sf/hipc.h + sf/cmif.h, restated. */
#define L_HIPC_TYPE_REQUEST 4u
#define L_HIPC_TYPE_CONTROL 5u
#define L_HIPC_TYPE_CLOSE 2u
#define L_TIPC_BASE 16u
#define L_SFCI 0x49434653u
#define L_SFCO 0x4F434653u

static const uint32_t k_caps[] = {0x7u | (44u << 4) | (28u << 10) | (0u << 16) | (2u << 24),
                                  0xFu | (0xFFu << 5)};

uint32_t test_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t test_le64(const uint8_t *p) { return (uint64_t)test_le32(p) | ((uint64_t)test_le32(p + 4) << 32); }

static void put32(uint8_t *p, uint32_t v) { fixture_put_le32(p, v); }

void ipc_fixture_boot(Emulator *emu) {
  static uint8_t text[0x1000];
  memset(text, 0xC3, sizeof(text));
  Fixture_NSO_Params nso;
  memset(&nso, 0, sizeof(nso));
  nso.text = (Fixture_NSO_Segment){text, sizeof(text), 0, true, false};
  nso.rodata = (Fixture_NSO_Segment){"", 0, 0x1000, false, false};
  nso.data = (Fixture_NSO_Segment){"", 0, 0x1000, false, false};
  Fixture_Buffer nso_image;
  fixture_build_nso(&nso, &nso_image);

  Fixture_NPDM_Params npdm;
  memset(&npdm, 0, sizeof(npdm));
  npdm.flags = 0x01 | (3 << 1);
  npdm.main_thread_priority = 44;
  npdm.main_thread_stack_size = 0x80000;
  npdm.name = "IpcTest";
  npdm.program_id_min = FIXTURE_PROGRAM_ID;
  npdm.program_id_max = FIXTURE_PROGRAM_ID;
  npdm.acid_capabilities = k_caps;
  npdm.acid_capability_count = 2;
  npdm.program_id = FIXTURE_PROGRAM_ID;
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
  fixture_build_nca(NCA_MAGIC_NCA3, 0, FIXTURE_PROGRAM_ID, sections, &nca_image);

  CHECK_OK(emulator_create(emu));
  const Byte_Source source = byte_source_from_memory(nca_image.bytes, nca_image.size);
  CHECK_OK(emulator_load_program(emu, &source, 0));

  /* nca_image stays allocated: the program's file must stay readable
   * while it runs (emulator.h). */
  fixture_buffer_free(&exefs_image);
  fixture_buffer_free(&npdm_image);
  fixture_buffer_free(&nso_image);
}

uint64_t ipc_fixture_tls(const Emulator *emu) {
  return emu->cpu_backend->get_sys_reg(emu->cpu_state, CPU_SYSREG_TPIDRRO_EL0);
}

static uint32_t put_buffer_descriptor(uint8_t *p, const Test_Ipc_Buffer *b) {
  put32(p, (uint32_t)b->size);
  put32(p + 4, (uint32_t)b->address);
  put32(p + 8, (uint32_t)(b->mode_or_index & 3u) | (uint32_t)((b->address >> 36) << 2) |
                   (uint32_t)(((b->size >> 32) & 0xFu) << 24) |
                   (uint32_t)(((b->address >> 32) & 0xFu) << 28));
  return 12;
}

void test_ipc_build(const Test_Ipc_Message *m, uint8_t out[TEST_IPC_BUFFER_BYTES]) {
  memset(out, 0, TEST_IPC_BUFFER_BYTES);
  uint32_t type = 0;
  uint32_t data_bytes = 0;
  switch (m->framing) {
  case TEST_IPC_CMIF:
    type = L_HIPC_TYPE_REQUEST;
    /* cmifMakeRequest: 16 padding + [16 domain + 4/in-object] + 16 header + payload */
    data_bytes = 16u + (m->domain ? 16u + 4u * m->in_object_count : 0u) + 16u + m->payload_size;
    break;
  case TEST_IPC_CONTROL:
    type = L_HIPC_TYPE_CONTROL;
    data_bytes = 16u + 16u + m->payload_size;
    break;
  case TEST_IPC_CMIF_CLOSE:
    type = L_HIPC_TYPE_CLOSE;
    data_bytes = 0;
    break;
  case TEST_IPC_TIPC:
    type = L_TIPC_BASE + m->command_id;
    data_bytes = m->payload_size;
    break;
  }
  const uint32_t data_words = (data_bytes + 3u) / 4u;
  const bool special = m->send_pid || m->copy_count || m->move_count;
  const uint32_t recv_mode = m->receive_list_count ? 2u + m->receive_list_count : 0u;
  put32(out, type | (m->static_count << 16) | (m->send_count << 20) | (m->receive_count << 24) |
                 (m->exchange_count << 28));
  put32(out + 4, data_words | (recv_mode << 10) | (special ? 1u << 31 : 0u));

  uint32_t o = 8;
  if (special) {
    put32(out + o, (m->send_pid ? 1u : 0u) | (m->copy_count << 1) | (m->move_count << 5));
    o += 4;
    if (m->send_pid) o += 8; /* placeholder, the kernel fills it */
    for (uint32_t i = 0; i < m->copy_count; i++, o += 4) put32(out + o, m->copy_handles[i]);
    for (uint32_t i = 0; i < m->move_count; i++, o += 4) put32(out + o, m->move_handles[i]);
  }
  for (uint32_t i = 0; i < m->static_count; i++, o += 8) {
    const Test_Ipc_Buffer *b = &m->statics[i];
    put32(out + o, (uint32_t)(b->mode_or_index & 0x3Fu) | (uint32_t)(((b->address >> 36) & 0x3Fu) << 6) |
                       (uint32_t)(((b->address >> 32) & 0xFu) << 12) | (uint32_t)(b->size << 16));
    put32(out + o + 4, (uint32_t)b->address);
  }
  for (uint32_t i = 0; i < m->send_count; i++) o += put_buffer_descriptor(out + o, &m->sends[i]);
  for (uint32_t i = 0; i < m->receive_count; i++) o += put_buffer_descriptor(out + o, &m->receives[i]);
  for (uint32_t i = 0; i < m->exchange_count; i++) o += put_buffer_descriptor(out + o, &m->exchanges[i]);

  const uint32_t data_offset = o;
  if (m->framing == TEST_IPC_TIPC) {
    if (m->payload_size) memcpy(out + data_offset, m->payload, m->payload_size);
  } else if (m->framing != TEST_IPC_CMIF_CLOSE) {
    uint32_t p = (data_offset + 15u) & ~15u;
    if (m->framing == TEST_IPC_CMIF && m->domain) {
      out[p] = m->domain_type;
      out[p + 1] = (uint8_t)m->in_object_count;
      fixture_put_le16(out + p + 2, (uint16_t)(16u + m->payload_size));
      put32(out + p + 4, m->object_id);
      p += 16;
    }
    put32(out + p, L_SFCI);
    put32(out + p + 4, 0);
    put32(out + p + 8, m->command_id);
    put32(out + p + 12, m->token);
    p += 16;
    if (m->payload_size) memcpy(out + p, m->payload, m->payload_size);
    p += m->payload_size;
    for (uint32_t i = 0; i < m->in_object_count; i++, p += 4) put32(out + p, m->in_objects[i]);
  }
  o = data_offset + data_words * 4u;
  for (uint32_t i = 0; i < m->receive_list_count; i++, o += 8) {
    const Test_Ipc_Buffer *b = &m->receive_list[i];
    put32(out + o, (uint32_t)b->address);
    put32(out + o + 4, (uint32_t)((b->address >> 32) & 0xFFFFu) | (uint32_t)(b->size << 16));
  }
}

void test_ipc_parse_reply(const uint8_t buffer[TEST_IPC_BUFFER_BYTES], Test_Ipc_Framing framing,
                          bool domain, uint32_t out_data_size, Test_Ipc_Reply *out) {
  memset(out, 0, sizeof(*out));
  const uint32_t w1 = test_le32(buffer + 4);
  uint32_t o = 8;
  if (w1 >> 31) {
    const uint32_t special = test_le32(buffer + o);
    o += 4;
    if (special & 1u) o += 8;
    out->copy_count = (special >> 1) & 0xFu;
    out->move_count = (special >> 5) & 0xFu;
    for (uint32_t i = 0; i < out->copy_count && i < TEST_IPC_MAX; i++, o += 4) out->copy_handles[i] = test_le32(buffer + o);
    for (uint32_t i = 0; i < out->move_count && i < TEST_IPC_MAX; i++, o += 4) out->move_handles[i] = test_le32(buffer + o);
  }
  if (framing == TEST_IPC_TIPC) {
    out->sfco_ok = true;
    out->result = test_le32(buffer + o);
    out->data = buffer + o + 4;
    return;
  }
  uint32_t p = (o + 15u) & ~15u;
  if (domain) {
    out->object_count = test_le32(buffer + p);
    p += 16;
  }
  out->sfco_ok = test_le32(buffer + p) == L_SFCO;
  out->result = test_le32(buffer + p + 8);
  out->token = test_le32(buffer + p + 12);
  out->data = buffer + p + 16;
  if (domain) {
    for (uint32_t i = 0; i < out->object_count && i < TEST_IPC_MAX; i++) {
      out->object_ids[i] = test_le32(buffer + p + 16 + out_data_size + 4u * i);
    }
  }
}

uint32_t ipc_fixture_send(Emulator *emu, uint32_t handle, const Test_Ipc_Message *message,
                          uint8_t reply_buffer[TEST_IPC_BUFFER_BYTES]) {
  uint8_t request[TEST_IPC_BUFFER_BYTES];
  test_ipc_build(message, request);
  const uint64_t tls = ipc_fixture_tls(emu);
  CHECK_OK(vmm_write_block(emu->vmm, tls, request, sizeof(request)));
  CPU_Register_File *regs = emu->cpu_backend->get_register_file(emu->cpu_state);
  regs->x[0] = handle;
  hle_on_svc(emu->cpu_state, SVC_SEND_SYNC_REQUEST, &emu->hle);
  CHECK_OK(vmm_read_block(emu->vmm, tls, reply_buffer, TEST_IPC_BUFFER_BYTES));
  return (uint32_t)regs->x[0];
}

uint32_t ipc_fixture_connect(Emulator *emu, const char *name, uint32_t *out_handle) {
  const uint64_t gva = emu->process.main_thread_stack.base + NAME_SCRATCH_OFFSET;
  CHECK_OK(vmm_write_block(emu->vmm, gva, name, strlen(name) + 1u));
  CPU_Register_File *regs = emu->cpu_backend->get_register_file(emu->cpu_state);
  regs->x[0] = 0;
  regs->x[1] = gva;
  hle_on_svc(emu->cpu_state, SVC_CONNECT_TO_NAMED_PORT, &emu->hle);
  if (regs->x[0] == 0 && out_handle) *out_handle = (uint32_t)regs->x[1];
  return (uint32_t)regs->x[0];
}
