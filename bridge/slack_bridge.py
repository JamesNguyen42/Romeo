"""Slack <-> Romeo bridge (Socket Mode). Runs on the GB10 next to the case service.

In:  @mentions and DMs -> POST /api/route (Clef picks the intent) -> open a case / add a message / reply with status.
Out: the case event stream -> one Slack thread per case: decisions, tool steps, scripts, PR links, conclusion, done.

Tokens are read from ~/.config/romeo/slack.env (SLACK_BOT_TOKEN, SLACK_APP_TOKEN) and never logged.
Thread mapping is kept in BRIDGE_STATE so a restart continues the same threads.
"""
import json
import os
import re
import threading
import time
import urllib.request
from pathlib import Path

from slack_sdk import WebClient
from slack_sdk.socket_mode import SocketModeClient
from slack_sdk.socket_mode.request import SocketModeRequest
from slack_sdk.socket_mode.response import SocketModeResponse

API = os.environ.get("ROMEO_API", "http://127.0.0.1:8790")
REPORT_CHANNEL = os.environ.get("SLACK_REPORT_CHANNEL", "")  # where demo cases (not opened from Slack) report
STATE = Path(os.environ.get("BRIDGE_STATE", "/opt/hack/bridge-state.json"))
DATA = Path(os.environ.get("ROMEO_DATA", "/opt/hack/data"))
PR_REPO = os.environ.get("PRODUCT_REPO", "Cosmin-B/romeo-renderer")


def load_env(path):
    for line in Path(path).expanduser().read_text().splitlines():
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())


load_env(os.environ.get("SLACK_ENV", "~/.config/romeo/slack.env"))
web = WebClient(token=os.environ["SLACK_BOT_TOKEN"])
BOT_USER = web.auth_test()["user_id"]
lock = threading.Lock()
state = json.loads(STATE.read_text()) if STATE.exists() else {"threads": {}}
chat_cases = set()  # cases opened from Slack: their thread is the reporter's message, saved by on_message


def save():
    STATE.write_text(json.dumps(state))


def api(method, path, body=None, timeout=60):
    req = urllib.request.Request(API + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read() or b"{}")


def post(case_id, text, broadcast=False):
    """Post into the case's thread, creating the thread on first use."""
    for _ in range(20):  # a case opened from Slack gets its thread from on_message; wait for that instead of starting a new one
        if case_id not in chat_cases or case_id in state["threads"]:
            break
        time.sleep(0.25)
    with lock:
        t = state["threads"].get(case_id)
        if not t:
            if not REPORT_CHANNEL or case_id in chat_cases:
                return
            first = web.chat_postMessage(channel=REPORT_CHANNEL, text=text)
            state["threads"][case_id] = {"channel": REPORT_CHANNEL, "ts": first["ts"]}
            save()
            return
    web.chat_postMessage(channel=t["channel"], thread_ts=t["ts"], text=text, reply_broadcast=broadcast)


def upload(case_id, artifact, title):
    t = state["threads"].get(case_id)
    path = DATA / "jobs" / artifact
    if not t or not path.is_file():
        return
    try:
        web.files_upload_v2(channel=t["channel"], thread_ts=t["ts"], file=str(path), title=title)
    except Exception as e:  # missing files:write scope must not stop the bridge
        print("upload failed:", type(e).__name__, flush=True)


# ---------------------------------------------------------------- Slack -> case service

SETUP = re.compile(r"\bsetup\s+([A-H])\b", re.I)


def on_message(event):
    text = re.sub(r"<@[A-Z0-9]+>", "", event.get("text", "")).strip()
    channel, user, ts = event["channel"], event.get("user", ""), event.get("thread_ts") or event["ts"]
    if not text:
        return
    known = next((cid for cid, t in state["threads"].items() if t["ts"] == ts), None)
    try:
        route = api("POST", "/api/route", {"text": text, "channel": channel, "user": user, **({"case_id": known} if known else {})}, timeout=90)
    except Exception as e:
        web.chat_postMessage(channel=channel, thread_ts=ts, text=f"Romeo could not reach the case service ({type(e).__name__}).")
        return
    intent, action, p = route.get("intent"), route.get("action"), route.get("p", {})
    conf = p.get(intent, 0) if isinstance(p, dict) else 0
    if action == "open_case":
        m = SETUP.search(text)
        case = api("POST", "/api/cases", {"title": text.split("\n")[0][:80], "setup": m.group(1).upper() if m else "A", "report": text,
                                          "channel": channel, "user": user})
        cid = case["id"]
        with lock:
            state["threads"][cid] = {"channel": channel, "ts": ts}
            save()
        web.chat_postMessage(channel=channel, thread_ts=ts, text=(
            f":inbox_tray: Recorded as *{cid}* — Clef read this as a *new report* (p={conf:.2f}). "
            f"Setup {case.get('setup', 'A')}. The agent starts now; I'll post each step in this thread."))
    elif action == "append_message":
        cid = route.get("case_id") or known
        api("POST", f"/api/cases/{cid}/messages", {"text": text, "channel": channel, "user": user})
        web.chat_postMessage(channel=channel, thread_ts=ts, text=f":memo: Added to *{cid}* (Clef: question about this case, p={conf:.2f}). The agent will read it on its next step.")
    elif action == "summarize_status":
        cases = api("GET", "/api/cases")
        lines = [f"• *{c['id']}* {c['title']} — {c['status']}" + (f" ({c['waiting_for']})" if c.get("waiting_for") else "") for c in cases]
        web.chat_postMessage(channel=channel, thread_ts=ts, text=":bar_chart: Current cases:\n" + "\n".join(lines[-15:]))
    else:
        web.chat_postMessage(channel=channel, thread_ts=ts, text=f"Clef read this as *{intent}* (p={conf:.2f}); no case action taken.")


def handle(client: SocketModeClient, req: SocketModeRequest):
    client.send_socket_mode_response(SocketModeResponse(envelope_id=req.envelope_id))
    if req.type != "events_api":
        return
    ev = req.payload.get("event", {})
    if ev.get("bot_id") or ev.get("subtype") or ev.get("user") == BOT_USER:
        return
    direct = ev.get("channel_type") == "im"
    if ev.get("type") == "app_mention" or (ev.get("type") == "message" and direct):
        threading.Thread(target=on_message, args=(ev,), daemon=True).start()


# ---------------------------------------------------------------- case service -> Slack

TOOL_WORDS = {"write_test": ":test_tube: *Wrote a regression check*", "write_fix": ":hammer_and_wrench: *Wrote and compiled a fix*",
              "open_pr": ":rocket: *Opened a pull request*", "git_diff": ":mag: *Read the change under investigation*", "plot": ":chart_with_upwards_trend: *Drew a chart*"}


def on_event(e):
    cid, typ, d = e.get("case"), e.get("type"), e.get("data", {})
    if typ == "case_opened" and d.get("channel"):
        chat_cases.add(cid)
    elif typ == "case_opened" and cid not in state["threads"]:
        post(cid, f":inbox_tray: *{cid}* — {d.get('title', '')}\n>{d.get('report', '')[:400]}")
    elif typ == "decision":
        top = ", ".join(f"{c.get('label', c['action'])} {c['p']:.2f}" for c in d.get("candidates", [])[:3])
        post(cid, f":compass: Clef chose *{d.get('chosen')}* ({top}) in {d.get('inference_ms', 0):.0f} ms")
    elif typ == "action_requested" and d.get("job"):
        post(cid, f":gear: Running *{d.get('label', d.get('kind'))}*")
    elif typ == "action_result":
        tool = d.get("tool") or ""
        head = TOOL_WORDS.get(tool, ":white_check_mark: Result")
        msg = f"{head}: {d.get('summary', '')[:600]}"
        data = d.get("data") or {}
        if data.get("pr_url"):
            msg += f"\n:link: {data['pr_url']}"
        if data.get("fix_rev"):
            msg += f"\n:link: https://github.com/{PR_REPO}/tree/{data['fix_rev']}"
        post(cid, msg)
        for a in d.get("artifacts", []):
            if a.endswith("check.sh") or a.endswith(".patch"):
                path = DATA / "jobs" / a
                if path.is_file():
                    lang = "sh" if a.endswith(".sh") else "diff"
                    post(cid, f"`{a.split('/')[-1]}`\n```{path.read_text()[:2500]}```")
            elif a.endswith(("chart.svg", "diff.png")) or (a.endswith("image.png") and tool == ""):
                pass
    elif typ == "thought" and d.get("text"):
        post(cid, f":thought_balloon: {d['text'][:700]}")
    elif typ == "conclusion":
        post(cid, f":bulb: *Conclusion*: {d.get('diagnosis', '')}\n*Proposed fix*: {d.get('fix', '')}", broadcast=True)
    elif typ == "done":
        checks = "\n".join(f"• {c}" for c in d.get("checks", []))
        post(cid, f":white_check_mark: *{cid} done* — {d.get('summary', '')}\n{checks}\n_Verified with: {d.get('verified_with', '')}_", broadcast=True)
    elif typ == "stopped":
        post(cid, f":octagonal_sign: *{cid} stopped* — {d.get('reason', '')}\nMissing: {d.get('missing', '')}", broadcast=True)
    elif typ == "restarted":
        post(cid, ":arrows_counterclockwise: The case service restarted; this case resumed from its log. No step runs twice.")


def follow_stream():
    while True:
        try:
            with urllib.request.urlopen(API + "/api/stream", timeout=3600) as r:
                for raw in r:
                    line = raw.decode().strip()
                    if line.startswith("data: "):
                        try:
                            on_event(json.loads(line[6:]))
                        except Exception as e:
                            print("event post failed:", type(e).__name__, str(e)[:200], flush=True)
        except Exception as e:
            print("stream reconnect:", type(e).__name__, flush=True)
            time.sleep(2)


if __name__ == "__main__":
    threading.Thread(target=follow_stream, daemon=True).start()
    sm = SocketModeClient(app_token=os.environ["SLACK_APP_TOKEN"], web_client=web)
    sm.socket_mode_request_listeners.append(handle)
    sm.connect()
    print("bridge connected as", BOT_USER, flush=True)
    while True:
        time.sleep(3600)
