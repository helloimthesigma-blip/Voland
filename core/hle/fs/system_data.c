#include "hle/fs/system_data.h"

#include <string.h>

#include "common/log.h"
#include "hle/fs/ramfs.h"
#include "hle/loader/nca_parse.h"
#include "hle/loader/romfs.h"

#include <stdio.h>

#define RD_DATA_OFFSET 0x200u      /* file data after the header, as packed images place it */
#define RD_ALIGN_DATA 0x10u        /* each file's data alignment */
#define RD_DIR_ENTRY_BYTES 0x18u   /* fixed part of a directory entry */
#define RD_FILE_ENTRY_BYTES 0x20u  /* fixed part of a file entry */
#define RD_MAX_FILES 16u
#define RD_ARCHIVE_BYTES 0x40000u   /* the MiiModel stand-in (~210 KiB) */

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1u) & ~(a - 1u); }

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }

uint64_t system_data_build_romfs(const System_Data_File *files, uint32_t count, uint8_t *out, uint64_t capacity) {
  if (count == 0 || count > RD_MAX_FILES) return 0;
  /* Sizes first: data, then the four tables. */
  uint64_t data_bytes = 0, file_table = 0;
  uint32_t entry_at[RD_MAX_FILES];
  for (uint32_t i = 0; i < count; i++) {
    data_bytes = align_up(data_bytes, RD_ALIGN_DATA) + files[i].size;
    entry_at[i] = (uint32_t)file_table;
    file_table += RD_FILE_ENTRY_BYTES + align_up(strlen(files[i].name), 4u);
  }
  const uint64_t dir_hash_at = align_up(RD_DATA_OFFSET + data_bytes, 4u), dir_hash_bytes = 4u;
  const uint64_t dir_at = dir_hash_at + dir_hash_bytes, dir_bytes = RD_DIR_ENTRY_BYTES;
  const uint64_t file_hash_at = dir_at + dir_bytes, file_hash_bytes = 4u * (uint64_t)count;
  const uint64_t file_at = file_hash_at + file_hash_bytes, total = file_at + file_table;
  if (total > capacity) return 0;
  memset(out, 0, (size_t)total);

  put64(out + 0x00, ROMFS_HEADER_SIZE);
  put64(out + 0x08, dir_hash_at);
  put64(out + 0x10, dir_hash_bytes);
  put64(out + 0x18, dir_at);
  put64(out + 0x20, dir_bytes);
  put64(out + 0x28, file_hash_at);
  put64(out + 0x30, file_hash_bytes);
  put64(out + 0x38, file_at);
  put64(out + 0x40, file_table);
  put64(out + 0x48, RD_DATA_OFFSET);

  /* The root directory: dir-table offset 0, empty name, every file a child. */
  put32(out + dir_hash_at, 0);
  uint8_t *root = out + dir_at;
  put32(root + 0x00, 0);
  put32(root + 0x04, ROMFS_NO_ENTRY);
  put32(root + 0x08, ROMFS_NO_ENTRY);
  put32(root + 0x0C, 0);
  put32(root + 0x10, ROMFS_NO_ENTRY);
  put32(root + 0x14, 0);

  for (uint32_t b = 0; b < count; b++) put32(out + file_hash_at + 4u * b, ROMFS_NO_ENTRY);
  uint64_t data = 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t name_length = (uint32_t)strlen(files[i].name);
    data = align_up(data, RD_ALIGN_DATA);
    memcpy(out + RD_DATA_OFFSET + data, files[i].bytes, files[i].size);
    uint8_t *e = out + file_at + entry_at[i];
    put32(e + 0x00, 0);
    put32(e + 0x04, i + 1u < count ? entry_at[i + 1u] : ROMFS_NO_ENTRY);
    put64(e + 0x08, data);
    put64(e + 0x10, files[i].size);
    const uint32_t bucket = romfs_path_hash(0, files[i].name, name_length) % count;
    uint32_t head;
    memcpy(&head, out + file_hash_at + 4u * bucket, sizeof(head));
    put32(e + 0x18, head); /* prepend to the bucket's chain */
    put32(out + file_hash_at + 4u * bucket, entry_at[i]);
    put32(e + 0x1C, name_length);
    memcpy(e + RD_FILE_ENTRY_BYTES, files[i].name, name_length);
    data += files[i].size;
  }
  return total;
}

/* ---- MiiModel ---------------------------------------------------------- */

/* The Mii resource layout (learned from the files' structure; no data of
 * Nintendo's is reproduced): a header - signature, u32 version, u32 total
 * size, then u32 per-part buffer sizes - and a fixed table of part entries
 * the Mii library indexes directly. A stand-in must be full size or the
 * library reads past its end (Mario Kart 8 faulted ~9 s in), and an entry
 * a Mii uses must hold real data: an empty texture crashed in NVN. So
 * every entry points at one tiny resource of our own, a zlib stream:
 * - texture entry (28 bytes): offset, size, compressed size, 0x806, 0x200,
 *   width | height << 16, format << 8 | mip levels. Ours: a black 8x8
 *   texture in the format the smallest real parts use.
 * - shape entry (0x58 bytes): offset, size, compressed size, 0x806, then
 *   the offsets and the sizes of five vertex streams (position, normal,
 *   texture coordinate and two unused) and of the 16-bit indices, then a
 *   float bounding box. Ours: one zero-area triangle at the origin.
 * Shapes also carry a block of part positions after the table, left zero.
 * Miis draw as nothing. */
#define MII_VERSION 1u
#define MII_HEADER_SIZE_AT 8u
#define MII_HEADER_SIZES_AT 12u
#define MII_ENTRY_TAG 0x806u
#define MII_TEXTURE_TABLE_AT 0x64u
#define MII_TEXTURE_ENTRIES 327u
#define MII_TEXTURE_ENTRY_BYTES 28u
#define MII_TEXTURE_DATA_AT (MII_TEXTURE_TABLE_AT + MII_TEXTURE_ENTRIES * MII_TEXTURE_ENTRY_BYTES)
#define MII_TEXTURE_ALIGN 0x200u
#define MII_TEXTURE_SIDE 8u
#define MII_TEXTURE_FORMAT 0x403u     /* format 4, 3 levels: the real 8x8 parts' */
#define MII_TEXTURE_PIXEL_BYTES 0x800u /* their decoded size */
#define MII_SHAPE_TABLE_AT 0xC4u
#define MII_SHAPE_ENTRIES 856u
#define MII_SHAPE_ENTRY_BYTES 0x58u
#define MII_SHAPE_POSITION_BYTES 0x26D0u
#define MII_SHAPE_DATA_AT (MII_SHAPE_TABLE_AT + MII_SHAPE_ENTRIES * MII_SHAPE_ENTRY_BYTES + MII_SHAPE_POSITION_BYTES)
#define MII_SHAPE_STREAMS 6u          /* position, normal, texcoord, two unused, indices */
#define MII_SHAPE_VERTICES 3u
#define MII_SHAPE_STREAM_ALIGN 0x10u
#define MII_BUFFER_BYTES 0x1000u      /* each per-part buffer size: room for ours */
/* A stored (uncompressed) zlib stream: 2-byte header, one final stored
 * block (1 + LEN + NLEN), the data, Adler-32. */
#define ZLIB_HEADER_CMF 0x78u
#define ZLIB_HEADER_FLG 0x01u
#define ZLIB_STORED_FINAL 0x01u
#define ZLIB_OVERHEAD 11u
#define ADLER_MOD 65521u

static const uint32_t k_shape_stream_bytes[MII_SHAPE_STREAMS] = {
    8u * MII_SHAPE_VERTICES, 4u * MII_SHAPE_VERTICES, 4u * MII_SHAPE_VERTICES, 0u, 0u, 2u * MII_SHAPE_VERTICES};

static uint32_t zlib_stored(uint8_t *out, const uint8_t *data, uint32_t size) {
  uint32_t a = 1u, b = 0u;
  for (uint32_t i = 0; i < size; i++) {
    a = (a + data[i]) % ADLER_MOD;
    b = (b + a) % ADLER_MOD;
  }
  out[0] = ZLIB_HEADER_CMF;
  out[1] = ZLIB_HEADER_FLG;
  out[2] = ZLIB_STORED_FINAL;
  out[3] = (uint8_t)size;
  out[4] = (uint8_t)(size >> 8);
  out[5] = (uint8_t)~size;
  out[6] = (uint8_t)(~size >> 8);
  memcpy(out + 7u, data, size);
  const uint32_t adler = (b << 16) | a;
  for (uint32_t i = 0; i < 4u; i++) out[7u + size + i] = (uint8_t)(adler >> (24u - 8u * i)); /* big-endian */
  return size + ZLIB_OVERHEAD;
}

static void mii_header(uint8_t *out, uint32_t size, uint32_t table_at, const char signature[4]) {
  memset(out, 0, size);
  memcpy(out, signature, 4u);
  put32(out + 4u, MII_VERSION);
  put32(out + MII_HEADER_SIZE_AT, size);
  for (uint32_t at = MII_HEADER_SIZES_AT; at < table_at; at += 4u) put32(out + at, MII_BUFFER_BYTES);
}

static uint32_t build_mii_texture(uint8_t *out) {
  static const uint8_t pixels[MII_TEXTURE_PIXEL_BYTES];
  const uint32_t packed = zlib_stored(out + MII_TEXTURE_DATA_AT, pixels, sizeof(pixels));
  const uint32_t size = MII_TEXTURE_DATA_AT + packed;
  mii_header(out, MII_TEXTURE_DATA_AT, MII_TEXTURE_TABLE_AT, "NFTR");
  put32(out + MII_HEADER_SIZE_AT, size);
  for (uint32_t i = 0; i < MII_TEXTURE_ENTRIES; i++) {
    uint8_t *e = out + MII_TEXTURE_TABLE_AT + i * MII_TEXTURE_ENTRY_BYTES;
    put32(e + 0x00, MII_TEXTURE_DATA_AT);
    put32(e + 0x04, MII_TEXTURE_PIXEL_BYTES);
    put32(e + 0x08, packed);
    put32(e + 0x0C, MII_ENTRY_TAG);
    put32(e + 0x10, MII_TEXTURE_ALIGN);
    put32(e + 0x14, MII_TEXTURE_SIDE | (MII_TEXTURE_SIDE << 16));
    put32(e + 0x18, MII_TEXTURE_FORMAT);
  }
  return size;
}

static uint32_t build_mii_shape(uint8_t *out) {
  uint8_t mesh[MII_SHAPE_STREAMS * MII_SHAPE_STREAM_ALIGN * 2u];
  memset(mesh, 0, sizeof(mesh));
  uint32_t at[MII_SHAPE_STREAMS], mesh_bytes = 0;
  for (uint32_t k = 0; k < MII_SHAPE_STREAMS; k++) {
    at[k] = k_shape_stream_bytes[k] ? (uint32_t)align_up(mesh_bytes, MII_SHAPE_STREAM_ALIGN) : 0u;
    if (k_shape_stream_bytes[k]) mesh_bytes = at[k] + k_shape_stream_bytes[k];
  }
  /* Indices 0, 1, 2 (the vertices all zero: a zero-area triangle). */
  const uint32_t indices = at[MII_SHAPE_STREAMS - 1u];
  for (uint32_t v = 0; v < MII_SHAPE_VERTICES; v++) mesh[indices + 2u * v] = (uint8_t)v;
  const uint32_t packed = zlib_stored(out + MII_SHAPE_DATA_AT, mesh, mesh_bytes);
  const uint32_t size = MII_SHAPE_DATA_AT + packed;
  mii_header(out, MII_SHAPE_DATA_AT, MII_SHAPE_TABLE_AT, "NFSR");
  put32(out + MII_HEADER_SIZE_AT, size);
  for (uint32_t i = 0; i < MII_SHAPE_ENTRIES; i++) {
    uint8_t *e = out + MII_SHAPE_TABLE_AT + i * MII_SHAPE_ENTRY_BYTES;
    put32(e + 0x00, MII_SHAPE_DATA_AT);
    put32(e + 0x04, mesh_bytes);
    put32(e + 0x08, packed);
    put32(e + 0x0C, MII_ENTRY_TAG);
    for (uint32_t k = 0; k < MII_SHAPE_STREAMS; k++) {
      put32(e + 0x10 + 4u * k, at[k]);
      put32(e + 0x10 + 4u * (MII_SHAPE_STREAMS + k), k_shape_stream_bytes[k]);
    }
  }
  return size;
}

static uint8_t g_mii_model[RD_ARCHIVE_BYTES];
static uint64_t g_mii_model_size;

static void build_mii_model(void) {
  static uint8_t texture[MII_TEXTURE_DATA_AT + MII_TEXTURE_PIXEL_BYTES + ZLIB_OVERHEAD];
  static uint8_t shape[MII_SHAPE_DATA_AT + MII_SHAPE_STREAMS * MII_SHAPE_STREAM_ALIGN * 2u + ZLIB_OVERHEAD];
  const uint32_t texture_bytes = build_mii_texture(texture), shape_bytes = build_mii_shape(shape);
  const System_Data_File files[] = {
      {"NXTextureLowLinear.dat", texture, texture_bytes}, {"NXTextureLowSRGB.dat", texture, texture_bytes},
      {"NXTextureMidLinear.dat", texture, texture_bytes}, {"NXTextureMidSRGB.dat", texture, texture_bytes},
      {"ShapeHigh.dat", shape, shape_bytes},              {"ShapeMid.dat", shape, shape_bytes},
  };
  g_mii_model_size = system_data_build_romfs(files, sizeof(files) / sizeof(files[0]), g_mii_model, sizeof(g_mii_model));
}

bool system_data_open(uint64_t data_id, Byte_Source *out) {
  if (data_id != SYSTEM_DATA_MII_MODEL) return false;
  if (!g_mii_model_size) build_mii_model();
  if (!g_mii_model_size) return false;
  *out = byte_source_from_memory(g_mii_model, g_mii_model_size);
  return true;
}

/* ---- the user's own archives --------------------------------------------- */

#define USER_DATA_SLOTS 4u
#define USER_DATA_PATH_BYTES 64u

typedef struct User_Data {
  uint64_t id;
  Ramfs_Pool *pool;
  uint32_t node;
  Byte_Source file;
  NCA_File nca;
  bool used;
} User_Data;

static User_Data g_user[USER_DATA_SLOTS];

static Error ramfs_source_read(void *user, uint64_t offset, void *out, uint64_t size) {
  const User_Data *d = (const User_Data *)user;
  uint64_t got = 0;
  if (ramfs_read(d->pool, d->node, offset, out, size, &got) != 0 || got != size)
    return ERR(RESULT_IO_ERROR, "system data: short read from the SD card");
  return OK;
}

/* Opens one candidate file as the archive: false if it is absent, empty
 * (a removed copy) or not usable. */
static bool open_user_file(Ramfs_Pool *pool, uint32_t sd_root, const char *path, bool is_nca, uint64_t data_id,
                           Byte_Source *out) {
  uint32_t node = RAMFS_NO_NODE;
  if (ramfs_lookup(pool, sd_root, path, &node) != 0 || pool->nodes[node].size == 0) return false;
  User_Data *d = NULL;
  for (uint32_t i = 0; i < USER_DATA_SLOTS && !d; i++)
    if (!g_user[i].used || g_user[i].id == data_id) d = &g_user[i];
  if (!d) return false;
  memset(d, 0, sizeof(*d));
  d->id = data_id;
  d->pool = pool;
  d->node = node;
  d->file.user = d;
  d->file.size = pool->nodes[node].size;
  d->file.read = ramfs_source_read;
  d->used = true;
  if (!is_nca) {
    *out = d->file;
    log_info("[fs] system data %016llx: the user's RomFS %s (%llu bytes)", (unsigned long long)data_id, path,
             (unsigned long long)d->file.size);
    return true;
  }
  int section = -1;
  if (!error_is_ok(nca_open(&d->file, &d->nca)) || (section = nca_find_section(&d->nca, NCA_FS_ROMFS)) < 0 ||
      d->nca.header.sections[section].has_compression_info ||
      !error_is_ok(nca_probe_section(&d->nca, (uint32_t)section))) {
    log_warn("[fs] system data %016llx: %s is not a pre-decrypted data NCA with an uncompressed RomFS",
             (unsigned long long)data_id, path);
    d->used = false;
    return false;
  }
  *out = *nca_section_source(&d->nca, (uint32_t)section);
  log_info("[fs] system data %016llx: the user's NCA %s", (unsigned long long)data_id, path);
  return true;
}

bool system_data_open_user(Ramfs_Pool *pool, uint32_t sd_root, uint64_t data_id, Byte_Source *out) {
  if (!pool || sd_root == RAMFS_NO_NODE) return false;
  static const char *const forms[] = {"/systemdata/%016llx.nca", "/systemdata/%016llx.romfs", "/%016llx.nca",
                                      "/%016llx.romfs"};
  char path[USER_DATA_PATH_BYTES];
  for (uint32_t form = 0; form < sizeof(forms) / sizeof(forms[0]); form++) {
    snprintf(path, sizeof(path), forms[form], (unsigned long long)data_id);
    if (open_user_file(pool, sd_root, path, (form & 1u) == 0, data_id, out)) return true;
  }
  return false;
}

System_Data_Kind system_data_identify(const Byte_Source *file, uint64_t *data_id) {
  *data_id = 0;
  NCA_File nca;
  const Error err = nca_open(file, &nca);
  if (err.code == RESULT_ENCRYPTED_INPUT) return SYSTEM_DATA_ENCRYPTED;
  if (error_is_ok(err)) {
    if (nca.header.content_type != NCA_CONTENT_DATA && nca.header.content_type != NCA_CONTENT_PUBLIC_DATA)
      return SYSTEM_DATA_UNKNOWN;
    if (nca_find_section(&nca, NCA_FS_ROMFS) < 0) return SYSTEM_DATA_UNKNOWN;
    *data_id = nca.header.program_id;
    return SYSTEM_DATA_NCA;
  }
  uint8_t header[8];
  if (file->size >= ROMFS_HEADER_SIZE && error_is_ok(byte_source_read(file, 0, header, sizeof(header)))) {
    uint64_t header_size;
    memcpy(&header_size, header, sizeof(header_size));
    if (header_size == ROMFS_HEADER_SIZE) return SYSTEM_DATA_ROMFS;
  }
  return SYSTEM_DATA_UNKNOWN;
}
