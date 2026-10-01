/**
 * Opens the page's audio output (§14): a 48kHz AudioContext with the
 * ring-draining worklet. Browsers start AudioContexts suspended until a
 * user gesture, so this resumes on the next click/key if needed. Audio is
 * a lifecycle concern only: samples never cross postMessage (§6).
 */
import type { VolandAudioReport } from "../e2e-hooks";
import processorUrl from "./ring-processor.ts?worker&url";

const SAMPLE_RATE = 48_000;

let context: AudioContext | null = null;

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
    node.connect(context.destination);
    let reportedUnderruns = 0;
    node.port.onmessage = (event: MessageEvent<VolandAudioReport>) => {
      window.__VOLAND_AUDIO__ = event.data;
      /* Underruns are normal while nothing plays; log only bursts during playback. */
      if (event.data.fill > 0 && event.data.underruns > reportedUnderruns) {
        onLog(`audio underruns so far: ${event.data.underruns}`);
      }
      reportedUnderruns = event.data.underruns;
    };
    if (context.state === "suspended") {
      const resume = (): void => { void context?.resume(); };
      window.addEventListener("pointerdown", resume, { once: true });
      window.addEventListener("keydown", resume, { once: true });
    }
    onLog(`audio output: ${context.sampleRate}Hz, worklet draining the ring`);
  } catch (e) {
    onLog(`audio output unavailable: ${e instanceof Error ? e.message : String(e)}`);
  }
}
