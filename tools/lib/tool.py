"""Shared helpers for Romeo tool scripts.

Contract: the service runs `tools/<name> <job_dir>`. The tool reads <job_dir>/input.json and writes
<job_dir>/result.json = {"summary", "facts": [...], "artifacts": [file names in job_dir], "data": {...}}.
"""
import json
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

JOB = Path(sys.argv[1])
INPUT = json.loads((JOB / "input.json").read_text())
PRODUCT = Path(os.environ.get("PRODUCT_DIR", "/opt/hack/product"))
TOOLS = Path(__file__).resolve().parents[1]
ORNITH = os.environ.get("ORNITH_URL", "http://127.0.0.1:8000/v1/chat/completions")


def finish(summary, facts=(), artifacts=(), data=None):
    (JOB / "result.json").write_text(json.dumps({"summary": summary, "facts": list(facts), "artifacts": list(artifacts), "data": data or {}}))
    sys.exit(0)


def sh(*cmd, cwd=None, timeout=600, check=False):
    """Run a command; return (exit code, combined output). Output is bounded to the last 20 kB."""
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    out = (p.stdout + p.stderr)[-20000:]
    if check and p.returncode != 0:
        finish(f"{cmd[0]} failed with exit code {p.returncode}", [f"{' '.join(map(str, cmd[:3]))} failed: {out[-400:]}"], data={"error": True})
    return p.returncode, out


def git(*args, check=True):
    return sh("git", "-C", str(PRODUCT), *args, check=check)[1].strip()


def ornith(system, user, max_tokens=1200, think=False):
    """One Ornith call. Returns (content, reasoning)."""
    body = {"model": "ornith-35b", "temperature": 0, "max_tokens": max_tokens,
            "chat_template_kwargs": {"enable_thinking": think},
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}]}
    req = urllib.request.Request(ORNITH, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        msg = json.loads(r.read())["choices"][0]["message"]
    return msg.get("content") or "", msg.get("reasoning_content") or msg.get("reasoning") or ""


def fenced(text, lang):
    """Extract the first ```lang fenced block (or the whole text if none)."""
    marker = "```" + lang
    if marker in text:
        body = text.split(marker, 1)[1]
        return body.split("```", 1)[0].strip("\n")
    if "```" in text:
        return text.split("```", 2)[1].split("\n", 1)[-1].strip("\n")
    return text.strip()


def ref(rev):
    """The fetched remote branch when it exists (the product clone tracks origin), otherwise the name as given."""
    sh("git", "-C", str(PRODUCT), "fetch", "-q", "origin", timeout=60)
    code, _ = sh("git", "-C", str(PRODUCT), "rev-parse", "--verify", "-q", f"origin/{rev}^{{commit}}")
    return f"origin/{rev}" if code == 0 else rev


def case_rev():
    return INPUT.get("rev") or f"change/{INPUT['case_id']}"


def build(rev):
    code, out = sh(str(TOOLS / "build_rev"), rev, timeout=900)
    if code != 0:
        return None, out
    return out.strip().splitlines()[-1], out
