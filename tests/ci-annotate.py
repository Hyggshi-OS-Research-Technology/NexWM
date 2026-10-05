#!/usr/bin/env python3
"""ci-annotate.py — publish the HDE smoke test results on GitHub (readable through the API, no artifacts needed).

  ci-annotate.py OUTDIR results      PASS/FAIL summary as a single job annotation
  ci-annotate.py OUTDIR checkruns    each screenshot -> one check run "hde-shot NAME" (base64 JPEG
                                     in output.summary + output.text). Needs a GITHUB_TOKEN with checks:write permission.
"""
import base64
import glob
import json
import os
import subprocess
import sys
import urllib.request

FIELD = 65000   # limit of 65535 characters per output field of a check run


def esc(s):
    return s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def results(out):
    path = os.path.join(out, "results.txt")
    text = open(path, encoding="utf-8", errors="replace").read() if os.path.exists(path) else "no results.txt"
    lines = text.splitlines()
    fails = sum(1 for l in lines if l.startswith("FAIL"))
    passes = sum(1 for l in lines if l.startswith("PASS"))
    level = "error" if fails else "notice"
    # annotations are cut at 4096 characters: put the FAIL lines first
    ordered = [l for l in lines if l.startswith("FAIL")] + [l for l in lines if not l.startswith("FAIL")]
    print(f"::{level} title=HDE smoke test: {passes} passed, {fails} failed::{esc(chr(10).join(ordered))}")


def encode(png):
    for width, quality in ((1280, 60), (1280, 45), (1100, 40), (960, 35), (800, 30)):
        jpg = png[:-4] + ".jpg"
        subprocess.run(["convert", png, "-resize", f"{width}x>", "-quality", str(quality), jpg], check=False)
        if not os.path.exists(jpg):
            return None
        data = base64.b64encode(open(jpg, "rb").read()).decode()
        if len(data) <= 2 * FIELD:
            return data
    return None


def checkruns(out):
    token, repo, sha = os.environ.get("GITHUB_TOKEN"), os.environ.get("GITHUB_REPOSITORY"), os.environ.get("GITHUB_SHA")
    if not (token and repo and sha):
        print("GITHUB_TOKEN / GITHUB_REPOSITORY / GITHUB_SHA missing", file=sys.stderr)
        return
    for png in sorted(glob.glob(os.path.join(out, "shot-*.png"))):
        name = os.path.basename(png)[5:-4]
        data = encode(png)
        if not data:
            print(f"skip {name}: too large", file=sys.stderr)
            continue
        body = {
            "name": f"hde-shot {name}",
            "head_sha": sha,
            "status": "completed",
            "conclusion": "neutral",
            "output": {"title": f"HDE screenshot {name} (base64 JPEG)", "summary": data[:FIELD], "text": data[FIELD:] or "-"},
        }
        req = urllib.request.Request(f"https://api.github.com/repos/{repo}/check-runs", data=json.dumps(body).encode(),
                                     method="POST", headers={"Authorization": f"Bearer {token}",
                                                             "Accept": "application/vnd.github+json"})
        try:
            with urllib.request.urlopen(req) as r:
                print(f"{name}: check run {json.load(r)['id']} ({len(data)} chars)")
        except Exception as e:  # noqa: BLE001
            print(f"{name}: failed to create check run: {e}", file=sys.stderr)


if __name__ == "__main__":
    out, mode = sys.argv[1], sys.argv[2]
    if mode == "results":
        results(out)
    elif mode == "checkruns":
        checkruns(out)
    else:
        print(__doc__)
        sys.exit(2)
