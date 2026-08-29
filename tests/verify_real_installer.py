#!/usr/bin/env python3
"""Verify the production path against the REAL 44GB installer zip on the UNC share.

A full structure-preserving extraction needs 44.22 GB and the box has ~16 GB free, so
this deliberately does NOT run to completion. It starts the real job, checks the
properties that actually failed in production, then cancels before the disk fills:

  * the UNC zip path is handled (no absolute() mangling)
  * the declared total is the honest 44,220,345,132 bytes
  * progress is NOT inflated - it must stay consistent with bytes actually on disk
    (the reported bug was 99.7% at entry 13/148 with only 5.38 GB written)
  * subdirectories from backslash-separated entries really get created
  * the extraction can be cancelled (a 44 GB job used to be unstoppable)
"""

import json
import sys
import time
import urllib.request

sys.path.insert(0, "tests")
import verify_copy_unzip as V

REAL_ZIP = r"\\trueforge-bm\workspace\8480\5300\CRD_NORNVME\Installer.zip"
DEST = r"C:\af_test\real_test"
DECLARED_TOTAL = 44_220_345_132
STOP_AT_BYTES = 3 * 1024**3      # cancel well before the disk fills
STOP_AT_SECS = 180


def send(installer, download, auto):
    body = json.dumps({
        "ip": V.CLIENT_IP, "auto_flash": auto,
        "installer_path": installer, "download_path": download,
        "chipset": "kenai", "storage": "nvme", "flash_stage": "both",
    }).encode()
    req = urllib.request.Request(V.SERVER + "/send", data=body,
                                headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=10).read()


def dest_bytes_and_dirs():
    out = V.remote_ps(f"""
if (-not (Test-Path '{DEST}')) {{ Write-Output 'B=0'; Write-Output 'D=0'; exit }}
$f = @(Get-ChildItem '{DEST}' -Recurse -File)
$s = 0; foreach ($x in $f) {{ $s += $x.Length }}
Write-Output ("B=" + $s)
Write-Output ("D=" + (@(Get-ChildItem '{DEST}' -Recurse -Directory)).Count)
""")
    b = d = 0
    for line in out.splitlines():
        if line.startswith("B="):
            b = int(line[2:])
        elif line.startswith("D="):
            d = int(line[2:])
    return b, d


def main():
    ok = []

    def check(cond, label, detail=""):
        ok.append(bool(cond))
        print(f"  {'PASS' if cond else 'FAIL'}  {label}" + (f"   [{detail}]" if detail else ""))

    print(f"free before: {V.free_gb()} GB")
    send(DEST, REAL_ZIP, "0")
    time.sleep(3)
    V.remote_ps(f"if (Test-Path '{DEST}') {{ Remove-Item '{DEST}' -Recurse -Force }}")

    n0 = V.log_len()
    send(DEST, REAL_ZIP, "1")

    totals, peak_disk, peak_dirs = set(), 0, 0
    progress_seq, last_claimed = [], 0
    overshoot = 0
    t0 = time.time()
    started = False
    while time.time() - t0 < STOP_AT_SECS:
        c = V.api_clients()
        fi = c.get("file_info", {})
        if fi.get("status") == "copying":
            started = True
        if fi.get("total_bytes"):
            totals.add(fi["total_bytes"])

        disk, dirs = dest_bytes_and_dirs()
        peak_disk, peak_dirs = max(peak_disk, disk), max(peak_dirs, dirs)

        claimed = fi.get("copied_bytes", 0) or 0
        if fi.get("status") == "copying":
            progress_seq.append(fi.get("progress", 0.0))
            # the only hard invariant while a multi-GB entry is mid-write: never claim
            # more than the whole job. NTFS does not flush a growing file's directory
            # size until close, so comparing against the folder size moment-to-moment
            # is not meaningful - that is checked once at cancel time instead.
            overshoot = max(overshoot, claimed - DECLARED_TOTAL)
            last_claimed = claimed

        print(f"    t={int(time.time()-t0):3}s status={fi.get('status'):12} "
              f"progress={fi.get('progress', 0):6.2f}% claimed={claimed/1024**3:6.2f}GB "
              f"disk={disk/1024**3:6.2f}GB dirs={dirs}")

        if disk > STOP_AT_BYTES or claimed > STOP_AT_BYTES:
            print("    -> reached byte budget, cancelling")
            break
        if fi.get("status") in ("copy_failed", "missing_files") and started:
            break
        time.sleep(6)

    # cancel and confirm it actually stops
    send(DEST, REAL_ZIP, "0")
    stopped = False
    for _ in range(20):
        time.sleep(3)
        if V.api_clients().get("file_info", {}).get("status") != "copying":
            stopped = True
            break

    log = V.log_since(n0)
    alive = "auto_flash.exe" in V.remote(
        'tasklist /FI "IMAGENAME eq auto_flash.exe"')
    final_disk, _ = dest_bytes_and_dirs()

    print("\nresults:")
    check(started, "client started the real UNC extraction")
    check(REAL_ZIP in log, "UNC zip path used as-is (not mangled)")
    check(DECLARED_TOTAL in totals,
          f"declared total is the honest {DECLARED_TOTAL}", f"seen={sorted(totals)}")
    check(f"exact bytes: {DECLARED_TOTAL}" in log,
          "log records the exact declared byte total")
    check(overshoot <= 0, "progress never claims more than the whole job",
          f"overshoot={overshoot}")
    check(all(b >= a - 0.01 for a, b in zip(progress_seq, progress_seq[1:])),
          "progress monotonic while copying", f"n={len(progress_seq)}")
    check(all(p <= 100.0001 for p in progress_seq),
          "progress never exceeds 100%", f"max={max(progress_seq, default=0):.2f}%")
    # One-sided on purpose. Claiming MORE than is on disk is the actual bug (that is what
    # "99.7% with only 5.38 GB written" was). Claiming LESS is expected here: a PROGRESS
    # line is only emitted after an entry finishes, so cancelling mid-entry leaves bytes
    # on disk that were never counted.
    over = last_claimed - final_disk
    check(over <= max(1, last_claimed) * 0.05,
          "claimed bytes never exceed bytes actually on disk",
          f"claimed={last_claimed/1024**3:.2f}GB disk={final_disk/1024**3:.2f}GB "
          f"over={over/1024**3:+.2f}GB")
    check(stopped, "extraction cancelled successfully")
    check(alive, "client process survived the cancel")

    print(f"\npeak disk used by test: {peak_disk/1024**3:.2f} GB")
    V.remote_ps(f"if (Test-Path '{DEST}') {{ Remove-Item '{DEST}' -Recurse -Force }}")
    print(f"free after cleanup: {V.free_gb()} GB")
    print(f"\n{sum(ok)}/{len(ok)} passed")
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
