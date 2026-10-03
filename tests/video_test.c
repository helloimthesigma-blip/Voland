/**
 * Video decode (DESIGN §13): the H.264 picture-setup parse
 * (hle/services/nvdrv/nvdec.h) and the video stream's requests and
 * frame slots (video/video_stream.h), web ring and native backend alike.
 */
#define CHECK_NAME "video_test"
#include "check.h"

#include "hle/services/nvdrv/nvdec.h"
#include "video/h264.h"
#include "video/video_stream.h"

#include <string.h>

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t rd32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static void test_h264_params(void) {
  static uint8_t s[NVDEC_PIC_SETUP_BYTES];
  memset(s, 0, sizeof(s));
  wr32(s + 0x48, 2733);         /* stream_len */
  wr32(s + 0x58, 2);            /* log2_max_pic_order_cnt_lsb_minus4 */
  wr32(s + 0x60, 1);            /* frame_mbs_only */
  wr32(s + 0x64, 80);           /* width in MBs */
  wr32(s + 0x68, 45);           /* height in MBs */
  wr32(s + 0x70, 1);            /* CABAC */
  wr32(s + 0x78, 3);            /* num_ref_idx_l0_active_minus1 */
  wr32(s + 0x80, 1);            /* deblocking control present */
  wr32(s + 0x88, 1);            /* transform 8x8 */
  /* direct_8x8 (bit 1), weighted_pred (bit 2), log2_max_frame_num_minus4 = 5,
   * pic_init_qp_minus26 = -3, chroma_qp_index_offset = -2, second = -2. */
  const uint32_t flags = (1u << 1) | (1u << 2) | (5u << 8) | ((uint32_t)(-3 & 0x3F) << 16) |
                         ((uint32_t)(-2 & 0x1F) << 22) | ((uint32_t)(-2 & 0x1F) << 27);
  wr32(s + 0xB0, flags);
  wr32(s + 0xB4, 2);            /* weighted_bipred_idc */
  memset(s + 0x1C0, 16, 96 + 128); /* flat scaling lists */
  H264_Params p;
  nvdec_h264_params(s, &p);
  CHECK(nvdec_h264_stream_length(s) == 2733u);
  CHECK(p.width_mbs == 80u && p.height_mbs == 45u && p.frame_mbs_only);
  CHECK(p.log2_max_poc_lsb_minus4 == 2u && p.entropy_coding_mode && p.num_ref_idx_l0_default_minus1 == 3u);
  CHECK(p.deblocking_filter_control_present && p.transform_8x8_mode && p.direct_8x8_inference && p.weighted_pred);
  CHECK(p.log2_max_frame_num_minus4 == 5u && p.pic_init_qp_minus26 == -3);
  CHECK(p.chroma_qp_index_offset == -2 && p.second_chroma_qp_index_offset == -2 && p.weighted_bipred_idc == 2u);
  CHECK(p.chroma_format_idc == 1u && !p.scaling_matrix_present);
  /* A non-flat 4x4 list turns the matrices on, read in zig-zag order. */
  s[0x1C0 + 4] = 20; /* raster (row 1, col 0) = zig-zag position 2 */
  nvdec_h264_params(s, &p);
  CHECK(p.scaling_matrix_present && p.scaling_4x4[0][2] == 20u && p.scaling_4x4[0][1] == 16u);
  /* Parameter sets come out as two Annex-B NAL units. */
  uint8_t ps[512];
  const size_t n = h264_write_parameter_sets(&p, ps, sizeof(ps));
  CHECK(n > 10u && rd32(ps) == 0x01000000u && ps[4] == 0x67);
  bool pps = false;
  for (size_t i = 5; i + 4 < n; i++) pps |= ps[i] == 0 && ps[i + 1] == 0 && ps[i + 2] == 0 && ps[i + 3] == 1 && ps[i + 4] == 0x68;
  CHECK(pps);
  char codec[16];
  h264_codec_string(codec);
  CHECK(!strcmp(codec, "avc1.640033"));
}

static uint8_t g_region[VIDEO_REGION_BYTES];

typedef struct Backend_Log {
  uint32_t configures, decodes, last_sequence, last_bytes;
  bool last_key;
} Backend_Log;

static void log_configure(void *user, uint32_t generation, uint32_t width, uint32_t height, const char *codec) {
  (void)generation;
  (void)codec;
  Backend_Log *log = (Backend_Log *)user;
  CHECK(width == 1280u && height == 720u);
  log->configures++;
}
static void log_decode(void *user, uint32_t generation, uint32_t sequence, bool key, const uint8_t *data,
                       uint32_t bytes) {
  (void)generation;
  (void)data;
  Backend_Log *log = (Backend_Log *)user;
  log->decodes++;
  log->last_sequence = sequence;
  log->last_key = key;
  log->last_bytes = bytes;
}

static void test_stream_ring(void) {
  Video_Stream v;
  video_stream_init(&v, g_region, NULL, NULL);
  CHECK(rd32(g_region) == GPU_STREAM_MAGIC);
  video_configure(&v, 1280, 720, "avc1.640033");
  const uint8_t au[5] = {0, 0, 1, 0x65, 0x88};
  CHECK(video_decode(&v, true, au, sizeof(au)) == 1u);
  CHECK(video_decode(&v, false, au, sizeof(au)) == 2u);
  /* The ring holds CONFIGURE then two DECODEs, framed like the GPU stream. */
  const uint8_t *ring = g_region + VIDEO_RING_OFFSET;
  CHECK(rd32(ring) == VIDEO_REC_CONFIGURE && rd32(ring + 8) == 1u && rd32(ring + 12) == 1280u);
  CHECK(!strcmp((const char *)ring + 8 + 16, "avc1.640033"));
  const uint8_t *d = ring + rd32(ring + 4);
  CHECK(rd32(d) == VIDEO_REC_DECODE && rd32(d + 8) == 1u && rd32(d + 12) == 1u && rd32(d + 16) == VIDEO_DECODE_KEY);
  CHECK(rd32(d + 20) == sizeof(au) && !memcmp(d + 24, au, sizeof(au)));
  uint64_t write = 0;
  memcpy(&write, g_region + GPU_STREAM_OFF_WRITE, 8);
  CHECK(write > 0);
}

static void test_slots(void) {
  Video_Stream v;
  Backend_Log log = {0};
  const Video_Backend backend = {&log, log_configure, log_decode};
  video_stream_init(&v, g_region, NULL, &backend);
  video_configure(&v, 1280, 720, "avc1.640033");
  const uint8_t au[4] = {0, 0, 1, 0x65};
  for (uint32_t i = 0; i < 3; i++) (void)video_decode(&v, i == 0, au, sizeof(au));
  CHECK(log.configures == 1u && log.decodes == 3u && log.last_sequence == 3u && !log.last_key && log.last_bytes == 4u);
  Video_Frame f;
  CHECK(!video_frame_for(&v, 1, &f)); /* nothing decoded yet */
  /* The decoder publishes sequences 2 then 1 (display order). */
  const int32_t a = video_slot_acquire(&v), b = video_slot_acquire(&v);
  CHECK(a >= 0 && b >= 0 && a != b);
  video_slot_pixels(&v, (uint32_t)a)[0] = 0xAA;
  video_slot_publish(&v, (uint32_t)a, 1, 2, v.generation, 1280, 720, 1280, 1280 * 720);
  video_slot_publish(&v, (uint32_t)b, 2, 1, v.generation, 1280, 720, 1280, 1280 * 720);
  CHECK(video_frame_for(&v, 2, &f) && f.slot == (uint32_t)a && f.luma[0] == 0xAA && f.chroma == f.luma + 1280 * 720);
  CHECK(video_frame_for(&v, 3, &f) && f.output == 2u); /* not decoded yet: the newest stands in */
  /* Shown in display order 1, 2: showing 2 frees 1; 2 stays (shown again works). */
  CHECK(video_frame_for(&v, 1, &f) && f.slot == (uint32_t)b);
  video_frame_used(&v, &f);
  CHECK(video_frame_for(&v, 2, &f) && f.slot == (uint32_t)a);
  video_frame_used(&v, &f);
  CHECK(video_frame_for(&v, 2, &f) && f.slot == (uint32_t)a);
  CHECK(video_slot_acquire(&v) == b); /* sequence 1's slot is free again */
  /* A frame the stream moved well past without showing it is freed too. */
  const int32_t c = video_slot_acquire(&v);
  video_slot_publish(&v, (uint32_t)b, 3, 3, v.generation, 16, 16, 16, 256);
  video_slot_publish(&v, (uint32_t)c, 4, 3 + VIDEO_SLOT_COUNT + 1u, v.generation, 16, 16, 16, 256);
  CHECK(video_frame_for(&v, 3 + VIDEO_SLOT_COUNT + 1u, &f) && f.slot == (uint32_t)c);
  video_frame_used(&v, &f);
  CHECK(!video_frame_for(&v, 3, &f) || f.sequence != 3u);
  /* A new stream drops the old one's frames. */
  video_configure(&v, 1280, 720, "avc1.640033");
  CHECK(!video_frame_for(&v, 1, &f));
}

int main(void) {
  test_h264_params();
  test_stream_ring();
  test_slots();
  printf("[video_test] passed\n");
  return 0;
}
