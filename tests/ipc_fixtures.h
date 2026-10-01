/**
 * Test-side IPC: a booted emulator to send requests from, and an
 * independent HIPC/CMIF/TIPC encoder + reply decoder.
 *
 * The encoder restates the wire layout from libnx's sf/hipc.h,
 * sf/cmif.h and sf/tipc.h (hipcMakeRequest / cmifMakeRequest /
 * cmifParseResponse) rather than reusing core/hle/kernel/ipc.c's
 * constants, so a parser that misreads the format cannot be validated by
 * a fixture sharing the misreading (same rule as loader_fixtures.h).
 */
#ifndef VOLAND_TESTS_IPC_FIXTURES_H
#define VOLAND_TESTS_IPC_FIXTURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emulator.h"

#define TEST_IPC_BUFFER_BYTES 0x100u
#define TEST_IPC_MAX 8u

/* Boots a one-module synthetic program through emulator_load_program, so
 * the main thread's TLS block and tpidrro_el0 are exactly as a title
 * would see them. */
void ipc_fixture_boot(Emulator *emu);

/* The main thread's TLS block address. */
uint64_t ipc_fixture_tls(const Emulator *emu);

typedef enum Test_Ipc_Framing {
  TEST_IPC_CMIF,   /* HIPC type 4 + SFCI */
  TEST_IPC_CONTROL, /* HIPC type 5 + SFCI */
  TEST_IPC_CMIF_CLOSE,
  TEST_IPC_TIPC,   /* HIPC type 16 + command */
} Test_Ipc_Framing;

typedef struct Test_Ipc_Buffer {
  uint64_t address;
  uint64_t size;
  uint8_t mode_or_index;
} Test_Ipc_Buffer;

typedef struct Test_Ipc_Message {
  Test_Ipc_Framing framing;
  uint32_t command_id;
  uint32_t token;
  bool send_pid;
  uint32_t copy_handles[TEST_IPC_MAX];
  uint32_t copy_count;
  uint32_t move_handles[TEST_IPC_MAX];
  uint32_t move_count;
  Test_Ipc_Buffer statics[TEST_IPC_MAX]; /* X: mode_or_index = index */
  uint32_t static_count;
  Test_Ipc_Buffer sends[TEST_IPC_MAX];   /* A: mode_or_index = mode */
  uint32_t send_count;
  Test_Ipc_Buffer receives[TEST_IPC_MAX]; /* B */
  uint32_t receive_count;
  Test_Ipc_Buffer exchanges[TEST_IPC_MAX]; /* W */
  uint32_t exchange_count;
  Test_Ipc_Buffer receive_list[TEST_IPC_MAX]; /* C */
  uint32_t receive_list_count;
  /* Domain framing (CMIF requests only). */
  bool domain;
  uint8_t domain_type; /* 1 SendMessage, 2 Close */
  uint32_t object_id;
  uint32_t in_objects[TEST_IPC_MAX];
  uint32_t in_object_count;
  const void *payload;
  uint32_t payload_size;
} Test_Ipc_Message;

void test_ipc_build(const Test_Ipc_Message *message, uint8_t out[TEST_IPC_BUFFER_BYTES]);

typedef struct Test_Ipc_Reply {
  bool sfco_ok;  /* CMIF: SFCO magic found */
  uint32_t result;
  uint32_t token;
  const uint8_t *data; /* points into the caller's buffer */
  uint32_t copy_handles[TEST_IPC_MAX];
  uint32_t copy_count;
  uint32_t move_handles[TEST_IPC_MAX];
  uint32_t move_count;
  uint32_t object_count; /* domain */
  uint32_t object_ids[TEST_IPC_MAX];
} Test_Ipc_Reply;

/* Decodes a reply the way libnx does. `out_data_size` (the size the
 * caller expects, as libnx's out_size) locates domain object ids. */
void test_ipc_parse_reply(const uint8_t buffer[TEST_IPC_BUFFER_BYTES], Test_Ipc_Framing framing,
                          bool domain, uint32_t out_data_size, Test_Ipc_Reply *out);

/* Writes `message` into the main thread's TLS, issues SendSyncRequest on
 * `handle` through hle_on_svc, reads the TLS back into `reply_buffer`.
 * Returns W0. */
uint32_t ipc_fixture_send(Emulator *emu, uint32_t handle, const Test_Ipc_Message *message,
                          uint8_t reply_buffer[TEST_IPC_BUFFER_BYTES]);

/* ConnectToNamedPort over a name written into guest memory (the stack).
 * Returns W0; *out_handle = W1 on success. */
uint32_t ipc_fixture_connect(Emulator *emu, const char *name, uint32_t *out_handle);

/* Little-endian helpers. */
uint32_t test_le32(const uint8_t *p);
uint64_t test_le64(const uint8_t *p);

#endif /* VOLAND_TESTS_IPC_FIXTURES_H */
