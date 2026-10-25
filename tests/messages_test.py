"""Live tests for cases/messages/messages.py against a real Slack workspace.

These call the Slack Web API, so they need SLACK_BOT_TOKEN (or SLACK_USER_TOKEN) in the
environment or in the repo-root .env. Without a token every test is skipped.

SLACK_TEST_CHANNEL limits the tests to one channel name, which keeps them fast. That
channel must contain at least one message and the app must be a member of it.

Run from the repo root:
    cases\\.venv\\Scripts\\python.exe -m unittest tests/messages_test.py -v
"""

import itertools
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "cases" / "messages"))

import messages  # noqa: E402
from slack_sdk import WebClient  # noqa: E402

messages.load_env()
TOKEN = messages.pick_token()
TEST_CHANNEL = os.environ.get("SLACK_TEST_CHANNEL")
SAMPLE_SIZE = 20


@unittest.skipUnless(TOKEN, "SLACK_BOT_TOKEN or SLACK_USER_TOKEN is not set")
class SlackLiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.client = WebClient(token=TOKEN)
        loader = messages.SlackMessageLoader(TOKEN, [TEST_CHANNEL] if TEST_CHANNEL else None)
        cls.documents = list(itertools.islice(loader.lazy_load(), SAMPLE_SIZE))
        cls.entries = [messages.document_to_entry(document) for document in cls.documents]

    def test_token_is_accepted(self):
        response = self.client.auth_test()
        self.assertTrue(response["ok"])

    def test_loader_returns_messages(self):
        where = f"#{TEST_CHANNEL}" if TEST_CHANNEL else "any channel the token can read"
        self.assertGreater(len(self.entries), 0, f"no messages loaded from {where}")

    def test_each_entry_keeps_sender_and_channel(self):
        for entry in self.entries:
            with self.subTest(ts=entry["ts"]):
                self.assertTrue(entry["ts"])
                self.assertIsInstance(entry["text"], str)
                self.assertTrue(entry["sender"]["name"])
                self.assertTrue(entry["channel"]["id"])
                self.assertTrue(entry["channel"]["name"])
                if TEST_CHANNEL:
                    self.assertEqual(entry["channel"]["name"], TEST_CHANNEL)

    def test_entries_match_messages_stored_in_slack(self):
        self.assertGreater(len(self.entries), 0, "no messages loaded")
        for entry in self.entries[:5]:
            with self.subTest(ts=entry["ts"]):
                response = self.client.conversations_history(
                    channel=entry["channel"]["id"],
                    latest=entry["ts"],
                    oldest=entry["ts"],
                    inclusive=True,
                    limit=1,
                )
                found = response.get("messages", [])
                self.assertEqual(len(found), 1, "message is not in Slack")
                stored = found[0]
                self.assertEqual(stored["ts"], entry["ts"])
                self.assertEqual(messages.message_text(stored), entry["text"])
                sender_id = stored.get("user") or stored.get("bot_id") or ""
                self.assertEqual(entry["sender"]["id"], sender_id)

    def test_sender_names_resolve_from_slack_users(self):
        people = [entry for entry in self.entries if entry["sender"]["id"].startswith("U")]
        if not people:
            self.skipTest("no messages from people in the sample")
        entry = people[0]
        response = self.client.users_info(user=entry["sender"]["id"])
        user = response["user"]
        expected = user.get("real_name") or user.get("name")
        self.assertEqual(entry["sender"]["name"], expected)

    def test_script_writes_json_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "slack_messages.json"
            argv = ["messages.py", "--output", str(output)]
            if TEST_CHANNEL:
                argv += ["--channel", TEST_CHANNEL]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(messages.main(), 0)
            entries = json.loads(output.read_text(encoding="utf-8"))
        self.assertIsInstance(entries, list)
        self.assertGreater(len(entries), 0)
        for key in ("text", "sender", "channel", "ts", "thread_ts", "source"):
            self.assertIn(key, entries[0])


if __name__ == "__main__":
    unittest.main()
