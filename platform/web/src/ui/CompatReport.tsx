/**
 * "Compatibility report": when a game misbehaves, one click collects what
 * is needed to see why (src/compat-report.ts) - copy it or save it as a
 * file to share.
 */
import { Show, createSignal } from "solid-js";
import { formatReport, requestCoreReport } from "../compat-report";
import { getGuestConsole } from "../guest-console";
import { titleNames } from "../saves";

interface CompatReportProps {
  readonly titleId: string;
  readonly fileName: string;
  readonly gpuAdapter: string;
}

function CompatReport(props: CompatReportProps) {
  const [text, setText] = createSignal<string | null>(null);
  const [copied, setCopied] = createSignal(false);

  async function build(): Promise<void> {
    const guest = getGuestConsole();
    const core = await requestCoreReport();
    setText(formatReport(core, {
      gameName: titleNames()[props.titleId] ?? props.fileName,
      runState: guest.runState,
      runDetail: guest.detail,
      fps: guest.fps,
      gpuAdapter: props.gpuAdapter,
      userAgent: navigator.userAgent,
      settings: location.search.replace(/^\?/, ""),
      now: new Date(),
    }));
    setCopied(false);
  }

  function download(): void {
    const body = text();
    if (!body) return;
    const url = URL.createObjectURL(new Blob([body], { type: "text/plain" }));
    const link = document.createElement("a");
    link.href = url;
    link.download = `voland-report-${props.titleId}.txt`;
    link.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 10_000);
  }

  return (
    <div class="voland-report" data-testid="compat">
      <div class="voland-player-bar">
        <button type="button" data-testid="compat-build" onClick={() => void build()}>
          {text() ? "Refresh report" : "Compatibility report"}
        </button>
        <Show when={text()}>
          <button type="button" data-testid="compat-copy"
            onClick={() => void navigator.clipboard?.writeText(text() ?? "").then(() => setCopied(true))}>
            {copied() ? "Copied" : "Copy"}
          </button>
          <button type="button" onClick={download}>Save as file</button>
        </Show>
      </div>
      <Show when={text()}>
        {(body) => <pre class="voland-report-text" data-testid="compat-text">{body()}</pre>}
      </Show>
    </div>
  );
}

export default CompatReport;
