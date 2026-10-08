/**
 * GPU buffers mirroring guest GPU memory: see gpu_mirror.h.
 */
#include "gpu/gpu_mirror.h"

#include "common/log.h"
#include "gpu/gpu_channel.h"
#include "gpu/gpu_records.h"

#include <string.h>

#define PAGES (GPU_MIRROR_MAX_BYTES / GPU_MIRROR_PAGE_BYTES)
#define PAGE_MASK ((uint64_t)GPU_MIRROR_PAGE_BYTES - 1u)
#define WRITE_RUN_BYTES (256u * 1024u) /* largest BUFFER_WRITE record */
#define HASH_MULTIPLIER 0x9E3779B97F4A7C15ull
#define HASH_SEED 0x243F6A8885A308D3ull

typedef struct Mirror {
  uint32_t id;         /* buffer id; 0 = unused */
  uint64_t va;         /* page-aligned */
  uint32_t pages;
  uint32_t synced;     /* the submission last synced (+1; 0 = never) */
  uint64_t last_used;
  uint64_t hash[PAGES];      /* the guest copy as last seen (uploaded, or when taken) */
  bool known[PAGES];         /* hash is valid */
  bool gpu_owned[PAGES];
} Mirror;

static Mirror g_mirrors[GPU_MIRROR_COUNT];
static uint32_t g_next_id = 1;
static uint64_t g_clock;
static Gpu_Mirror_Stats g_stats;
static uint8_t g_page[GPU_MIRROR_PAGE_BYTES];
static uint8_t g_run[WRITE_RUN_BYTES];

/* Change detection, not cryptographic: 64-bit multiply-xorshift over words. */
static uint64_t page_hash(const uint8_t *p) {
  uint64_t h = HASH_SEED;
  for (uint32_t i = 0; i < GPU_MIRROR_PAGE_BYTES; i += 8u) {
    uint64_t w;
    memcpy(&w, p + i, 8);
    h = (h ^ w) * HASH_MULTIPLIER;
    h ^= h >> 29;
  }
  return h;
}

static void emit_u32s(Gpu_Stream *s, uint32_t type, const uint32_t *words, uint32_t count) {
  uint8_t *p = gpu_stream_begin(s, type, count * 4u);
  memcpy(p, words, count * 4u);
  gpu_stream_end(s);
}

static void destroy(Gpu_Stream *s, Mirror *m) {
  if (!m->id) return;
  if (s) emit_u32s(s, GPU_REC_BUFFER_DESTROY, &m->id, 1);
  memset(m, 0, sizeof(*m));
  g_stats.destroyed++;
}

void gpu_mirror_reset(Gpu_Stream *stream) {
  for (uint32_t i = 0; i < GPU_MIRROR_COUNT; i++) destroy(stream, &g_mirrors[i]);
}

const Gpu_Mirror_Stats *gpu_mirror_stats(void) { return &g_stats; }

uint32_t gpu_mirror_bytes(uint32_t id) {
  for (uint32_t i = 0; i < GPU_MIRROR_COUNT; i++)
    if (g_mirrors[i].id == id && id) return g_mirrors[i].pages * GPU_MIRROR_PAGE_BYTES;
  return 0;
}

static Mirror *find(uint64_t va, uint64_t bytes) {
  for (uint32_t i = 0; i < GPU_MIRROR_COUNT; i++) {
    Mirror *m = &g_mirrors[i];
    if (m->id && va >= m->va && va + bytes <= m->va + (uint64_t)m->pages * GPU_MIRROR_PAGE_BYTES) return m;
  }
  return NULL;
}

/* Uploads pages [first, first + count) from the guest copy in g_run. */
static void upload(Gpu_Stream *s, const Mirror *m, uint32_t first, uint32_t count) {
  const uint32_t bytes = count * GPU_MIRROR_PAGE_BYTES;
  uint8_t *p = gpu_stream_begin(s, GPU_REC_BUFFER_WRITE, 12u + bytes);
  const uint32_t head[3] = {m->id, first * GPU_MIRROR_PAGE_BYTES, bytes};
  memcpy(p, head, sizeof(head));
  memcpy(p + sizeof(head), g_run, bytes);
  gpu_stream_end(s);
  g_stats.pages_uploaded += count;
}

/* Every CPU-owned page whose guest copy changed goes up; a GPU-owned page
 * whose guest copy changed was rewritten by the guest and is CPU-owned
 * again. Consecutive pages share a write. */
static void sync(Gpu_Stream *s, const Gpu_Memory *mem, Mirror *m) {
  uint32_t run_first = 0, run_count = 0;
  for (uint32_t i = 0; i <= m->pages; i++) {
    bool send = false;
    if (i < m->pages) {
      if (!mem->read(mem->user, m->va + (uint64_t)i * GPU_MIRROR_PAGE_BYTES, g_page, GPU_MIRROR_PAGE_BYTES))
        memset(g_page, 0, sizeof(g_page));
      const uint64_t h = page_hash(g_page);
      g_stats.pages_hashed++;
      if (m->gpu_owned[i] && m->known[i] && h != m->hash[i]) {
        m->gpu_owned[i] = false;
        g_stats.pages_returned++;
      }
      send = !m->gpu_owned[i] && (!m->known[i] || h != m->hash[i]);
      m->hash[i] = h;
      m->known[i] = true;
      if (send) {
        if (!run_count) run_first = i;
        memcpy(g_run + (size_t)run_count * GPU_MIRROR_PAGE_BYTES, g_page, GPU_MIRROR_PAGE_BYTES);
        run_count++;
      }
    }
    if (run_count && (!send || run_count * GPU_MIRROR_PAGE_BYTES == WRITE_RUN_BYTES || i == m->pages)) {
      upload(s, m, run_first, run_count);
      run_count = 0;
    }
  }
}

uint32_t gpu_mirror_window(Gpu_Stream *stream, const Gpu_Memory *mem, uint64_t va, uint32_t bytes,
                           uint32_t submission, uint32_t *offset) {
  if (!stream || !bytes) return 0;
  const uint64_t lo = va & ~PAGE_MASK, hi = (va + bytes + PAGE_MASK) & ~PAGE_MASK;
  if (hi - lo > GPU_MIRROR_MAX_BYTES) return 0;
  Mirror *m = find(va, bytes);
  if (!m) {
    /* A new mirror over the union with any it overlaps (their GPU-owned
     * pages are lost: the guest has never seen them). */
    uint64_t new_lo = lo, new_hi = hi;
    for (uint32_t i = 0; i < GPU_MIRROR_COUNT; i++) {
      Mirror *o = &g_mirrors[i];
      const uint64_t o_hi = o->va + (uint64_t)o->pages * GPU_MIRROR_PAGE_BYTES;
      if (!o->id || o->va >= new_hi || o_hi <= new_lo) continue;
      if ((o->va < new_lo ? new_hi - o->va : o_hi - new_lo) > GPU_MIRROR_MAX_BYTES) continue;
      if (o->va < new_lo) new_lo = o->va;
      if (o_hi > new_hi) new_hi = o_hi;
    }
    Mirror *slot = NULL;
    for (uint32_t i = 0; i < GPU_MIRROR_COUNT; i++) {
      Mirror *o = &g_mirrors[i];
      const uint64_t o_hi = o->va + (uint64_t)o->pages * GPU_MIRROR_PAGE_BYTES;
      if (o->id && o->va < new_hi && o_hi > new_lo) {
        for (uint32_t p = 0; p < o->pages; p++)
          if (o->gpu_owned[p]) {
            log_warn("[gpu] mirror at 0x%llx grows over GPU-written pages: dropped", (unsigned long long)o->va);
            break;
          }
        destroy(stream, o);
      }
      if (!o->id && !slot) slot = o;
    }
    if (!slot) { /* full: the least recently used goes */
      slot = &g_mirrors[0];
      for (uint32_t i = 1; i < GPU_MIRROR_COUNT; i++)
        if (g_mirrors[i].last_used < slot->last_used) slot = &g_mirrors[i];
      destroy(stream, slot);
    }
    m = slot;
    m->id = g_next_id++;
    m->va = new_lo;
    m->pages = (uint32_t)((new_hi - new_lo) / GPU_MIRROR_PAGE_BYTES);
    const uint32_t create[2] = {m->id, m->pages * GPU_MIRROR_PAGE_BYTES};
    emit_u32s(stream, GPU_REC_BUFFER_CREATE, create, 2);
    g_stats.created++;
  }
  m->last_used = ++g_clock;
  if (m->synced != submission + 1u) {
    sync(stream, mem, m);
    m->synced = submission + 1u;
  }
  *offset = (uint32_t)(va - m->va);
  return m->id;
}

void gpu_mirror_take(const Gpu_Memory *mem, uint64_t va, uint32_t bytes) {
  Mirror *m = find(va, bytes);
  if (!m || !bytes) return;
  const uint32_t first = (uint32_t)((va - m->va) / GPU_MIRROR_PAGE_BYTES);
  const uint32_t last = (uint32_t)((va + bytes - 1u - m->va) / GPU_MIRROR_PAGE_BYTES);
  for (uint32_t p = first; p <= last && p < m->pages; p++) {
    if (m->gpu_owned[p]) continue;
    /* The guest copy as it is now: if it changes, the guest rewrote it. */
    if (!m->known[p]) {
      if (!mem->read(mem->user, m->va + (uint64_t)p * GPU_MIRROR_PAGE_BYTES, g_page, GPU_MIRROR_PAGE_BYTES))
        memset(g_page, 0, sizeof(g_page));
      m->hash[p] = page_hash(g_page);
      m->known[p] = true;
    }
    m->gpu_owned[p] = true;
    g_stats.pages_taken++;
  }
}

bool gpu_mirror_resident(uint64_t va, uint64_t bytes, uint32_t *id, uint32_t *offset) {
  if (!bytes) return false;
  const Mirror *m = find(va, bytes);
  if (!m) {
    /* Partly inside a mirror with GPU-owned pages: the caller cannot
     * read it whole from either side - report it as not resident. */
    return false;
  }
  const uint32_t first = (uint32_t)((va - m->va) / GPU_MIRROR_PAGE_BYTES);
  const uint32_t last = (uint32_t)((va + bytes - 1u - m->va) / GPU_MIRROR_PAGE_BYTES);
  for (uint32_t p = first; p <= last; p++) {
    if (!m->gpu_owned[p]) continue;
    *id = m->id;
    *offset = (uint32_t)(va - m->va);
    return true;
  }
  return false;
}

void gpu_mirror_cpu_wrote(uint64_t va, uint64_t bytes) {
  for (uint32_t i = 0; i < GPU_MIRROR_COUNT; i++) {
    Mirror *m = &g_mirrors[i];
    const uint64_t m_hi = m->va + (uint64_t)m->pages * GPU_MIRROR_PAGE_BYTES;
    if (!m->id || va >= m_hi || va + bytes <= m->va) continue;
    const uint64_t lo = va > m->va ? va : m->va, hi = va + bytes < m_hi ? va + bytes : m_hi;
    for (uint64_t p = (lo - m->va) / GPU_MIRROR_PAGE_BYTES; p <= (hi - 1u - m->va) / GPU_MIRROR_PAGE_BYTES; p++) {
      m->gpu_owned[p] = false;
      m->known[p] = false; /* uploads at the next sync */
    }
    m->synced = 0;
  }
}
