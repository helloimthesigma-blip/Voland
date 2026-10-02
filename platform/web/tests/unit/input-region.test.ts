/**
 * Web input writer + mappings (§18). Byte layout is checked against
 * tests/data/input_region_vectors.txt - the same file the C reader/writer
 * test consumes - so the two writers cannot drift.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

import {
  DISCONNECTED,
  INPUT_REGION_SLOT_BYTES,
  INPUT_REGION_SLOT_COUNT,
  INPUT_TOUCH_MAX,
  InputButton,
  InputDeviceKind,
  createInputRegionWriter,
} from "../../src/input/input-region.ts";
import { type GamepadLike, mapStandardGamepad, toStickUnits } from "../../src/input/gamepad-mapping.ts";
import { KEYBOARD_PROFILE_ID, mapKeyboard } from "../../src/input/keyboard-mapping.ts";
import { collectSlotStates, mapPointerToTouch } from "../../src/input/input-loop.ts";

const VECTORS = fileURLToPath(new URL("../../../../tests/data/input_region_vectors.txt", import.meta.url));
const REGION_BASE = 256;

function sharedBuffer(): ArrayBufferLike {
  return new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true }).buffer;
}

function hex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

test("writer bytes match the shared C/TS vectors", () => {
  const lines = readFileSync(VECTORS, "utf8").split("\n").filter((l) => l && !l.startsWith("#"));
  assert.ok(lines.length >= 5);
  for (const line of lines) {
    const fields = line.trim().split(/\s+/);
    const buttons = Number.parseInt(fields[0] ?? "", 16);
    const axes = fields.slice(1, 9).map(Number);
    const flags = Number.parseInt(fields[9] ?? "", 16);
    const expected = fields[10] ?? "";

    const buffer = sharedBuffer();
    const writer = createInputRegionWriter(buffer, REGION_BASE);
    writer.writeSlot(3, {
      buttons,
      axes,
      connected: (flags & 1) !== 0,
      deviceKind: (flags >>> 8) & 0xff,
      profileId: (flags >>> 16) & 0xffff,
    });
    const slot = new Uint8Array(buffer, REGION_BASE + 3 * INPUT_REGION_SLOT_BYTES, INPUT_REGION_SLOT_BYTES);
    assert.equal(hex(slot), expected, line);
  }
});

test("each write advances the sequence by two and leaves it even", () => {
  const buffer = sharedBuffer();
  const writer = createInputRegionWriter(buffer, REGION_BASE);
  const sequence = new Uint32Array(buffer, REGION_BASE + 2 * INPUT_REGION_SLOT_BYTES, 1);
  writer.writeSlot(2, DISCONNECTED);
  writer.writeSlot(2, DISCONNECTED);
  assert.equal(sequence[0], 4);
  assert.throws(() => writer.writeSlot(8, DISCONNECTED), RangeError);
});

function pad(pressed: readonly number[], axes: readonly number[] = [0, 0, 0, 0]): GamepadLike {
  return {
    connected: true,
    mapping: "standard",
    buttons: Array.from({ length: 17 }, (_, i) => ({ pressed: pressed.includes(i) })),
    axes,
  };
}

test("standard gamepad: Switch labels are swapped against Xbox positions", () => {
  assert.equal(mapStandardGamepad(pad([0])).buttons, InputButton.B); // bottom face -> B
  assert.equal(mapStandardGamepad(pad([1])).buttons, InputButton.A); // right face -> A
  assert.equal(mapStandardGamepad(pad([2])).buttons, InputButton.Y); // left face -> Y
  assert.equal(mapStandardGamepad(pad([3])).buttons, InputButton.X); // top face -> X
  assert.equal(mapStandardGamepad(pad([8, 9])).buttons, InputButton.Minus | InputButton.Plus);
  assert.equal(mapStandardGamepad(pad([12, 15])).buttons, InputButton.DpadUp | InputButton.DpadRight);
  assert.equal(mapStandardGamepad(pad([16])).buttons, InputButton.Home);
  const state = mapStandardGamepad(pad([]));
  assert.equal(state.deviceKind, InputDeviceKind.StandardGamepad);
  assert.ok(state.connected);
});

test("stick Y axes invert (Gamepad +Y is down, Horizon +Y is up) and clamp", () => {
  const state = mapStandardGamepad(pad([], [1, 1, -1, -0.5]));
  assert.deepEqual(state.axes.slice(0, 4), [32767, -32767, -32767, 16384]);
  assert.equal(toStickUnits(5, false), 32767);
  assert.equal(toStickUnits(Number.NaN, false), 0);
});

test("keyboard profile: buttons and digital sticks", () => {
  const state = mapKeyboard(new Set(["KeyZ", "ArrowLeft", "KeyW", "KeyD", "KeyJ"]));
  assert.equal(state.buttons, InputButton.A | InputButton.DpadLeft);
  assert.deepEqual(state.axes.slice(0, 4), [32767, 32767, -32767, 0]);
  assert.equal(state.profileId, KEYBOARD_PROFILE_ID);
  assert.equal(state.deviceKind, InputDeviceKind.Keyboard);
  // Opposite directions cancel.
  assert.equal(mapKeyboard(new Set(["KeyA", "KeyD"])).axes[0], 0);
});

test("slot policy: gamepads by index, keyboard on slot 0 only when free", () => {
  const keys = new Set(["KeyZ"]);
  const noPads = collectSlotStates([], true, keys);
  assert.equal(noPads[0]?.deviceKind, InputDeviceKind.Keyboard);
  assert.equal(noPads[1]?.connected, false);

  const withPad = collectSlotStates([pad([1])], true, keys);
  assert.equal(withPad[0]?.deviceKind, InputDeviceKind.StandardGamepad);

  const inactive = collectSlotStates([null, pad([])], false, keys);
  assert.equal(inactive[0]?.connected, false);
  assert.equal(inactive[1]?.connected, true);
  assert.equal(inactive.length, 8);
});

test("touch block: seqlock, count, packed points, clamped", () => {
  const buffer = sharedBuffer();
  const writer = createInputRegionWriter(buffer, REGION_BASE);
  const touchBase = REGION_BASE + INPUT_REGION_SLOT_COUNT * INPUT_REGION_SLOT_BYTES;
  const view = new DataView(buffer);
  writer.writeTouch([{ x: 100, y: 200 }, { x: 5000, y: -3 }, { x: 1, y: 1 }]);
  assert.equal(view.getUint32(touchBase, true), 2); // two increments: published
  assert.equal(view.getUint32(touchBase + 4, true), INPUT_TOUCH_MAX);
  assert.equal(view.getUint32(touchBase + 8, true), 100 | (200 << 16));
  assert.equal(view.getUint16(touchBase + 12, true), 1279);
  assert.equal(view.getUint16(touchBase + 14, true), 0);
  writer.writeTouch([]);
  assert.equal(view.getUint32(touchBase, true), 4);
  assert.equal(view.getUint32(touchBase + 4, true), 0);
  assert.equal(view.getUint32(touchBase + 8, true), 0);
  // The slots are not touched by the touch writer.
  assert.equal(view.getUint32(REGION_BASE, true), 0);
});

test("pointer mapping honours the 16:9 letterbox", () => {
  const exact = { left: 10, top: 20, width: 640, height: 360 };
  assert.deepEqual(mapPointerToTouch(10, 20, exact), { x: 0, y: 0 });
  assert.deepEqual(mapPointerToTouch(330, 200, exact), { x: 640, y: 360 });
  assert.equal(mapPointerToTouch(650, 200, exact), null);
  // A 4:3 box: bars top and bottom.
  const tall = { left: 0, top: 0, width: 1280, height: 960 };
  assert.equal(mapPointerToTouch(640, 100, tall), null);
  assert.deepEqual(mapPointerToTouch(640, 120, tall), { x: 640, y: 0 });
  assert.equal(mapPointerToTouch(0, 0, { left: 0, top: 0, width: 0, height: 0 }), null);
});
