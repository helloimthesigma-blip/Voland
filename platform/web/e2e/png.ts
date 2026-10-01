/**
 * Minimal PNG decoder for screenshot assertions (8-bit RGB/RGBA,
 * non-interlaced - what Chromium's screenshots are). Node's zlib does the
 * inflate; this undoes the per-row filters.
 */
import { inflateSync } from "node:zlib";

export interface Image {
  readonly width: number;
  readonly height: number;
  pixel(x: number, y: number): readonly [number, number, number];
}

export function decodePng(png: Buffer): Image {
  let offset = 8;
  let width = 0, height = 0, channels = 4;
  const idat: Buffer[] = [];
  while (offset < png.length) {
    const length = png.readUInt32BE(offset);
    const type = png.toString("ascii", offset + 4, offset + 8);
    const data = png.subarray(offset + 8, offset + 8 + length);
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      channels = data[9] === 6 ? 4 : 3;
    } else if (type === "IDAT") {
      idat.push(data);
    }
    offset += 12 + length;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const out = Buffer.alloc(stride * height);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)] ?? 0;
    for (let x = 0; x < stride; x++) {
      const value = raw[y * (stride + 1) + 1 + x] ?? 0;
      const left = x >= channels ? out[y * stride + x - channels] ?? 0 : 0;
      const up = y > 0 ? out[(y - 1) * stride + x] ?? 0 : 0;
      const upLeft = x >= channels && y > 0 ? out[(y - 1) * stride + x - channels] ?? 0 : 0;
      let predictor = 0;
      switch (filter) {
        case 1: predictor = left; break;
        case 2: predictor = up; break;
        case 3: predictor = (left + up) >> 1; break;
        case 4: {
          const p = left + up - upLeft;
          const pa = Math.abs(p - left), pb = Math.abs(p - up), pc = Math.abs(p - upLeft);
          predictor = pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft;
          break;
        }
        default: predictor = 0;
      }
      out[y * stride + x] = (value + predictor) & 0xFF;
    }
  }
  return {
    width,
    height,
    pixel(x: number, y: number) {
      const i = y * stride + x * channels;
      return [out[i] ?? 0, out[i + 1] ?? 0, out[i + 2] ?? 0] as const;
    },
  };
}
