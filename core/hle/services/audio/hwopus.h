/**
 * hwopus - the hardware Opus decoder (§14). Titles decode voice and some
 * music through it (Super Smash Bros. Ultimate's videos and menus, The
 * Legend of Zelda: Tears of the Kingdom's voices). Decoding is the
 * vendored libopus (core/third_party/opus, decoder only, fixed point).
 * Each decoder object owns a run of slots in a fixed pool that lives in
 * Hwopus_State - so it is part of the Emulator and of every save state,
 * and nothing is allocated per packet. When the pool is full or a stream
 * layout is too large for it, that decoder falls back to the old
 * behaviour: the right number of silent samples, read from the packet's
 * TOC byte (RFC 6716 §3.1), so the title keeps running without that
 * stream's sound.
 *
 *   0/4 Open(HardwareOpusDecoder|Ex) {u32 sample rate, u32 channels, ...},
 *       2/6 Open...ForMultiStream(Ex) (parameters in a buffer: rate,
 *       channels, total streams, stereo streams, u8 mapping[256]), and
 *       their GetWorkBufferSize* (1, 3, 5, 7, 8, 9) -> IHardwareOpusDecoder.
 *   IHardwareOpusDecoder: 0/2/4/5/6/7/8/9 DecodeInterleaved* - input: an
 *       8-byte big-endian header (packet size, final range) + the packet;
 *       output: interleaved s16 PCM -> u32 bytes consumed, u32 samples
 *       (per channel), and u64 0 for the *WithPerf forms. 6/7/8/9 take a
 *       bool "reset" in their raw data that resets the decoder first;
 *       1/3 SetContext.
 */
#ifndef SWITCH_HLE_SERVICES_AUDIO_HWOPUS_H
#define SWITCH_HLE_SERVICES_AUDIO_HWOPUS_H

#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define HWOPUS_WORK_BUFFER_BYTES 0x40000u
#define HWOPUS_PACKET_HEADER_BYTES 8u
#define HWOPUS_MODULE 111u
#define HWOPUS_RESULT_INVALID_PACKET ((6u << 9) | HWOPUS_MODULE)

/* The decoder pool. One slot holds a mono or stereo decoder or a
 * two-stream multistream one (libopus 1.5.2 fixed point: 17860, 26580 and
 * 44720 bytes); a larger multistream decoder takes a run of slots. */
#define HWOPUS_SLOT_BYTES 0xB000u
#define HWOPUS_SLOTS 16u
#define HWOPUS_MAX_CHANNELS 8u /* largest layout decoded; more decode to silence */

typedef struct Hwopus_State {
  Service_Interface service;
  Service_Interface decoder; /* object state: see hwopus.c decoder_state() */
  bool slot_used[HWOPUS_SLOTS];
  _Alignas(16) uint8_t slots[HWOPUS_SLOTS][HWOPUS_SLOT_BYTES];
} Hwopus_State;

/* Samples per channel in Opus `packet` (`bytes` long) at `sample_rate`
 * (RFC 6716 §3.1: TOC configuration and frame-count code); 0 if malformed. */
uint32_t hwopus_packet_samples(const uint8_t *packet, uint32_t bytes, uint32_t sample_rate);

void hwopus_init(Hwopus_State *state);
Error hwopus_register(Hwopus_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_AUDIO_HWOPUS_H */
