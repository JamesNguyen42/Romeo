import { Type } from "typebox";
import { definePluginEntry } from "openclaw/plugin-sdk/plugin-entry";
import { extractMessages, type ExtractConfig } from "./extract.ts";

const Sender = Type.Object({ id: Type.String(), name: Type.String(), display_name: Type.String() });
const Channel = Type.Object({ id: Type.String(), name: Type.String() });
const Message = Type.Object({
  text: Type.String(),
  sender: Sender,
  channel: Channel,
  ts: Type.String(),
  thread_ts: Type.Union([Type.String(), Type.Null()]),
  source: Type.Union([Type.String(), Type.Null()]),
});

export default definePluginEntry({
  id: "romeo-messages",
  name: "Romeo Messages",
  description: "Lets the agent pull Slack messages, with sender and channel, for Romeo cases",
  register(api) {
    const config = (api.pluginConfig ?? {}) as ExtractConfig;

    api.registerTool({
      name: "romeo_get_messages",
      description:
        "Fetch Slack messages the workspace token can read. Each message has its text, sender " +
        "(id, name, display name), channel (id, name), Slack timestamp, thread timestamp and link. " +
        "Messages are newest first within each channel.",
      parameters: Type.Object({
        channels: Type.Optional(
          Type.Array(Type.String(), {
            description: "Channel names without '#'. Omit to read every channel the token can see.",
          }),
        ),
        oldest: Type.Optional(
          Type.Number({ description: "Only messages after this Unix time, in seconds." }),
        ),
        latest: Type.Optional(
          Type.Number({ description: "Only messages before this Unix time, in seconds." }),
        ),
        limit: Type.Optional(
          Type.Integer({
            minimum: 1,
            maximum: 500,
            default: 50,
            description: "Most recent messages to return per channel.",
          }),
        ),
      }),
      outputSchema: Type.Object(
        { count: Type.Integer(), messages: Type.Array(Message) },
        { additionalProperties: false },
      ),
      async execute(_id, params, signal) {
        const messages = await extractMessages({ limit: 50, ...params }, config, signal);
        return {
          content: [{ type: "text", text: JSON.stringify(messages) }],
          details: { count: messages.length, messages },
        };
      },
    });
  },
});
