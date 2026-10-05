#!/usr/bin/env python3
"""ci-annotate.py — đưa kết quả kiểm thử khói (và ảnh chụp màn hình, dạng base64) vào annotation của
GitHub Actions, để xem được qua API check-runs ngay cả khi không tải được artifact.

  ci-annotate.py OUTDIR results          tóm tắt PASS/FAIL (1 annotation)
  ci-annotate.py OUTDIR shots BATCH      các mảnh ảnh của lô BATCH (tối đa 9 annotation/bước)
Ảnh: JPEG chất lượng thấp, chia thành mảnh "shot NAME i/n" (ghép lại theo thứ tự i).
"""
import base64
import glob
import os
import subprocess
import sys

CHUNK = 48000
PER_STEP = 9


def esc(s):
    return s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def results(out):
    path = os.path.join(out, "results.txt")
    text = open(path, encoding="utf-8", errors="replace").read() if os.path.exists(path) else "no results.txt"
    fails = sum(1 for l in text.splitlines() if l.startswith("FAIL"))
    passes = sum(1 for l in text.splitlines() if l.startswith("PASS"))
    level = "error" if fails else "notice"
    print(f"::{level} title=HDE smoke test: {passes} passed, {fails} failed::{esc(text)}")


def chunks(out):
    items = []
    for png in sorted(glob.glob(os.path.join(out, "shot-*.png"))):
        name = os.path.basename(png)[5:-4]
        jpg = png[:-4] + ".jpg"
        subprocess.run(["convert", png, "-resize", "1024x", "-quality", "45", jpg], check=False)
        if not os.path.exists(jpg):
            continue
        data = base64.b64encode(open(jpg, "rb").read()).decode()
        parts = [data[i:i + CHUNK] for i in range(0, len(data), CHUNK)]
        for i, p in enumerate(parts):
            items.append((f"shot {name} {i + 1}/{len(parts)}", p))
    return items


if __name__ == "__main__":
    out, mode = sys.argv[1], sys.argv[2]
    if mode == "results":
        results(out)
    else:
        batch = int(sys.argv[3])
        items = chunks(out)
        for title, data in items[batch * PER_STEP:(batch + 1) * PER_STEP]:
            print(f"::notice title={title}::{data}")
        print(f"{len(items)} screenshot chunks in total", file=sys.stderr)
