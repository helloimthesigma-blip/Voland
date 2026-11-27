/**
 * Vertex-program captures: see vs_capture.h.
 */
#include "gpu/vs_capture.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gpu/wgsl.h"

#define VS_CAPTURE_WGSL_BYTES (1u << 20)
#define VS_CAPTURE_CBUF_BYTES 0x10000u /* a constant buffer at most */
#define VS_CAPTURE_GLOBAL_BYTES (1u << 20) /* the global buffers' copies at most */
#define VS_CAPTURE_PIECE_BYTES 512u

typedef struct Out {
  Vs_Capture_Emit emit;
  void *user;
} Out;

static void out(const Out *o, const char *format, ...) {
  char piece[VS_CAPTURE_PIECE_BYTES];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(piece, sizeof(piece), format, args);
  va_end(args);
  if (n > 0) o->emit(o->user, piece, (size_t)n < sizeof(piece) ? (size_t)n : sizeof(piece) - 1u);
}

static void json_words(const Out *o, const uint32_t *w, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) out(o, "%s%u", i ? "," : "", w[i]);
}

void vs_capture_write(const Sm_Program *program, const Sm_Env *env, const Sm_Thread *t, Vs_Capture_Emit emit, void *user) {
  const Out o_value = {emit, user};
  const Out *o = &o_value;
  static Wgsl_Program_Desc desc;
  wgsl_default_desc(program, &desc, WGSL_BASE_VARYINGS);
  const char *skip = NULL;
  if (desc.texture_count) skip = "samples textures";
  static Wgsl_Globals globals;
  globals.count = 0;
  if (!skip && wgsl_reads_globals(program) && !wgsl_find_globals(program, &globals))
    skip = "reads global memory through untraced addresses";
  else if (program->uses_bindless_textures) skip = "bindless textures";
  static char wgsl[VS_CAPTURE_WGSL_BYTES];
  Wgsl_Result res = {0};
  if (!skip) {
    res = wgsl_translate(program, &desc, wgsl, sizeof(wgsl));
    if (!res.ok) skip = res.reason;
  }
  out(o, "{\"hash\":\"%08x\",\"address\":\"%llx\"", program->hash, (unsigned long long)program->address);
  if (skip) {
    out(o, ",\"skip\":\"%s\"}\n", skip);
    return;
  }
  /* D: the vertex stage's constant-buffer table; P: the buffers it reads. */
  static uint32_t pool[(SM_CBUF_SLOTS * VS_CAPTURE_CBUF_BYTES + VS_CAPTURE_GLOBAL_BYTES) / 4u];
  uint32_t table[2u * SM_CBUF_SLOTS] = {0}, used = 0;
  for (uint32_t s = 0; s < SM_CBUF_SLOTS; s++) {
    if (!((res.cbuf_slots >> s) & 1u) || !env->cbuf[s]) continue;
    const uint32_t extent = program->cbuf_extent[s] < env->cbuf_size[s] ? program->cbuf_extent[s] : env->cbuf_size[s];
    const uint32_t n = (extent + 3u) / 4u;
    memset(pool + used, 0, (size_t)n * 4u);
    memcpy(pool + used, env->cbuf[s], extent);
    table[2u * s] = used;
    table[2u * s + 1u] = n;
    used += n;
  }
  /* Each global buffer: its descriptor {base, size} from the constant
   * buffer, its bytes and a word of slack (gpu_copy_globals). */
  uint32_t global_table[WGSL_GLOBAL_WORDS * WGSL_MAX_GLOBALS] = {0};
  uint32_t global_bytes = 0;
  for (uint32_t k = 0; k < globals.count && !skip; k++) {
    const uint32_t slot = globals.slot[k], at = globals.offset[k];
    uint32_t d[3] = {0};
    if (env->cbuf[slot] && at + sizeof(d) <= env->cbuf_size[slot]) memcpy(d, env->cbuf[slot] + at, sizeof(d));
    const uint32_t n = (d[2] + 3u) / 4u + 1u;
    global_bytes += n * 4u;
    if (!d[2] || global_bytes > VS_CAPTURE_GLOBAL_BYTES || !env->global_read) {
      skip = "global buffer not captured";
      break;
    }
    memset(pool + used, 0, (size_t)n * 4u);
    (void)env->global_read(env->user, (uint64_t)d[0] | (uint64_t)d[1] << 32, pool + used, d[2]);
    uint32_t *e = global_table + WGSL_GLOBAL_WORDS * k;
    e[0] = d[0];
    e[1] = d[1];
    e[2] = d[2];
    e[3] = used * 4u;
    used += n;
  }
  if (skip) {
    out(o, ",\"skip\":\"%s\"}\n", skip);
    return;
  }
  out(o, ",\"globals_table\":%u,\"globals\":[", WGSL_DRAW_GLOBALS);
  json_words(o, global_table, WGSL_GLOBAL_WORDS * globals.count);
  out(o, "]");
  out(o, ",\"d_words\":%u,\"cbuf_table\":%u,\"table\":[", WGSL_DRAW_CONSTANT_WORDS, WGSL_DRAW_VS_CBUF_TABLE);
  json_words(o, table, 2u * SM_CBUF_SLOTS);
  out(o, "],\"pool\":[");
  json_words(o, pool, used);
  /* Compared: the position and every generic output the header declares. */
  uint32_t words[SM_ATTRIBUTE_WORDS], nw = 0;
  for (uint32_t c4 = 0; c4 < 4u; c4++) words[nw++] = SM_ATTR_POSITION / 4u + c4;
  for (uint32_t b = 0; b < 4u * SM_ATTR_GENERIC_COUNT; b++)
    if ((program->header.output_generic[b / 32u] >> (b % 32u)) & 1u) words[nw++] = SM_ATTR_GENERIC / 4u + b;
  out(o, "],\"inputs\":%u,\"words\":[", desc.input_count);
  json_words(o, words, nw);
  out(o, "],\"lanes\":[");
  uint8_t vector_of[SM_ATTR_GENERIC_COUNT];
  for (uint32_t v = 0; v < SM_ATTR_GENERIC_COUNT; v++)
    if (desc.input_location[v] < desc.input_count) vector_of[desc.input_location[v]] = (uint8_t)v;
  for (uint32_t l = 0; l < t->lanes; l++) {
    out(o, "%s{\"ids\":[%u,%u],\"in\":[", l ? "," : "", t->vertex_id[l], t->instance_id[l]);
    for (uint32_t i = 0; i < desc.input_count; i++)
      for (uint32_t c4 = 0; c4 < 4u; c4++)
        out(o, "%s%u", i || c4 ? "," : "", t->attr_in[SM_ATTR_GENERIC / 4u + 4u * vector_of[i] + c4][l]);
    out(o, "],\"out\":[");
    for (uint32_t i = 0; i < nw; i++) out(o, "%s%u", i ? "," : "", t->attr_out[words[i]][l]);
    out(o, "]}");
  }
  out(o, "],\"wgsl\":\"");
  for (const char *p = wgsl; *p; p++) {
    if (*p == '\n') out(o, "\\n");
    else if (*p == '"' || *p == '\\') out(o, "\\%c", *p);
    else out(o, "%c", *p);
  }
  out(o, "\"}\n");
}
