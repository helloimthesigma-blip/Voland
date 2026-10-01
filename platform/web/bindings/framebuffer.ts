/**
 * Mirror of core/gpu/framebuffer.h: the §6 two-slot frame handoff header.
 * The GPU worker is the consumer: it presents the NEWEST published slot
 * and then sets consume = publish (stale frames are skipped).
 */

export const FRAMEBUFFER_HEADER_BYTES = 128;           /* LAYOUT_FRAMEBUFFER_HEADER_BYTES */
export const FRAMEBUFFER_SLOT_BYTES = 1920 * 1080 * 4; /* LAYOUT_FRAMEBUFFER_SLOT_BYTES */
export const FRAMEBUFFER_SLOT_COUNT = 2;
export const PUBLISH_INDEX = 0;                        /* Int32 index of the publish counter */
export const CONSUME_INDEX = 1;
const OFFSET_METADATA = 8;
const METADATA_BYTES = 24;
export const FRAMEBUFFER_FORMAT_RGBA8 = 1;

export interface PublishedFrame {
  readonly slot: number;
  readonly width: number;
  readonly height: number;
  readonly stride: number;
  readonly format: number;
  readonly frameNumber: bigint;
  readonly pixelOffset: number; /* byte offset of the slot within the region */
}

/** Describes the newest frame given the publish counter's value, or null
 * if nothing was ever published. Pure: reads only the header view. */
export function newestFrame(header: DataView, published: number): PublishedFrame | null {
  if (published === 0) return null;
  const slot = (published - 1) % FRAMEBUFFER_SLOT_COUNT;
  const meta = OFFSET_METADATA + slot * METADATA_BYTES;
  return {
    slot,
    width: header.getUint32(meta, true),
    height: header.getUint32(meta + 4, true),
    stride: header.getUint32(meta + 8, true),
    format: header.getUint32(meta + 12, true),
    frameNumber: header.getBigUint64(meta + 16, true),
    pixelOffset: FRAMEBUFFER_HEADER_BYTES + slot * FRAMEBUFFER_SLOT_BYTES,
  };
}
