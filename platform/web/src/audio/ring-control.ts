/**
 * §14 dynamic rate control, as a pure function so it is unit-testable
 * outside the audio thread: the consumer resamples by a ratio steered
 * from the ring's fill error (PI controller, setpoint = half capacity,
 * bounded ±1%) so host-clock vs virtual-time drift never fills or drains
 * the ring for good.
 */
export const RATE_LIMIT = 0.01;
const KP = 0.02;
const KI = 0.00002;

export interface RateState {
  readonly integral: number;
}

export function nextRatio(state: RateState, fillFrames: number, capacityFrames: number): { ratio: number; state: RateState } {
  const error = (fillFrames - capacityFrames / 2) / capacityFrames; // -0.5 .. 0.5
  const integral = Math.max(-RATE_LIMIT / KI, Math.min(RATE_LIMIT / KI, state.integral + error));
  const adjust = Math.max(-RATE_LIMIT, Math.min(RATE_LIMIT, KP * error + KI * integral));
  return { ratio: 1 + adjust, state: { integral } };
}
