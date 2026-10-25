"""Pull Slack messages with a LangChain loader and write one JSON object per message.

Each entry keeps the message text plus the sender and the channel it was sent in.
The Slack token is read from SLACK_BOT_TOKEN or SLACK_USER_TOKEN (a repo-root .env
is loaded when python-dotenv is installed). The bot token is used when it can list
channels; otherwise the user token is used. Nothing in this file stores a token.

The token needs channels:read, channels:history, and users:read. Private channels
also need groups:read and groups:history. The app must already be in a channel
before that channel's history can be read.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Iterator

from langchain_core.document_loaders import BaseLoader
from langchain_core.documents import Document
from slack_sdk import WebClient
from slack_sdk.errors import SlackApiError

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = Path(__file__).resolve().parent / "slack_messages.json"

# Slack history mixes real messages with join/leave/topic events. Only messages
# someone sent are written out.
SKIP_SUBTYPES = {
    "channel_archive",
    "channel_canvas_updated",
    "channel_join",
    "channel_leave",
    "channel_name",
    "channel_purpose",
    "channel_topic",
    "channel_unarchive",
    "ekm_access_denied",
    "group_archive",
    "group_join",
    "group_leave",
    "group_name",
    "group_purpose",
    "group_topic",
    "group_unarchive",
    "message_deleted",
    "pinned_item",
    "reminder_add",
    "tombstone",
    "unpinned_item",
}

SKIP_CHANNEL_ERRORS = {"not_in_channel", "channel_not_found", "missing_scope", "is_archived"}


def load_env() -> None:
    env_path = REPO_ROOT / ".env"
    if not env_path.is_file():
        return
    try:
        from dotenv import load_dotenv
    except ImportError:
        return
    load_dotenv(env_path)


def token_scopes(token: str) -> set[str]:
    response = WebClient(token=token).auth_test()
    return set((response.headers.get("x-oauth-scopes") or "").split(","))


def pick_token() -> str | None:
    """Return the first token in .env/environment that can list channels and read history."""
    candidates = [
        os.environ.get(name)
        for name in ("SLACK_BOT_TOKEN", "SLACK_USER_TOKEN")
        if os.environ.get(name)
    ]
    for token in candidates:
        try:
            if {"channels:read", "channels:history"} <= token_scopes(token):
                return token
        except SlackApiError:
            continue
    return candidates[0] if candidates else None


def slack_call(method, **kwargs):
    """Call a Slack Web API method, waiting out rate limits."""
    while True:
        try:
            return method(**kwargs)
        except SlackApiError as exc:
            error = exc.response.get("error")
            if exc.response.status_code != 429 and error != "ratelimited":
                raise
            wait = int(exc.response.headers.get("Retry-After", "1"))
            time.sleep(wait)


def paginate(method, result_key: str, **kwargs):
    cursor = None
    while True:
        call = dict(kwargs, limit=200)
        if cursor:
            call["cursor"] = cursor
        response = slack_call(method, **call)
        yield from response.get(result_key, [])
        cursor = (response.get("response_metadata") or {}).get("next_cursor") or None
        if not cursor:
            return


def load_users(client: WebClient) -> dict[str, dict]:
    users: dict[str, dict] = {}
    try:
        members = paginate(client.users_list, "members")
    except SlackApiError as exc:
        if exc.response.get("error") == "missing_scope":
            print("users:read is missing; sender names will be Slack user ids", file=sys.stderr)
            return users
        raise
    for user in members:
        profile = user.get("profile") or {}
        user_id = user.get("id") or ""
        if not user_id:
            continue
        users[user_id] = {
            "id": user_id,
            "name": user.get("real_name") or user.get("name") or user_id,
            "display_name": profile.get("display_name") or user.get("name") or "",
        }
    return users


def list_channels(client: WebClient, names: set[str] | None):
    types = "public_channel,private_channel"
    try:
        slack_call(client.conversations_list, types=types, exclude_archived=True, limit=1)
    except SlackApiError as exc:
        if exc.response.get("error") != "missing_scope":
            raise
        types = "public_channel"
    for channel in paginate(
        client.conversations_list,
        "channels",
        types=types,
        exclude_archived=True,
    ):
        if names and channel.get("name") not in names:
            continue
        yield channel


def sender_of(message: dict, users: dict[str, dict]) -> dict:
    user_id = message.get("user") or ""
    if user_id and user_id in users:
        return users[user_id]
    if user_id:
        return {"id": user_id, "name": user_id, "display_name": ""}
    bot_profile = message.get("bot_profile") or {}
    bot_id = message.get("bot_id") or ""
    name = message.get("username") or bot_profile.get("name") or bot_id or "unknown"
    return {"id": bot_id, "name": name, "display_name": name}


def message_text(message: dict) -> str:
    text = message.get("text") or ""
    if text:
        return text
    names = [item.get("name") or "" for item in message.get("files") or []]
    return " ".join(name for name in names if name)


def source_link(workspace_url: str | None, channel_id: str, timestamp: str) -> str | None:
    if not workspace_url or not timestamp:
        return None
    return f"{workspace_url}/archives/{channel_id}/p{timestamp.replace('.', '')}"


def message_to_document(
    message: dict,
    channel: dict,
    users: dict[str, dict],
    workspace_url: str | None,
) -> Document | None:
    if message.get("subtype") in SKIP_SUBTYPES:
        return None
    text = message_text(message)
    timestamp = str(message.get("ts") or "")
    channel_id = channel["id"]
    return Document(
        page_content=text,
        metadata={
            "sender": sender_of(message, users),
            "channel": {"id": channel_id, "name": channel.get("name") or channel_id},
            "ts": timestamp,
            "thread_ts": message.get("thread_ts"),
            "source": source_link(workspace_url, channel_id, timestamp),
        },
    )


class SlackMessageLoader(BaseLoader):
    """LangChain loader that reads Slack channel history the token can see."""

    def __init__(
        self,
        token: str,
        channel_names: list[str] | None = None,
        workspace_url: str | None = None,
        oldest: str | None = None,
        latest: str | None = None,
        limit_per_channel: int | None = None,
    ):
        self.client = WebClient(token=token)
        names = {name.lstrip("#") for name in channel_names or [] if name.strip("# ")}
        self.channel_names = names or None
        self.workspace_url = (workspace_url or "").rstrip("/") or None
        self.history_range = {key: value for key, value in (("oldest", oldest), ("latest", latest)) if value}
        self.limit_per_channel = limit_per_channel

    def lazy_load(self) -> Iterator[Document]:
        users = load_users(self.client)
        seen: set[str] = set()
        for channel in list_channels(self.client, self.channel_names):
            channel_name = channel.get("name") or channel["id"]
            seen.add(channel_name)
            try:
                history = paginate(
                    self.client.conversations_history,
                    "messages",
                    channel=channel["id"],
                    **self.history_range,
                )
                count = 0
                for message in history:
                    document = message_to_document(message, channel, users, self.workspace_url)
                    if document is None:
                        continue
                    yield document
                    count += 1
                    if self.limit_per_channel and count >= self.limit_per_channel:
                        break
            except SlackApiError as exc:
                error = exc.response.get("error")
                if error in SKIP_CHANNEL_ERRORS:
                    print(f"skip #{channel_name}: {error}", file=sys.stderr)
                    continue
                raise
        if self.channel_names:
            for name in sorted(self.channel_names - seen):
                print(f"skip #{name}: channel not found", file=sys.stderr)


def document_to_entry(document: Document) -> dict:
    metadata = document.metadata
    return {
        "text": document.page_content,
        "sender": metadata["sender"],
        "channel": metadata["channel"],
        "ts": metadata["ts"],
        "thread_ts": metadata.get("thread_ts"),
        "source": metadata.get("source"),
    }


def main() -> int:
    load_env()
    parser = argparse.ArgumentParser(
        description="Pull Slack messages into a JSON file, one object per message."
    )
    parser.add_argument(
        "--output",
        default=str(DEFAULT_OUTPUT),
        help=f"JSON file to write, or - for standard output (default: {DEFAULT_OUTPUT.name})",
    )
    parser.add_argument(
        "--channel",
        action="append",
        default=[],
        help="Channel name to pull. Repeat for more than one. Default: every channel the token can read.",
    )
    parser.add_argument(
        "--workspace-url",
        default=os.environ.get("SLACK_WORKSPACE_URL"),
        help="Workspace URL, used to build a source link for each message. Defaults to SLACK_WORKSPACE_URL.",
    )
    parser.add_argument("--oldest", help="Only messages after this Unix timestamp in seconds.")
    parser.add_argument("--latest", help="Only messages before this Unix timestamp in seconds.")
    parser.add_argument(
        "--limit",
        type=int,
        help="Most recent messages to keep per channel. Default: all of them.",
    )
    args = parser.parse_args()

    token = pick_token()
    if not token:
        print("Set SLACK_BOT_TOKEN (or SLACK_USER_TOKEN) in the environment or in .env", file=sys.stderr)
        return 1

    loader = SlackMessageLoader(
        token,
        args.channel or None,
        args.workspace_url,
        oldest=args.oldest,
        latest=args.latest,
        limit_per_channel=args.limit,
    )
    entries = [document_to_entry(document) for document in loader.lazy_load()]
    payload = json.dumps(entries, indent=2, ensure_ascii=False) + "\n"
    if args.output == "-":
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stdout.write(payload)
        return 0
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(payload, encoding="utf-8")
    print(f"wrote {len(entries)} messages to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
