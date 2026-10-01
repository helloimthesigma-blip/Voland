/**
 * NRO, the homebrew executable format (§12: "required for the Phase 2
 * goal"). Layout per switchbrew: NroStart (a branch + the MOD0 offset)
 * then a 0x70-byte header at 0x10 - magic "NRO0", total size, and the
 * .text/.rodata/.data segments as (file offset, size) pairs whose file
 * offsets equal their memory offsets, plus .bss. Nothing is compressed.
 *
 * nro_open() describes an NRO as the uncompressed single-module image the
 * process bootstrap already knows how to map (an NSO with raw segments),
 * so homebrew and titles share one code path from there on. Homebrew is
 * user-built code, not Nintendo content: nothing here relates to §1.6.
 */
#ifndef SWITCH_HLE_LOADER_NRO_H
#define SWITCH_HLE_LOADER_NRO_H

#include "common/result.h"
#include "hle/loader/byte_source.h"
#include "hle/loader/nso.h"

#define NRO_HEADER_OFFSET 0x10u
#define NRO_HEADER_END 0x80u
#define NRO_MAX_IMAGE_BYTES ((uint64_t)512 * 1024 * 1024)

/* Parses the header at offset 0 of `source` into `out` as raw segments.
 *   RESULT_INVALID_ARGUMENT NULL; source too short; magic not "NRO0";
 *                           segments not page-aligned, not contiguous
 *                           (.text at 0, .rodata after it, .data after
 *                           that), past the declared size or the source;
 *                           an image over NRO_MAX_IMAGE_BYTES. */
Error nro_open(const Byte_Source *source, NSO *out);

#endif /* SWITCH_HLE_LOADER_NRO_H */
