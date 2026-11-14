/**
 * Opens the page's audio output (§14): a 48kHz AudioContext with the
 * ring-draining worklet. Browsers start AudioContexts suspended until a
 * user gesture, so this resumes on the next click/key if needed. Audio is
 * a lifecycle concern only: samples never cross postMessage (§6).
 *
 * A gain stage after the worklet mutes the output while a game boots and
 * has shown no picture yet (setAudioMuted): the worklet keeps draining the
 * ring, so nothing waits, but the stretched, slowed boot sound (Hollow
 * Knight's) is not heard over a black screen.
 */
import type { VolandAudioReport } from "../e2e-hooks";
import processorUrl from "./ring-processor.ts?worker&url";

const SAMPLE_RATE = 48_000;
const STRETCH_SETTLE_MS = 5_000;

let context: AudioContext | null = null;
let gain: GainNode | null = null;
/** Wanted before the output exists (a load mutes before audio starts). */
let mutedWanted = false;
/** Seconds the output takes to fade back in (no click). */
const UNMUTE_FADE_S = 0.05;

/** Mutes or unmutes what the worklet plays (lifecycle: per load, not per frame). */
export function setAudioMuted(muted: boolean): void {
  mutedWanted = muted;
  if (!gain || !context) return;
  gain.gain.cancelScheduledValues(context.currentTime);
  if (muted) gain.gain.setValueAtTime(0, context.currentTime);
  else gain.gain.setTargetAtTime(1, context.currentTime, UNMUTE_FADE_S);
}

export async function startAudioOutput(memory: WebAssembly.Memory, ringBase: number, capacityFrames: number,
                                       onLog: (message: string) => void): Promise<void> {
  if (context) return;
  try {
    context = new AudioContext({ sampleRate: SAMPLE_RATE, latencyHint: "interactive" });
    await context.audioWorklet.addModule(processorUrl);
    const node = new AudioWorkletNode(context, "voland-audio-ring", {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: { memory, ringBase, capacityFrames },
    });
    gain = new GainNode(context, { gain: mutedWanted ? 0 : 1 });
    node.connect(gain);
    gain.connect(context.destination);
    let candidate = false;
    let candidateSince = 0;
    let reported = false;
    node.port.onmessage = (event: MessageEvent<VolandAudioReport>) => {
      window.__VOLAND_AUDIO__ = event.data;
      const now = Date.now();
      /* Underrun counts are not logged: they follow from the game running
       * below full speed, which the CPU worker's status line reports in
       * plain words (workers/log-feed.ts). They stay in __VOLAND_AUDIO__. */
      /* Stretching comes and goes with load spikes; only a change that
       * lasts STRETCH_SETTLE_MS is worth a line. */
      const stretching = event.data.stretching === true;
      if (stretching !== candidate) {
        candidate = stretching;
        candidateSince = now;
      } else if (stretching !== reported && now - candidateSince >= STRETCH_SETTLE_MS) {
        reported = stretching;
        onLog(stretching ? "sound: the game is below full speed, so sound is stretched to keep up (may sound slow or crackle)" : "sound: back to full speed");
      }
    };
    if (context.state === "suspended") {
      const resume = (): void => { void context?.resume(); };
      window.addEventListener("pointerdown", resume, { once: true });
      window.addEventListener("keydown", resume, { once: true });
    }
    onLog(`sound output ready (${context.sampleRate / 1000} kHz)`);
  } catch (e) {
    onLog(`audio output unavailable: ${e instanceof Error ? e.message : String(e)}`);
  }
}
