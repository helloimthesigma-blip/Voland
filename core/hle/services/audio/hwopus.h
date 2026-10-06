/**
 * hwopus - the hardware Opus decoder (§14). Titles decode voice and some
 * music through it (Super Smash Bros. Ultimate's videos and menus open it
 * at start-up). Voland has no Opus decoder yet: each packet "decodes" to
 * the right number of silent samples, read from the packet's TOC byte
 * (RFC 6716 §3.1), so a title's audio timing and buffers stay correct and
 * it keeps running - without that stream's sound.
 *
 *   0/4 Open(HardwareOpusDecoder|Ex) {u32 sample rate, u32 channels, ...},
 *       2/6 Open...ForMultiStream(Ex) (parameters in a buffer), and their
 *       GetWorkBufferSize* (1, 3, 5, 7, 8, 9) -> IHardwareOpusDecoder.
 *   IHardwareOpusDecoder: 0/2/4/5/6/7/8/9 DecodeInterleaved* - input: an
 *       8-byte big-endian header (packet size, final range) + the packet;
 *       output: interleaved s16 PCM -> u32 bytes consumed, u32 samples
 *       (per channel), and u64 0 for the *WithPerf forms; 1/3 SetContext.
 */
#ifndef SWITCH_HLE_SERVICES_AUDIO_HWOPUS_H
#define SWITCH_HLE_SERVICES_AUDIO_HWOPUS_H

#include "hle/kernel/ipc.h"
#include "hle/services/sm/sm.h"

#define HWOPUS_WORK_BUFFER_BYTES 0x40000u
#define HWOPUS_PACKET_HEADER_BYTES 8u
#define HWOPUS_MODULE 111u
#define HWOPUS_RESULT_INVALID_PACKET ((6u << 9) | HWOPUS_MODULE)

typedef struct Hwopus_State {
  Service_Interface service;
  Service_Interface decoder; /* object state: sample rate << 8 | channels */
} Hwopus_State;

/* Samples per channel in Opus `packet` (`bytes` long) at `sample_rate`
 * (RFC 6716 §3.1: TOC configuration and frame-count code); 0 if malformed. */
uint32_t hwopus_packet_samples(const uint8_t *packet, uint32_t bytes, uint32_t sample_rate);

void hwopus_init(Hwopus_State *state);
Error hwopus_register(Hwopus_State *state, SM_Registry *registry);

#endif /* SWITCH_HLE_SERVICES_AUDIO_HWOPUS_H */
