#include "hle/fs/system_data.h"

#include <string.h>

#include "hle/loader/romfs.h"

#define RD_DATA_OFFSET 0x200u      /* file data after the header, as packed images place it */
#define RD_ALIGN_DATA 0x10u        /* each file's data alignment */
#define RD_DIR_ENTRY_BYTES 0x18u   /* fixed part of a directory entry */
#define RD_FILE_ENTRY_BYTES 0x20u  /* fixed part of a file entry */
#define RD_MAX_FILES 16u
#define RD_ARCHIVE_BYTES 0x1000u

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

#define MII_RESOURCE_HEADER_BYTES 16u
#define MII_RESOURCE_VERSION 1u

/* A resource header: four-character signature, then a u32 version. */
static void mii_header(uint8_t out[MII_RESOURCE_HEADER_BYTES], const char signature[4]) {
  memset(out, 0, MII_RESOURCE_HEADER_BYTES);
  memcpy(out, signature, 4u);
  put32(out + 4u, MII_RESOURCE_VERSION);
}

static uint8_t g_mii_model[RD_ARCHIVE_BYTES];
static uint64_t g_mii_model_size;

static void build_mii_model(void) {
  static uint8_t texture[MII_RESOURCE_HEADER_BYTES], shape[MII_RESOURCE_HEADER_BYTES];
  mii_header(texture, "NFTR");
  mii_header(shape, "NFSR");
  const System_Data_File files[] = {
      {"NXTextureLowLinear.dat", texture, sizeof(texture)}, {"NXTextureLowSRGB.dat", texture, sizeof(texture)},
      {"NXTextureMidLinear.dat", texture, sizeof(texture)}, {"NXTextureMidSRGB.dat", texture, sizeof(texture)},
      {"NXShapeHigh.dat", shape, sizeof(shape)},            {"NXShapeMid.dat", shape, sizeof(shape)},
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
