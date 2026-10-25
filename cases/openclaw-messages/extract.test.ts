// Live test: needs Slack tokens in the repo-root .env. Run with: node --test cases/openclaw-messages/extract.test.ts
import assert from "node:assert/strict";
import { test } from "node:test";
import { buildArgs, extractMessages } from "./extract.ts";

test("buildArgs passes filters to messages.py", () => {
  const args = buildArgs("messages.py", { channels: ["#general", " ", "alerts"], oldest: 10, limit: 5 });
  assert.deepEqual(args, [
    "messages.py", "--output", "-", "--channel", "general", "--channel", "alerts",
    "--oldest", "10", "--limit", "5",
  ]);
});

test("extractMessages returns real Slack messages with sender and channel", async () => {
  const messages = await extractMessages({ limit: 5 });
  assert.ok(messages.length > 0, "no messages returned");
  for (const message of messages) {
    assert.equal(typeof message.text, "string");
    assert.ok(message.ts);
    assert.ok(message.sender.id && message.sender.name);
    assert.ok(message.channel.id && message.channel.name);
  }
});

test("extractMessages honours channel and per-channel limit", async () => {
  const [first] = await extractMessages({ limit: 1 });
  assert.ok(first, "no messages returned");
  const messages = await extractMessages({ channels: [first.channel.name], limit: 2 });
  assert.ok(messages.length > 0 && messages.length <= 2);
  assert.ok(messages.every((message) => message.channel.name === first.channel.name));
});

test("extractMessages reports a failing script as an error", async () => {
  await assert.rejects(
    extractMessages({}, { scriptPath: "does-not-exist.py", timeoutMs: 20_000 }),
    /Slack message extraction failed/,
  );
});
