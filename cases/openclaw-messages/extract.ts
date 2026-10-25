import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CASES_DIR = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const MAX_OUTPUT_BYTES = 32 * 1024 * 1024;

export type Sender = { id: string; name: string; display_name: string };
export type Channel = { id: string; name: string };

export type SlackMessage = {
  text: string;
  sender: Sender;
  channel: Channel;
  ts: string;
  thread_ts: string | null;
  source: string | null;
};

export type ExtractRequest = {
  channels?: string[];
  oldest?: number;
  latest?: number;
  limit?: number;
};

export type ExtractConfig = {
  pythonPath?: string;
  scriptPath?: string;
  timeoutMs?: number;
};

export function defaultPython(): string {
  const venv = join(CASES_DIR, ".venv");
  const candidate =
    process.platform === "win32" ? join(venv, "Scripts", "python.exe") : join(venv, "bin", "python");
  if (existsSync(candidate)) return candidate;
  return process.platform === "win32" ? "python" : "python3";
}

export function defaultScript(): string {
  return join(CASES_DIR, "messages", "messages.py");
}

export function buildArgs(script: string, request: ExtractRequest): string[] {
  const args = [script, "--output", "-"];
  for (const channel of request.channels ?? []) {
    const name = channel.trim().replace(/^#/, "");
    if (name) args.push("--channel", name);
  }
  if (request.oldest !== undefined) args.push("--oldest", String(request.oldest));
  if (request.latest !== undefined) args.push("--latest", String(request.latest));
  if (request.limit !== undefined) args.push("--limit", String(request.limit));
  return args;
}

export function extractMessages(
  request: ExtractRequest,
  config: ExtractConfig = {},
  signal?: AbortSignal,
): Promise<SlackMessage[]> {
  const python = config.pythonPath || defaultPython();
  const args = buildArgs(config.scriptPath || defaultScript(), request);
  return new Promise((resolvePromise, reject) => {
    execFile(
      python,
      args,
      {
        cwd: CASES_DIR,
        encoding: "utf8",
        env: { ...process.env, PYTHONIOENCODING: "utf-8" },
        maxBuffer: MAX_OUTPUT_BYTES,
        signal,
        timeout: config.timeoutMs ?? 120_000,
        windowsHide: true,
      },
      (error, stdout, stderr) => {
        if (error) {
          const detail = (stderr || error.message).trim().split("\n").slice(-5).join("\n");
          reject(new Error(`Slack message extraction failed: ${detail}`));
          return;
        }
        try {
          resolvePromise(JSON.parse(stdout) as SlackMessage[]);
        } catch {
          reject(new Error("Slack message extraction returned output that is not JSON"));
        }
      },
    );
  });
}
