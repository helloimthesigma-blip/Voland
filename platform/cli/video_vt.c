/**
 * voland-cli's native video decoder (DESIGN §13 "native backends"): the
 * core's NVDEC requests (core/video/video_stream.h Video_Backend) decoded
 * synchronously with macOS VideoToolbox into the video region's NV12
 * slots, so a native run shows - and --dump-frame captures - cutscenes.
 * Elsewhere there is no native decoder: requests are counted and
 * cutscenes stay black, as before.
 *
 * Optionally (VOLAND_DUMP_VIDEO=FILE) every access unit is appended to
 * FILE as Annex-B, for checking the reconstructed bitstream offline, and
 * (VOLAND_DUMP_VIDEO_FRAMES=PREFIX[:N]) every Nth decoded frame (default
 * 30) is written as PREFIX.<sequence>.ppm.
 */
#include "video_vt.h"

#include "video/vic_convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Vt_Decoder Vt_Decoder;
static void dump_access_unit(const uint8_t *data, uint32_t bytes);

#if defined(__APPLE__)
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#define NAL_TYPE_MASK 0x1Fu
#define NAL_SPS 7u
#define NAL_PPS 8u
#define NAL_AUD 9u
#define AVCC_LENGTH_BYTES 4
#define MAX_AU_BYTES (4u * 1024u * 1024u)

struct Vt_Decoder {
  Video_Stream *video;
  VTDecompressionSessionRef session;
  CMVideoFormatDescriptionRef format;
  uint8_t sps[256], pps[256];
  size_t sps_bytes, pps_bytes;
  uint32_t generation;
  uint64_t decoded, failed;
  uint8_t avcc[MAX_AU_BYTES];
};

static Vt_Decoder g_decoder;
static bool g_forked;

#define DEFAULT_FRAME_DUMP_EVERY 30u
#define PPM_BYTES_PER_PIXEL 3u

static void dump_frame(uint32_t sequence, const uint8_t *nv12, uint32_t width, uint32_t height) {
  static const char *prefix;
  static uint32_t every;
  static uint8_t rgba[VIDEO_MAX_WIDTH * VIDEO_MAX_HEIGHT * 4u];
  static char path[1024];
  if (!every) {
    const char *spec = getenv("VOLAND_DUMP_VIDEO_FRAMES");
    every = DEFAULT_FRAME_DUMP_EVERY;
    if (!spec) {
      every = UINT32_MAX;
      return;
    }
    static char copy[1024];
    snprintf(copy, sizeof(copy), "%s", spec);
    char *colon = strrchr(copy, ':');
    if (colon) {
      *colon = 0;
      every = (uint32_t)strtoul(colon + 1, NULL, 0);
      if (!every) every = DEFAULT_FRAME_DUMP_EVERY;
    }
    prefix = copy;
  }
  if (!prefix || sequence % every) return;
  const Vic_Nv12 source = {nv12, nv12 + (size_t)width * height, width, height, width};
  const Vic_Target target = {rgba, width, height, width * 4u, false, 0, false, 0xFF};
  vic_convert_nv12(&source, &target, VIC_MATRIX_BT709);
  snprintf(path, sizeof(path), "%s.%u.ppm", prefix, sequence);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%u %u\n255\n", width, height);
  for (uint32_t i = 0; i < width * height; i++) fwrite(rgba + (size_t)i * 4u, 1, PPM_BYTES_PER_PIXEL, f);
  fclose(f);
}

static void drop_session(Vt_Decoder *d) {
  if (d->session) {
    VTDecompressionSessionInvalidate(d->session);
    CFRelease(d->session);
    d->session = NULL;
  }
  if (d->format) {
    CFRelease(d->format);
    d->format = NULL;
  }
}

/* Copies a decoded CVPixelBuffer (NV12) into a slot, keyed by the
 * request's sequence number. */
static void on_frame(void *user, void *source_ref, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                     CMTime pts, CMTime duration) {
  (void)flags;
  (void)pts;
  (void)duration;
  Vt_Decoder *d = (Vt_Decoder *)user;
  const uint32_t sequence = (uint32_t)(uintptr_t)source_ref;
  if (status != noErr || !image) {
    d->failed++;
    return;
  }
  const int32_t slot = video_slot_acquire(d->video);
  if (slot < 0) return;
  CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
  const uint32_t width = (uint32_t)CVPixelBufferGetWidthOfPlane(image, 0) & ~1u;
  const uint32_t height = (uint32_t)CVPixelBufferGetHeightOfPlane(image, 0) & ~1u;
  const uint32_t w = width < VIDEO_MAX_WIDTH ? width : VIDEO_MAX_WIDTH;
  const uint32_t h = height < VIDEO_MAX_HEIGHT ? height : VIDEO_MAX_HEIGHT;
  uint8_t *out = video_slot_pixels(d->video, (uint32_t)slot);
  const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0);
  const uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(image, 1);
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
  for (uint32_t row = 0; row < h; row++) memcpy(out + (size_t)row * w, y + row * y_stride, w);
  for (uint32_t row = 0; row < h / 2u; row++) memcpy(out + (size_t)w * h + (size_t)row * w, uv + row * uv_stride, w);
  CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
  /* Synchronous decode in decode order: the sequence doubles as the
   * output index (VIC looks frames up by sequence). */
  video_slot_publish(d->video, (uint32_t)slot, sequence, sequence, d->generation, w, h, w, w * h);
  d->decoded++;
  dump_frame(sequence, out, w, h);
}

static bool make_session(Vt_Decoder *d) {
  const uint8_t *sets[2] = {d->sps, d->pps};
  const size_t sizes[2] = {d->sps_bytes, d->pps_bytes};
  if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, sizes, AVCC_LENGTH_BYTES,
                                                          &d->format) != noErr) {
    return false;
  }
  const int32_t pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
  const void *keys[1] = {kCVPixelBufferPixelFormatTypeKey};
  const void *values[1] = {number};
  CFDictionaryRef attributes =
      CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  VTDecompressionOutputCallbackRecord callback = {on_frame, d};
  const OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault, d->format, NULL, attributes, &callback,
                                                       &d->session);
  CFRelease(attributes);
  CFRelease(number);
  if (status != noErr) {
    fprintf(stderr, "voland-cli: VideoToolbox session failed (%d)\n", (int)status);
    d->session = NULL;
    return false;
  }
  return true;
}

static void vt_configure(void *user, uint32_t generation, uint32_t width, uint32_t height, const char *codec) {
  Vt_Decoder *d = (Vt_Decoder *)user;
  drop_session(d);
  d->generation = generation;
  d->sps_bytes = d->pps_bytes = 0;
  fprintf(stderr, "voland-cli: video stream %u: %s %ux%u (VideoToolbox)\n", generation, codec, width, height);
}

/* Annex-B -> 4-byte length prefixes; SPS/PPS are kept aside (a change
 * rebuilds the session) and access unit delimiters dropped. */
static size_t to_avcc(Vt_Decoder *d, const uint8_t *data, uint32_t bytes, bool *parameters_changed) {
  size_t out = 0;
  uint32_t i = 0;
  *parameters_changed = false;
  while (i + 3u <= bytes) {
    if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
      i++;
      continue;
    }
    const uint32_t start = i + 3u;
    uint32_t end = start;
    while (end + 3u <= bytes && !(data[end] == 0 && data[end + 1] == 0 && (data[end + 2] == 1 ||
                                                                          (data[end + 2] == 0 && end + 3u < bytes &&
                                                                           data[end + 3] == 1)))) {
      end++;
    }
    if (end + 3u > bytes) end = bytes;
    const uint32_t length = end - start;
    i = end;
    if (!length) continue;
    const uint32_t type = data[start] & NAL_TYPE_MASK;
    if (type == NAL_SPS || type == NAL_PPS) {
      uint8_t *keep = type == NAL_SPS ? d->sps : d->pps;
      size_t *kept = type == NAL_SPS ? &d->sps_bytes : &d->pps_bytes;
      if (length <= sizeof(d->sps) && (*kept != length || memcmp(keep, data + start, length))) {
        memcpy(keep, data + start, length);
        *kept = length;
        *parameters_changed = true;
      }
      continue;
    }
    if (type == NAL_AUD || out + AVCC_LENGTH_BYTES + length > sizeof(d->avcc)) continue;
    d->avcc[out++] = (uint8_t)(length >> 24);
    d->avcc[out++] = (uint8_t)(length >> 16);
    d->avcc[out++] = (uint8_t)(length >> 8);
    d->avcc[out++] = (uint8_t)length;
    memcpy(d->avcc + out, data + start, length);
    out += length;
  }
  return out;
}

static void vt_decode(void *user, uint32_t generation, uint32_t sequence, bool key, const uint8_t *data,
                      uint32_t bytes) {
  (void)key;
  Vt_Decoder *d = (Vt_Decoder *)user;
  dump_access_unit(data, bytes);
  if (generation != d->generation || g_forked) return;
  bool changed = false;
  const size_t length = to_avcc(d, data, bytes, &changed);
  if (changed) drop_session(d);
  if (!d->session && (!d->sps_bytes || !d->pps_bytes || !make_session(d))) return;
  if (!length) return;
  CMBlockBufferRef block = NULL;
  if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, d->avcc, length, kCFAllocatorNull, NULL, 0, length, 0,
                                         &block) != noErr) {
    return;
  }
  CMSampleBufferRef sample = NULL;
  const size_t sample_size = length;
  if (CMSampleBufferCreateReady(kCFAllocatorDefault, block, d->format, 1, 0, NULL, 1, &sample_size, &sample) ==
      noErr) {
    VTDecodeInfoFlags info = 0;
    const OSStatus status =
        VTDecompressionSessionDecodeFrame(d->session, sample, 0, (void *)(uintptr_t)sequence, &info);
    if (status != noErr) d->failed++;
    CFRelease(sample);
  }
  CFRelease(block);
}

const Video_Backend *video_vt_backend(Video_Stream *video) {
  static Video_Backend backend;
  g_decoder.video = video;
  backend = (Video_Backend){&g_decoder, vt_configure, vt_decode};
  return &backend;
}

void video_vt_forked(void) {
  g_forked = true;
  fprintf(stderr, "voland-cli: snapshot job: VideoToolbox off (fork), access units still dumped\n");
}

void video_vt_report(void) {
  if (g_decoder.decoded || g_decoder.failed) {
    fprintf(stderr, "voland-cli: video: %llu frames decoded, %llu failed (VideoToolbox)\n",
            (unsigned long long)g_decoder.decoded, (unsigned long long)g_decoder.failed);
  }
}

#else /* no native decoder */

static uint64_t g_requests;
static void none_configure(void *user, uint32_t generation, uint32_t width, uint32_t height, const char *codec) {
  (void)user;
  fprintf(stderr, "voland-cli: video stream %u: %s %ux%u (no native decoder: black)\n", generation, codec, width,
          height);
}
static void none_decode(void *user, uint32_t generation, uint32_t sequence, bool key, const uint8_t *data,
                        uint32_t bytes) {
  (void)user;
  (void)generation;
  (void)sequence;
  (void)key;
  dump_access_unit(data, bytes);
  g_requests++;
}
const Video_Backend *video_vt_backend(Video_Stream *video) {
  (void)video;
  static const Video_Backend backend = {NULL, none_configure, none_decode};
  return &backend;
}
void video_vt_forked(void) {}
void video_vt_report(void) {
  if (g_requests) fprintf(stderr, "voland-cli: video: %llu access units, not decoded\n", (unsigned long long)g_requests);
}
#endif

static void dump_access_unit(const uint8_t *data, uint32_t bytes) {
  static FILE *dump;
  static bool opened;
  if (!opened) {
    opened = true;
    const char *path = getenv("VOLAND_DUMP_VIDEO");
    if (path) dump = fopen(path, "wb");
  }
  if (dump) {
    fwrite(data, 1, bytes, dump);
    fflush(dump);
  }
}
