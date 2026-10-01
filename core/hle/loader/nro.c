#include "hle/loader/nro.h"

#include <string.h>

#define NRO_MAGIC 0x304F524Eu /* "NRO0" */
#define NRO_PAGE 0x1000u
#define NRO_OFFSET_MAGIC 0x00u     /* relative to NRO_HEADER_OFFSET */
#define NRO_OFFSET_SIZE 0x08u
#define NRO_OFFSET_SEGMENTS 0x10u  /* 3 x (u32 file offset, u32 size) */
#define NRO_OFFSET_BSS 0x28u
#define NRO_OFFSET_BUILD_ID 0x30u

Error nro_open(const Byte_Source *source, NSO *out) {
  if (!source || !out) return ERR(RESULT_INVALID_ARGUMENT, "nro_open: NULL argument");
  if (source->size < NRO_HEADER_END) return ERR(RESULT_INVALID_ARGUMENT, "nro_open: file shorter than the header");
  uint8_t header[NRO_HEADER_END - NRO_HEADER_OFFSET];
  Error err = byte_source_read(source, NRO_HEADER_OFFSET, header, sizeof(header));
  if (!error_is_ok(err)) return err;
  if (byte_source_le32(header + NRO_OFFSET_MAGIC) != NRO_MAGIC) {
    return ERR(RESULT_INVALID_ARGUMENT, "nro_open: missing NRO0 magic (not a homebrew NRO)");
  }
  const uint32_t total = byte_source_le32(header + NRO_OFFSET_SIZE);
  if (total > source->size) return ERR(RESULT_INVALID_ARGUMENT, "nro_open: declared size exceeds the file");

  memset(out, 0, sizeof(*out));
  out->source = source;
  uint32_t expected_offset = 0;
  for (uint32_t i = 0; i < NSO_SEGMENT_COUNT; i++) {
    const uint32_t offset = byte_source_le32(header + NRO_OFFSET_SEGMENTS + 8u * i);
    const uint32_t size = byte_source_le32(header + NRO_OFFSET_SEGMENTS + 8u * i + 4u);
    if (offset != expected_offset || (offset % NRO_PAGE) || (size % NRO_PAGE)) {
      return ERR(RESULT_INVALID_ARGUMENT, "nro_open: segments must be page-aligned and contiguous from 0");
    }
    if ((uint64_t)offset + size > total) return ERR(RESULT_INVALID_ARGUMENT, "nro_open: segment past the image");
    out->segments[i] = (NSO_Segment){offset, size, offset, size, false, false};
    expected_offset = offset + size;
  }
  if (out->segments[NSO_SEGMENT_TEXT].memory_size == 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "nro_open: empty .text");
  }
  out->bss_size = byte_source_le32(header + NRO_OFFSET_BSS);
  const uint64_t image = (uint64_t)expected_offset + out->bss_size;
  out->image_size = (image + NRO_PAGE - 1u) & ~(uint64_t)(NRO_PAGE - 1u);
  if (out->image_size > NRO_MAX_IMAGE_BYTES) return ERR(RESULT_INVALID_ARGUMENT, "nro_open: image too large");
  memcpy(out->module_id, header + NRO_OFFSET_BUILD_ID, NSO_MODULE_ID_SIZE);
  return OK;
}
