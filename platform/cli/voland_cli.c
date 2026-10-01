/**
 * voland-cli - the headless native runner (§17): the test harness for
 * golden-image and differential runs, and the integration point for
 * launcher frontends. No GUI toolkit.
 *
 *   voland-cli run <file.nca|file.nro> [options]
 *       --backend interpreter|noop   CPU backend (default: interpreter)
 *       --budget N                   cycles per scheduler slice (default 100000)
 *       --max-slices N               stop after N slices (default 10000000)
 *       --test-card                  publish the core's test card before running
 *       --expect-output TEXT         exit 4 unless the guest printed TEXT
 *       --expect-frame-hash HEX      exit 5 unless the newest frame hashes to HEX
 *       --sdmc DIR                   seed the emulated SD card with DIR's contents
 *       --dump-frame FILE            write the newest frame as a binary PPM (P6)
 *       --font FILE                  the TTF/OTF pl:u serves as the system font
 *       --svc-stats                  print per-SVC call counts at the end
 *     Prints guest output (svcOutputDebugString) as it happens, then a
 *     summary with the newest frame's FNV-1a-64 hash (golden-image check).
 *     Exit: 0 exited, 1 crashed, 2 deadlock, 3 slice limit, 4/5 failed
 *     expectations, 64 usage, 66 load failure.
 *
 *   voland-cli verify-dump <file>
 *     Structural check of a dump without booting: NRO or decrypted NCA
 *     (encrypted input is reported with docs/DUMP.md, §1.6), its type,
 *     program id and sections, plus an FNV-1a-64 fingerprint of the file
 *     for bug reports. A fingerprint, not an authenticity check (§12).
 *
 * Deviations, stated: §17's offscreen render-to-texture via Dawn arrives
 * with GPU work to render (Phase 4); until then every frame is CPU-side
 * pixels in the framebuffer slots (§6), which is what --expect-frame-hash
 * hashes. `ptc-precompile`, `cache` and `save-export/import` arrive with
 * the features they operate on (Phases 5-6).
 */
#include "common/layout.h"
#include "emulator.h"
#include "gpu/framebuffer.h"
#include "hle/loader/nca_parse.h"
#include "hle/loader/nro.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define EXIT_EXITED 0
#define EXIT_CRASHED 1
#define EXIT_DEADLOCK 2
#define EXIT_SLICE_LIMIT 3
#define EXIT_OUTPUT_MISMATCH 4
#define EXIT_FRAME_MISMATCH 5
#define EXIT_USAGE 64
#define EXIT_LOAD_FAILED 66

#define DEFAULT_BUDGET 100000ull
#define DEFAULT_MAX_SLICES 10000000ull
#define TEST_CARD_WIDTH 1280u
#define TEST_CARD_HEIGHT 720u
#define OUTPUT_CAPTURE_BYTES 65536u
#define FNV_OFFSET 0xCBF29CE484222325ull
#define FNV_PRIME 0x100000001B3ull
#define FINGERPRINT_CHUNK 65536u
#define SDMC_PATH_BYTES 0x301u
#define SDMC_MAX_FILE_BYTES ((uint64_t)64 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* File-backed Byte_Source (byte_source.h contract).                   */
/* ------------------------------------------------------------------ */

static Error file_read(void *user, uint64_t offset, void *out, uint64_t size) {
  FILE *file = (FILE *)user;
  if (fseek(file, (long)offset, SEEK_SET) != 0) return ERR(RESULT_IO_ERROR, "seek failed");
  if (fread(out, 1, (size_t)size, file) != (size_t)size) return ERR(RESULT_IO_ERROR, "short read");
  return OK;
}

static bool open_source(const char *path, FILE **file, Byte_Source *source) {
  *file = fopen(path, "rb");
  if (!*file) {
    fprintf(stderr, "voland-cli: cannot open %s\n", path);
    return false;
  }
  fseek(*file, 0, SEEK_END);
  const long size = ftell(*file);
  *source = (Byte_Source){*file, (uint64_t)(size < 0 ? 0 : size), file_read};
  return true;
}

static uint64_t fnv1a(uint64_t hash, const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * FNV_PRIME;
  return hash;
}

/* ------------------------------------------------------------------ */
/* run                                                                 */
/* ------------------------------------------------------------------ */

static char g_output[OUTPUT_CAPTURE_BYTES];
static size_t g_output_length;

static void on_guest_output(void *userdata, const char *text, size_t length) {
  (void)userdata;
  printf("%.*s\n", (int)length, text);
  fflush(stdout);
  if (g_output_length + length + 1u < sizeof(g_output)) {
    memcpy(g_output + g_output_length, text, length);
    g_output_length += length;
    g_output[g_output_length++] = '\n';
    g_output[g_output_length] = '\0';
  }
}

/* The newest frame as a P6 PPM (RGB; alpha dropped). */
static bool dump_frame(const char *path) {
  const uint32_t published = framebuffer_published();
  if (published == 0) return false;
  const uint8_t *region = (const uint8_t *)(uintptr_t)layout_get()->framebuffer_slot_base;
  const uint32_t slot = (published - 1u) % FRAMEBUFFER_SLOT_COUNT;
  const uint8_t *meta = region + FRAMEBUFFER_OFFSET_METADATA + slot * FRAMEBUFFER_METADATA_BYTES;
  uint32_t width = 0, height = 0, stride = 0;
  memcpy(&width, meta, 4);
  memcpy(&height, meta + 4, 4);
  memcpy(&stride, meta + 8, 4);
  const uint8_t *pixels = region + LAYOUT_FRAMEBUFFER_HEADER_BYTES + (uint64_t)slot * LAYOUT_FRAMEBUFFER_SLOT_BYTES;
  FILE *out = fopen(path, "wb");
  if (!out) return false;
  fprintf(out, "P6\n%u %u\n255\n", width, height);
  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) fwrite(pixels + (uint64_t)y * stride + x * 4u, 1, 3, out);
  }
  fclose(out);
  return true;
}

/* FNV-1a-64 over the newest published frame's visible pixels, or 0. */
static uint64_t newest_frame_hash(uint32_t *width, uint32_t *height) {
  const uint32_t published = framebuffer_published();
  *width = *height = 0;
  if (published == 0) return 0;
  const uint8_t *region = (const uint8_t *)(uintptr_t)layout_get()->framebuffer_slot_base;
  const uint32_t slot = (published - 1u) % FRAMEBUFFER_SLOT_COUNT;
  const uint8_t *meta = region + FRAMEBUFFER_OFFSET_METADATA + slot * FRAMEBUFFER_METADATA_BYTES;
  uint32_t stride = 0;
  memcpy(width, meta, 4);
  memcpy(height, meta + 4, 4);
  memcpy(&stride, meta + 8, 4);
  const uint8_t *pixels = region + LAYOUT_FRAMEBUFFER_HEADER_BYTES + (uint64_t)slot * LAYOUT_FRAMEBUFFER_SLOT_BYTES;
  uint64_t hash = FNV_OFFSET;
  for (uint32_t y = 0; y < *height; y++) hash = fnv1a(hash, pixels + (uint64_t)y * stride, (size_t)*width * 4u);
  return hash;
}

/* Copies host directory `host` into the SD card at `guest` (recursive).
 * Returns the number of files imported, or -1 on failure. */
static int import_sdmc(Emulator *emu, const char *host, const char *guest) {
  DIR *dir = opendir(host);
  if (!dir) return -1;
  int imported = 0;
  const struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && imported >= 0) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char host_path[4096], guest_path[SDMC_PATH_BYTES];
    snprintf(host_path, sizeof(host_path), "%s/%s", host, entry->d_name);
    if (snprintf(guest_path, sizeof(guest_path), "%s/%s", guest, entry->d_name) >= (int)sizeof(guest_path)) continue;
    struct stat st;
    if (stat(host_path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (!error_is_ok(emulator_sd_card_create_directory(emu, guest_path))) imported = -1;
      else {
        const int nested = import_sdmc(emu, host_path, guest_path);
        imported = nested < 0 ? -1 : imported + nested;
      }
      continue;
    }
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size > SDMC_MAX_FILE_BYTES) continue;
    FILE *file = fopen(host_path, "rb");
    if (!file) continue;
    void *data = malloc((size_t)st.st_size + 1u);
    const bool ok = data && fread(data, 1, (size_t)st.st_size, file) == (size_t)st.st_size &&
                    error_is_ok(emulator_sd_card_write_file(emu, guest_path, data, (uint64_t)st.st_size));
    free(data);
    fclose(file);
    if (!ok) imported = -1;
    else imported++;
  }
  closedir(dir);
  return imported;
}

static int run(int argc, char **argv) {
  if (argc < 1) return EXIT_USAGE;
  const char *path = argv[0];
  const CPU_Backend *backend = &CPU_BACKEND_INTERPRETER;
  uint64_t budget = DEFAULT_BUDGET, max_slices = DEFAULT_MAX_SLICES;
  bool test_card = false, svc_stats = false;
  const char *expect_output = NULL, *expect_hash = NULL, *sdmc = NULL, *frame_path = NULL, *font_path = NULL;
  for (int i = 1; i < argc; i++) {
    const bool has_value = i + 1 < argc;
    if (!strcmp(argv[i], "--backend") && has_value) {
      const char *name = argv[++i];
      if (!strcmp(name, "noop")) backend = &CPU_BACKEND_NOOP;
      else if (!strcmp(name, "interpreter")) backend = &CPU_BACKEND_INTERPRETER;
      else { fprintf(stderr, "voland-cli: unknown backend %s\n", name); return EXIT_USAGE; }
    } else if (!strcmp(argv[i], "--budget") && has_value) {
      budget = strtoull(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "--max-slices") && has_value) {
      max_slices = strtoull(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "--test-card")) {
      test_card = true;
    } else if (!strcmp(argv[i], "--expect-output") && has_value) {
      expect_output = argv[++i];
    } else if (!strcmp(argv[i], "--expect-frame-hash") && has_value) {
      expect_hash = argv[++i];
    } else if (!strcmp(argv[i], "--svc-stats")) {
      svc_stats = true;
    } else if (!strcmp(argv[i], "--font") && has_value) {
      font_path = argv[++i];
    } else if (!strcmp(argv[i], "--dump-frame") && has_value) {
      frame_path = argv[++i];
    } else if (!strcmp(argv[i], "--sdmc") && has_value) {
      sdmc = argv[++i];
    } else {
      fprintf(stderr, "voland-cli: unknown option %s\n", argv[i]);
      return EXIT_USAGE;
    }
  }

  FILE *file = NULL;
  Byte_Source source;
  if (!open_source(path, &file, &source)) return EXIT_LOAD_FAILED;
  static Emulator emu;
  Error err = emulator_create_with_backend(&emu, backend);
  if (!error_is_ok(err)) {
    fprintf(stderr, "voland-cli: emulator_create: %s\n", err.message);
    fclose(file);
    return EXIT_LOAD_FAILED;
  }
  emulator_set_debug_output(&emu, on_guest_output, NULL);
  framebuffer_reset();
  if (test_card) (void)framebuffer_publish_test_card(TEST_CARD_WIDTH, TEST_CARD_HEIGHT);
  if (sdmc) {
    const int imported = import_sdmc(&emu, sdmc, "");
    if (imported < 0) {
      fprintf(stderr, "voland-cli: could not import %s into the SD card\n", sdmc);
      emulator_destroy(&emu);
      fclose(file);
      return EXIT_LOAD_FAILED;
    }
    fprintf(stderr, "voland-cli: SD card seeded with %d files from %s\n", imported, sdmc);
  }
  static uint8_t *font;
  if (font_path) {
    FILE *f = fopen(font_path, "rb");
    long size = 0;
    if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (font = malloc((size_t)size)) != NULL && fread(font, 1, (size_t)size, f) == (size_t)size) {
      emulator_set_shared_font(&emu, font, (uint32_t)size);
    } else {
      fprintf(stderr, "voland-cli: could not read font %s\n", font_path);
    }
    if (f) fclose(f);
  }
  /* Homebrew appears on the SD card under its own name (its argv[0]). */
  const char *base = strrchr(path, '/');
  emulator_set_program_path(&emu, base ? base + 1 : path);
  /* The file stays open while the program runs: fsp-srv reads its RomFS. */
  err = emulator_load(&emu, &source, 0);
  if (!error_is_ok(err)) {
    fprintf(stderr, "voland-cli: load failed: %s\n", err.message ? err.message : "(no message)");
    emulator_destroy(&emu);
    fclose(file);
    return EXIT_LOAD_FAILED;
  }

  Emulator_Status status = EMULATOR_RUNNING;
  uint64_t slices = 0;
  while (slices < max_slices && (status == EMULATOR_RUNNING || status == EMULATOR_IDLE)) {
    status = emulator_run_slice(&emu, budget);
    framebuffer_consume_all(); /* the CLI "displays" every frame at once */
    slices++;
  }

  uint32_t width = 0, height = 0;
  const uint64_t frame_hash = newest_frame_hash(&width, &height);
  static const char *const k_status[] = {"running", "idle", "exited", "crashed", "deadlock", "not loaded"};
  fprintf(stderr, "voland-cli: %s after %llu slices, virtual time %llu ticks, %llu SVCs\n",
          k_status[status], (unsigned long long)slices, (unsigned long long)emu.scheduler.ticks,
          (unsigned long long)emu.hle.svc_call_count);
  if (width) fprintf(stderr, "voland-cli: frame %ux%u fnv1a64=%016llx\n", width, height, (unsigned long long)frame_hash);
  if (svc_stats) {
    for (uint32_t i = 0; i < HLE_SVC_COUNT; i++) {
      if (emu.hle.svc_counts[i]) fprintf(stderr, "voland-cli: svc 0x%02x x %llu\n", i, (unsigned long long)emu.hle.svc_counts[i]);
    }
  }
  if (frame_path && !dump_frame(frame_path)) fprintf(stderr, "voland-cli: no frame to dump to %s\n", frame_path);
  if (status == EMULATOR_CRASHED) {
    fprintf(stderr, "voland-cli: crashed at pc=0x%010llx\n", (unsigned long long)emu.scheduler.crash_pc);
  }
  emulator_destroy(&emu);
  fclose(file);

  if (expect_output && !strstr(g_output, expect_output)) {
    fprintf(stderr, "voland-cli: expected output not seen: %s\n", expect_output);
    return EXIT_OUTPUT_MISMATCH;
  }
  if (expect_hash && (!width || strtoull(expect_hash, NULL, 16) != frame_hash)) {
    fprintf(stderr, "voland-cli: frame hash %016llx != expected %s\n", (unsigned long long)frame_hash, expect_hash);
    return EXIT_FRAME_MISMATCH;
  }
  switch (status) {
  case EMULATOR_EXITED: return EXIT_EXITED;
  case EMULATOR_CRASHED: return EXIT_CRASHED;
  case EMULATOR_DEADLOCK: return EXIT_DEADLOCK;
  default: return EXIT_SLICE_LIMIT;
  }
}

/* ------------------------------------------------------------------ */
/* verify-dump                                                         */
/* ------------------------------------------------------------------ */

static uint64_t fingerprint(const Byte_Source *source) {
  static uint8_t chunk[FINGERPRINT_CHUNK];
  uint64_t hash = FNV_OFFSET;
  for (uint64_t offset = 0; offset < source->size; offset += FINGERPRINT_CHUNK) {
    const uint64_t n = source->size - offset < FINGERPRINT_CHUNK ? source->size - offset : FINGERPRINT_CHUNK;
    if (!error_is_ok(byte_source_read(source, offset, chunk, n))) return 0;
    hash = fnv1a(hash, chunk, (size_t)n);
  }
  return hash;
}

static int verify_dump(const char *path) {
  FILE *file = NULL;
  Byte_Source source;
  if (!open_source(path, &file, &source)) return EXIT_LOAD_FAILED;
  if (!error_is_ok(layout_create())) {
    fclose(file);
    return EXIT_LOAD_FAILED;
  }
  int code = EXIT_EXITED;
  printf("file:        %s (%llu bytes)\n", path, (unsigned long long)source.size);
  NSO nro;
  NCA_File nca;
  if (error_is_ok(nro_open(&source, &nro))) {
    printf("type:        homebrew NRO\n");
    printf("image:       %llu bytes (.text %u, .rodata %u, .data %u, .bss %u)\n",
           (unsigned long long)nro.image_size, nro.segments[0].memory_size, nro.segments[1].memory_size,
           nro.segments[2].memory_size, nro.bss_size);
  } else {
    const Error err = nca_open(&source, &nca);
    if (!error_is_ok(err)) {
      printf("type:        unusable (%s)\n", err.message ? err.message : "unknown error");
      code = EXIT_LOAD_FAILED;
    } else {
      static const char *const k_types[] = {"Program", "Meta", "Control", "Manual", "Data", "PublicData"};
      const unsigned type = (unsigned)nca.header.content_type;
      printf("type:        decrypted NCA, %s content\n", type < 6 ? k_types[type] : "unknown");
      printf("program id:  %016llX\n", (unsigned long long)nca.header.program_id);
      for (uint32_t i = 0; i < NCA_SECTION_COUNT; i++) {
        if (!nca.header.sections[i].present) continue;
        const Error probe = nca_probe_section(&nca, i);
        printf("section %u:   %s, %llu bytes, %s\n", i, nca.header.sections[i].fs_type == NCA_FS_ROMFS ? "RomFS" : "PFS0",
               (unsigned long long)nca.header.sections[i].size,
               error_is_ok(probe) ? "plaintext" : (probe.message ? probe.message : "unreadable"));
        if (!error_is_ok(probe)) code = EXIT_LOAD_FAILED;
      }
    }
  }
  printf("fingerprint: fnv1a64=%016llx\n", (unsigned long long)fingerprint(&source));
  fclose(file);
  layout_destroy();
  return code;
}

static void usage(void) {
  fprintf(stderr,
          "usage: voland-cli run <file.nca|file.nro> [--backend interpreter|noop] [--budget N]\n"
          "                      [--max-slices N] [--test-card] [--expect-output TEXT]\n"
          "                      [--expect-frame-hash HEX]\n"
          "       voland-cli verify-dump <file>\n");
}

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "run")) return run(argc - 2, argv + 2);
  if (argc == 3 && !strcmp(argv[1], "verify-dump")) return verify_dump(argv[2]);
  usage();
  return EXIT_USAGE;
}
