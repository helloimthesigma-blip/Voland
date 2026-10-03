/**
 * voland-cli - the headless native runner (§17): the test harness for
 * golden-image and differential runs, and the integration point for
 * launcher frontends. No GUI toolkit.
 *
 *   voland-cli run <file.nca|file.nro> [options]
 *       --backend interpreter|jit|noop CPU backend (default: interpreter)
 *       --budget N                   cycles per scheduler slice (default 100000)
 *       --jit-threshold N            jit: executions before a block is compiled
 *       --jit-dump DIR               jit: write every compiled module to DIR
 *       --max-slices N               stop after N slices (default 10000000)
 *       --test-card                  publish the core's test card before running
 *       --expect-output TEXT         exit 4 unless the guest printed TEXT
 *       --expect-frame-hash HEX      exit 5 unless the newest frame hashes to HEX
 *       --sdmc DIR                   seed the emulated SD card with DIR's contents
 *       --dump-frame FILE            write the newest frame as a binary PPM (P6)
 *       --frame-skip N               rasterise and show one of every N + 1 frames
 *       --dump-frames-every N        with --dump-frame: also write FILE.<slice>.ppm
 *                                    every N slices (watching a long run progress)
 *       --snapshot-at N --snapshot-dir DIR
 *                                    (POSIX) at slice N, stop and serve jobs: each
 *                                    DIR/job file forks a copy-on-write child that
 *                                    continues from slice N with the job's lines
 *                                    (max_slices N, input S:HEX:L, frame PATH,
 *                                    every N, trace S:L, log PATH, snapshot N DIR -
 *                                    a nested snapshot server); the result is
 *                                    DIR/job.done.<pid>. DIR/quit ends the server.
 *                                    Iterating on a late scene without replaying.
 *       --font FILE                  the TTF/OTF pl:u serves as the system font
 *       --svc-stats                  print per-SVC call counts at the end
 *       --dump-audio FILE            write what the guest played as a 48kHz stereo WAV
 *       --input SLICE:BUTTONS:SLICES player 1 holds BUTTONS (hex, HidNpadButton
 *                                    bits) from SLICE for SLICES slices (repeatable)
 *       --swkbd TEXT                 answer software-keyboard prompts with TEXT
 *                                    (default: accept the prompt's initial text;
 *                                    --swkbd-cancel cancels them instead)
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
#include "audio/audio_ring.h"
#include "common/input_region.h"
#include "common/layout.h"
#include "emulator.h"
#include "hle/kernel/handle_table.h"
#include "cpu/backends/interpreter/interpreter.h"
#include "cpu/backends/jit/jit.h"
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
#define MAX_INPUT_EVENTS 32u
#define WAV_HEADER_BYTES 44u
#define WAV_DRAIN_FRAMES 4096u

/* Drains the audio ring into an open WAV file (16-bit stereo). */
static uint64_t drain_audio(FILE *wav) {
  static float frames[WAV_DRAIN_FRAMES * AUDIO_RING_CHANNELS];
  static int16_t pcm[WAV_DRAIN_FRAMES * AUDIO_RING_CHANNELS];
  uint64_t total = 0;
  uint32_t n;
  while ((n = audio_ring_drain(frames, WAV_DRAIN_FRAMES)) > 0) {
    for (uint32_t i = 0; i < n * AUDIO_RING_CHANNELS; i++) {
      const float v = frames[i] > 1.0f ? 1.0f : frames[i] < -1.0f ? -1.0f : frames[i];
      pcm[i] = (int16_t)(v * 32767.0f);
    }
    if (wav) fwrite(pcm, sizeof(int16_t) * AUDIO_RING_CHANNELS, n, wav);
    total += n;
  }
  return total;
}

static void write_wav_header(FILE *wav, uint64_t frames) {
  const uint32_t data_bytes = (uint32_t)(frames * AUDIO_RING_CHANNELS * sizeof(int16_t));
  const uint32_t rate = AUDIO_RING_SAMPLE_RATE, byte_rate = rate * AUDIO_RING_CHANNELS * 2u;
  uint8_t h[WAV_HEADER_BYTES];
  memcpy(h, "RIFF", 4);
  const uint32_t riff = 36u + data_bytes;
  memcpy(h + 4, &riff, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  const uint32_t fmt_size = 16;
  const uint16_t pcm_format = 1, channels = (uint16_t)AUDIO_RING_CHANNELS, align = (uint16_t)(AUDIO_RING_CHANNELS * 2u),
                 bits = 16;
  memcpy(h + 16, &fmt_size, 4);
  memcpy(h + 20, &pcm_format, 2);
  memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &rate, 4);
  memcpy(h + 28, &byte_rate, 4);
  memcpy(h + 32, &align, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &data_bytes, 4);
  fseek(wav, 0, SEEK_SET);
  fwrite(h, 1, sizeof(h), wav);
}

typedef struct Input_Event {
  uint64_t start;
  uint32_t buttons;
  uint64_t length;
} Input_Event;

/* Player 1 as a connected standard gamepad holding whatever the active
 * events say at `slice`. */
static void apply_input(const Input_Event *events, uint32_t count, uint64_t slice) {
  Input_Controller_State state;
  memset(&state, 0, sizeof(state));
  state.flags = INPUT_FLAG_CONNECTED | ((uint32_t)INPUT_DEVICE_STANDARD_GAMEPAD << INPUT_FLAG_DEVICE_KIND_SHIFT);
  for (uint32_t i = 0; i < count; i++) {
    if (slice >= events[i].start && slice < events[i].start + events[i].length) state.buttons |= events[i].buttons;
  }
  void *region = (void *)(uintptr_t)layout_get()->input_region_base;
  input_region_write_begin(region, 0);
  input_region_write_payload(region, 0, &state);
  input_region_write_end(region, 0);
}
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

/* VOLAND_DUMP_SHADERS=DIR: every decoded program as DIR/<address>.txt -
 * index, mnemonic, form and the raw word (diagnostics). */
static void dump_program(void *user, const Sm_Program *program) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%llx-%08x.txt", (const char *)user, (unsigned long long)program->address, program->hash);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "stage %u words %u\n", program->header.stage, program->word_count);
  for (uint32_t i = 0; i < program->word_count; i++) {
    const Sm_Insn *in = &program->insns[i];
    if (in->op == SM_OP_SCHED) continue;
    fprintf(f, "%4u %-8s form %u pred %x target %d raw %016llx\n", i, sm_op_name((Sm_Op)in->op), in->form, in->pred,
            in->target, (unsigned long long)in->raw);
  }
  fclose(f);
}

/* VOLAND_DUMP_TEXTURES=DIR: every decoded RGBA8 texture as DIR/<address>-WxH.ppm
 * and its alpha as DIR/<address>-WxH-a.pgm (diagnostics). */
static void dump_texture(void *user, const Tex_Image *image, uint64_t address) {
  const char *dir = (const char *)user;
  if (!image->rgba8 && image->format != 0x08u) return;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%llx-%ux%u.ppm", dir, (unsigned long long)address, image->width, image->height);
  FILE *rgb = fopen(path, "wb");
  snprintf(path, sizeof(path), "%s/%llx-%ux%u-a.pgm", dir, (unsigned long long)address, image->width, image->height);
  FILE *alpha = fopen(path, "wb");
  if (rgb && alpha) {
    fprintf(rgb, "P6\n%u %u\n255\n", image->width, image->height);
    fprintf(alpha, "P5\n%u %u\n255\n", image->width, image->height);
    for (uint32_t y = 0; y < image->height; y++) {
      for (uint32_t x = 0; x < image->width; x++) {
        const uint8_t *p = image->texels + (uint64_t)y * image->row_bytes + (uint64_t)x * 4u;
        fwrite(p, 1, 3, rgb);
        fwrite(p + 3, 1, 1, alpha);
      }
    }
  }
  if (rgb) fclose(rgb);
  if (alpha) fclose(alpha);
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

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>

/* A snapshot job's settings (see --snapshot-at). */
typedef struct Snapshot_Job {
  uint64_t max_slices, dump_every, trace_start, trace_length, snapshot_at;
  char frame_path[512];
  char snapshot_dir[512];
  Input_Event inputs[MAX_INPUT_EVENTS];
  uint32_t input_count;
} Snapshot_Job;

static bool read_job(const char *path, Snapshot_Job *job) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char line[600], log_path[512] = "";
  while (fgets(line, sizeof(line), f)) {
    unsigned long long a = 0, b = 0;
    unsigned buttons = 0;
    if (sscanf(line, "max_slices %llu", &a) == 1) job->max_slices = a;
    else if (sscanf(line, "every %llu", &a) == 1) job->dump_every = a;
    else if (sscanf(line, "trace %llu:%llu", &a, &b) == 2) {
      job->trace_start = a;
      job->trace_length = b;
    } else if (sscanf(line, "input %llu:%x:%llu", &a, &buttons, &b) == 3 && job->input_count < MAX_INPUT_EVENTS) {
      job->inputs[job->input_count++] = (Input_Event){a, buttons, b};
    } else if (sscanf(line, "snapshot %llu %511s", &a, job->snapshot_dir) == 2) {
      job->snapshot_at = a;
    } else if (sscanf(line, "frame %511s", job->frame_path) == 1) {
    } else if (sscanf(line, "log %511s", log_path) == 1) {
    }
  }
  fclose(f);
  if (log_path[0] && !freopen(log_path, "w", stderr)) return false;
  return true;
}

/* Parent: serves jobs until DIR/quit, then exits. Child: returns the job. */
static void snapshot_serve(const char *dir, Snapshot_Job *job) {
  char job_path[1024], running[1024], done[1024], quit[1024];
  snprintf(job_path, sizeof(job_path), "%s/job", dir);
  snprintf(running, sizeof(running), "%s/job.running", dir);
  snprintf(quit, sizeof(quit), "%s/quit", dir);
  fprintf(stderr, "voland-cli: snapshot ready; waiting for %s\n", job_path);
  fflush(stderr);
  for (;;) {
    if (access(quit, F_OK) == 0) exit(0);
    if (rename(job_path, running) != 0) {
      sleep(1);
      continue;
    }
    const pid_t pid = fork();
    if (pid == 0) {
      if (!read_job(running, job)) _exit(EXIT_USAGE);
      return;
    }
    int status = 0;
    if (pid > 0) waitpid(pid, &status, 0);
    snprintf(done, sizeof(done), "%s/job.done.%d", dir, (int)pid);
    rename(running, done);
  }
}
#endif

static void setup_call_trace(Emulator *emu);

static int run(int argc, char **argv) {
  if (argc < 1) return EXIT_USAGE;
  const char *path = argv[0];
  const CPU_Backend *backend = &CPU_BACKEND_INTERPRETER;
  uint64_t budget = DEFAULT_BUDGET, max_slices = DEFAULT_MAX_SLICES, dump_every = 0, snapshot_at = 0;
  const char *snapshot_dir = NULL;
  uint32_t frame_skip = 0;
  bool test_card = false, svc_stats = false, swkbd_cancel = false;
  const char *swkbd_text = NULL;
  Input_Event inputs[MAX_INPUT_EVENTS];
  uint32_t input_count = 0;
  const char *expect_output = NULL, *expect_hash = NULL, *sdmc = NULL, *frame_path = NULL, *font_path = NULL, *audio_path = NULL;
  for (int i = 1; i < argc; i++) {
    const bool has_value = i + 1 < argc;
    if (!strcmp(argv[i], "--backend") && has_value) {
      const char *name = argv[++i];
      if (!strcmp(name, "noop")) backend = &CPU_BACKEND_NOOP;
      else if (!strcmp(name, "interpreter")) backend = &CPU_BACKEND_INTERPRETER;
      else if (!strcmp(name, "jit")) backend = &CPU_BACKEND_JIT;
      else { fprintf(stderr, "voland-cli: unknown backend %s\n", name); return EXIT_USAGE; }
    } else if (!strcmp(argv[i], "--jit-dump") && has_value) {
      jit_set_dump_directory(argv[++i]);
    } else if (!strcmp(argv[i], "--jit-threshold") && has_value) {
      jit_set_hot_threshold((uint32_t)strtoul(argv[++i], NULL, 0));
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
    } else if (!strcmp(argv[i], "--input") && has_value) {
      unsigned long long start = 0, length = 0;
      unsigned buttons = 0;
      if (input_count == MAX_INPUT_EVENTS || sscanf(argv[++i], "%llu:%x:%llu", &start, &buttons, &length) != 3) {
        fprintf(stderr, "voland-cli: bad --input %s (want SLICE:HEXBUTTONS:SLICES)\n", argv[i]);
        return EXIT_USAGE;
      }
      inputs[input_count++] = (Input_Event){start, buttons, length};
    } else if (!strcmp(argv[i], "--swkbd") && has_value) {
      swkbd_text = argv[++i];
    } else if (!strcmp(argv[i], "--swkbd-cancel")) {
      swkbd_cancel = true;
    } else if (!strcmp(argv[i], "--svc-stats")) {
      svc_stats = true;
    } else if (!strcmp(argv[i], "--dump-audio") && has_value) {
      audio_path = argv[++i];
    } else if (!strcmp(argv[i], "--font") && has_value) {
      font_path = argv[++i];
    } else if (!strcmp(argv[i], "--frame-skip") && has_value) {
      frame_skip = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "--snapshot-at") && has_value) {
      snapshot_at = strtoull(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "--snapshot-dir") && has_value) {
      snapshot_dir = argv[++i];
    } else if (!strcmp(argv[i], "--dump-frames-every") && has_value) {
      dump_every = strtoull(argv[++i], NULL, 0);
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
  emulator_set_frame_skip(&emu, frame_skip);
  err = emulator_load(&emu, &source, 0);
  if (!error_is_ok(err)) {
    fprintf(stderr, "voland-cli: load failed: %s\n", err.message ? err.message : "(no message)");
    emulator_destroy(&emu);
    fclose(file);
    return EXIT_LOAD_FAILED;
  }

  setup_call_trace(&emu);
  Emulator_Status status = EMULATOR_RUNNING;
  uint64_t slices = 0, audio_frames = 0;
  FILE *wav = NULL;
  if (audio_path) {
    wav = fopen(audio_path, "wb");
    if (!wav) fprintf(stderr, "voland-cli: cannot write %s\n", audio_path);
    else write_wav_header(wav, 0);
  }
  if (getenv("VOLAND_DUMP_SHADERS")) {
    emu.renderer.on_program_decoded = dump_program;
    emu.renderer.on_program_user = getenv("VOLAND_DUMP_SHADERS");
  }
  if (getenv("VOLAND_DUMP_TEXTURES")) {
    emu.renderer.on_texture_decoded = dump_texture;
    emu.renderer.on_texture_user = getenv("VOLAND_DUMP_TEXTURES");
  }
  /* VOLAND_TRACE_DRAWS=START:LENGTH logs every draw in that slice window. */
  uint64_t trace_start = UINT64_MAX, trace_length = 0;
  if (getenv("VOLAND_TRACE_DRAWS")) {
    char *end = NULL;
    trace_start = strtoull(getenv("VOLAND_TRACE_DRAWS"), &end, 0);
    trace_length = (end && *end == ':') ? strtoull(end + 1, NULL, 0) : 1u;
  }
  while (slices < max_slices && (status == EMULATOR_RUNNING || status == EMULATOR_IDLE)) {
#ifndef _WIN32
    if (snapshot_dir && snapshot_at && slices == snapshot_at) {
      static Snapshot_Job job;
      memset(&job, 0, sizeof(job));
      job.max_slices = max_slices;
      job.trace_start = UINT64_MAX;
      snapshot_serve(snapshot_dir, &job); /* returns in a job's child */
      raster3d_restart_workers_after_fork(&emu.renderer);
      max_slices = job.max_slices;
      if (job.dump_every) dump_every = job.dump_every;
      if (job.frame_path[0]) frame_path = job.frame_path;
      if (job.trace_length) {
        trace_start = job.trace_start;
        trace_length = job.trace_length;
      }
      for (uint32_t k = 0; k < job.input_count && input_count < MAX_INPUT_EVENTS; k++) inputs[input_count++] = job.inputs[k];
      if (job.snapshot_at > slices) {
        static char nested_dir[sizeof(job.snapshot_dir)]; /* `job` is cleared before the next serve */
        memcpy(nested_dir, job.snapshot_dir, sizeof(nested_dir));
        snapshot_at = job.snapshot_at;
        snapshot_dir = nested_dir;
      }
    }
#endif
    if (input_count) apply_input(inputs, input_count, slices);
    emu.renderer.trace_draws = slices >= trace_start && slices - trace_start < trace_length;
    status = emulator_run_slice(&emu, budget);
    {
      /* A software keyboard is up: answer it like a player would. */
      static Am_Text_Request request;
      if (emulator_text_request(&emu, &request)) {
        const char *answer = swkbd_text ? swkbd_text : request.initial;
        fprintf(stderr, "voland-cli: software keyboard \"%s\" -> %s\n", request.header[0] ? request.header : request.guide,
                swkbd_cancel ? "(cancelled)" : answer);
        emulator_text_respond(&emu, answer, !swkbd_cancel);
      }
    }
    framebuffer_consume_all(); /* the CLI "displays" every frame at once */
    audio_frames += drain_audio(wav); /* and plays (or discards) every sample */
    slices++;
    if (frame_path && dump_every && slices % dump_every == 0) {
      char numbered[1024];
      snprintf(numbered, sizeof(numbered), "%s.%llu.ppm", frame_path, (unsigned long long)slices);
      (void)dump_frame(numbered);
    }
  }

  uint32_t width = 0, height = 0;
  const uint64_t frame_hash = newest_frame_hash(&width, &height);
  static const char *const k_status[] = {"running", "idle", "exited", "crashed", "deadlock", "not loaded"};
  fprintf(stderr, "voland-cli: %s after %llu slices, virtual time %llu ticks, %llu SVCs\n",
          k_status[status], (unsigned long long)slices, (unsigned long long)emu.scheduler.ticks,
          (unsigned long long)emu.hle.svc_call_count);
  if (width) fprintf(stderr, "voland-cli: frame %ux%u fnv1a64=%016llx\n", width, height, (unsigned long long)frame_hash);
  /* Where every live guest thread is: module + offset, for stalls. */
  for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS && emu.program_loaded; i++) {
    const Sched_Thread *th = &emu.scheduler.threads[i];
    if (th->state == THREAD_STATE_FREE || th->state == THREAD_STATE_DEAD || !th->thread.cpu_state) continue;
    const uint64_t pc = emu.cpu_backend->get_pc(th->thread.cpu_state);
    const char *module = "?";
    uint64_t offset = pc;
    for (uint32_t m = 0; m < emu.process.module_count; m++) {
      const Process_Module *mod = &emu.process.modules[m];
      if (pc >= mod->base_gva && pc < mod->base_gva + mod->image_size) {
        module = mod->name;
        offset = pc - mod->base_gva;
      }
    }
    static const char *const k_state[] = {"free", "created", "runnable", "waiting", "dead"};
    fprintf(stderr, "voland-cli: thread %llu (handle 0x%x) %s pc=%016llx (%s+0x%llx)\n", (unsigned long long)th->thread_id,
            th->handle,
            k_state[th->state], (unsigned long long)pc, module, (unsigned long long)offset);
    if (th->state == THREAD_STATE_WAITING) {
      uint32_t word = 0;
      (void)vmm_read32(emu.vmm, th->wait_address, &word);
      fprintf(stderr, "    wait kind %d, wake_at %llu, address 0x%llx (= 0x%x), handles", (int)th->wait,
              (unsigned long long)th->wake_at, (unsigned long long)th->wait_address, word);
      for (uint32_t h = 0; h < th->wait_handle_count && h < 4u; h++) {
        const Kernel_Object_Type type = handle_table_type_of(&emu.process.handles, th->wait_handles[h]);
        fprintf(stderr, " 0x%x(type %d)", th->wait_handles[h], (int)type);
      }
      fprintf(stderr, "\n");
    }
    if (getenv("VOLAND_BACKTRACE")) {
      /* AArch64 frame records: x29 -> {previous x29, return address}. */
      const CPU_Register_File *rf = emu.cpu_backend->get_register_file(th->thread.cpu_state);
      uint64_t fp = rf->x[29], lr = rf->x[30];
      for (uint32_t depth = 0; depth < 24u; depth++) {
        const char *m = "?";
        uint64_t off = lr;
        for (uint32_t k = 0; k < emu.process.module_count; k++) {
          const Process_Module *mod = &emu.process.modules[k];
          if (lr >= mod->base_gva && lr < mod->base_gva + mod->image_size) {
            m = mod->name;
            off = lr - mod->base_gva;
          }
        }
        fprintf(stderr, "    #%u %s+0x%llx\n", depth, m, (unsigned long long)off);
        uint64_t next_fp = 0, next_lr = 0;
        if (!fp || !error_is_ok(vmm_read64(emu.vmm, fp, &next_fp)) || !error_is_ok(vmm_read64(emu.vmm, fp + 8u, &next_lr)))
          break;
        fp = next_fp;
        lr = next_lr;
      }
    }
    if (getenv("VOLAND_DUMP_MODULE") && emu.process.module_count) {
      /* A module's whole image (VOLAND_DUMP_MODULE_INDEX, default 0), for
       * offline disassembly and symbolization. */
      FILE *f = fopen(getenv("VOLAND_DUMP_MODULE"), "wb");
      const char *which = getenv("VOLAND_DUMP_MODULE_INDEX");
      const uint32_t mi = which ? (uint32_t)atoi(which) : 0u;
      const Process_Module *mod = &emu.process.modules[mi < emu.process.module_count ? mi : 0u];
      for (uint64_t a = mod->base_gva; f && a < mod->base_gva + mod->image_size; a += 4) {
        uint32_t insn = 0;
        (void)vmm_read32(emu.vmm, a, &insn);
        fwrite(&insn, 4, 1, f);
      }
      if (f) fclose(f);
    }
    if (getenv("VOLAND_DUMP_REGS")) {
      const CPU_Register_File *rr = emu.cpu_backend->get_register_file(th->thread.cpu_state);
      for (uint32_t k = 0; k < 31u; k++)
        fprintf(stderr, "    x%-2u %016llx%s", k, (unsigned long long)rr->x[k], k % 4u == 3u ? "\n" : "");
      fprintf(stderr, "    sp  %016llx\n", (unsigned long long)rr->sp);
    }
    if (getenv("VOLAND_DUMP_PC")) {
      for (int64_t k = -12; k <= 4; k++) {
        uint32_t insn = 0;
        if (error_is_ok(vmm_read32(emu.vmm, pc + (uint64_t)(k * 4), &insn)))
          fprintf(stderr, "  %016llx: %08x\n", (unsigned long long)(pc + (uint64_t)(k * 4)), insn);
      }
    }
  }
  {
    const Raster3d_Stats *g = &emu.renderer.stats;
    if (g->draws || g->clears)
      fprintf(stderr,
              "voland-cli: gpu %llu clears, %llu draws (%llu skipped), %llu triangles, %llu pixels, "
              "%llu shader faults, %llu unknown ops, %llu texture misses\n",
              (unsigned long long)g->clears, (unsigned long long)g->draws, (unsigned long long)g->skipped_draws,
              (unsigned long long)g->triangles, (unsigned long long)g->pixels, (unsigned long long)g->shader_faults,
              (unsigned long long)g->unknown_ops, (unsigned long long)g->texture_misses);
  }
  if (backend == &CPU_BACKEND_JIT) {
    const Jit_Stats *j = jit_stats();
    fprintf(stderr,
            "voland-cli: jit %llu regions compiled (%llu blocks; %llu failed, %llu evicted, %llu KB of modules), "
            "%llu compiled / %llu interpreted block runs, %llu code generations (%llu blocks kept, %llu stale)\n",
            (unsigned long long)j->blocks_compiled, (unsigned long long)j->region_blocks,
            (unsigned long long)j->compile_failures,
            (unsigned long long)j->evictions, (unsigned long long)(j->module_bytes / 1024u),
            (unsigned long long)j->block_entries, (unsigned long long)j->interpreted_blocks,
            (unsigned long long)j->generations, (unsigned long long)j->revalidations, (unsigned long long)j->stale);
  }
  if (wav) {
    write_wav_header(wav, audio_frames);
    fclose(wav);
    fprintf(stderr, "voland-cli: audio %llu frames (%.2fs) -> %s\n", (unsigned long long)audio_frames,
            (double)audio_frames / AUDIO_RING_SAMPLE_RATE, audio_path);
  }
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
          "usage: voland-cli run <file.nca|file.nro> [--backend interpreter|jit|noop] [--budget N]\n"
          "                      [--max-slices N] [--test-card] [--expect-output TEXT]\n"
          "                      [--dump-frame FILE [--dump-frames-every N]]\n"
          "                      [--expect-frame-hash HEX]\n"
          "       voland-cli verify-dump <file>\n");
}


/* VOLAND_TRACE_CALLS="module+0xoff,...": log calls to those addresses
 * (x0-x3, the caller) - diagnostics for titles without symbols. */
static Emulator *g_trace_emu;
static void trace_hook(CPU_State *state, uint64_t target, uint64_t return_address, bool returning) {
  const CPU_Register_File *r = g_trace_emu->cpu_backend->get_register_file(state);
  if (returning) {
    fprintf(stderr, "[trace] return 0x%llx to 0x%llx: x0 %llx\n", (unsigned long long)target,
            (unsigned long long)return_address, (unsigned long long)r->x[0]);
    return;
  }
  fprintf(stderr, "[trace] call 0x%llx from 0x%llx: x0 %llx x1 %llx x2 %llx x3 %llx\n", (unsigned long long)target,
          (unsigned long long)return_address, (unsigned long long)r->x[0], (unsigned long long)r->x[1],
          (unsigned long long)r->x[2], (unsigned long long)r->x[3]);
  const char *dump = getenv("VOLAND_TRACE_DUMP_X1");
  if (dump) {
    uint8_t bytes[0x80];
    if (error_is_ok(vmm_read_block(g_trace_emu->vmm, r->x[1], bytes, sizeof(bytes)))) {
      for (uint32_t i = 0; i < sizeof(bytes); i += 16) {
        fprintf(stderr, "    +%02x", i);
        for (uint32_t j = 0; j < 16; j += 4) {
          uint32_t w;
          memcpy(&w, bytes + i + j, 4);
          fprintf(stderr, " %08x", w);
        }
        fprintf(stderr, "\n");
      }
    }
  }
}

static void setup_call_trace(Emulator *emu) {
  const char *spec = getenv("VOLAND_TRACE_CALLS");
  if (!spec || !emu->program_loaded) return;
  uint64_t targets[INTERP_TRACE_MAX];
  uint32_t count = 0;
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", spec);
  for (char *tok = strtok(buf, ","); tok && count < INTERP_TRACE_MAX; tok = strtok(NULL, ",")) {
    char *plus = strchr(tok, '+');
    if (!plus) continue;
    *plus = '\0';
    const uint64_t off = strtoull(plus + 1, NULL, 0);
    for (uint32_t m = 0; m < emu->process.module_count; m++)
      if (strcmp(emu->process.modules[m].name, tok) == 0) targets[count++] = emu->process.modules[m].base_gva + off;
  }
  g_trace_emu = emu;
  interp_set_call_trace(targets, count, trace_hook);
}

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "run")) return run(argc - 2, argv + 2);
  if (argc == 3 && !strcmp(argv[1], "verify-dump")) return verify_dump(argv[2]);
  usage();
  return EXIT_USAGE;
}
