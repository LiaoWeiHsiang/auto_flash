#!/usr/bin/env python3
"""Watch a real UNC folder copy end-to-end and detect any restart-from-zero.

Deliberately does NOT touch auto_flash after arming: toggling it sets copy_cancel, which
kills an in-flight copy and makes it start over -- that is what corrupted the earlier
observation, so the observer must stay read-only while the copy runs.

Reports: every restart (a new "Starting folder copy" line), every backwards jump in
copied_bytes, and the final outcome.
"""

import sys
import time

sys.path.insert(0, "tests")
import verify_copy_unzip as V

SRC = r"\\hamoatw\dropbox\DavidLiao\4900\Installer"
DEST = r"C:\af_test\destttt"
POLL = 5
MAX_SECS = 3600


def dest_bytes():
    out = V.remote_ps(f"""
if (-not (Test-Path '{DEST}')) {{ Write-Output 'B=0;F=0'; exit }}
$f = @(Get-ChildItem '{DEST}' -Recurse -File)
$s = 0; foreach ($x in $f) {{ $s += $x.Length }}
Write-Output ('B=' + $s + ';F=' + $f.Count)
""")
    b = f = 0
    for line in out.splitlines():
        if line.startswith("B="):
            try:
                parts = line.strip().split(";")
                b = int(parts[0][2:])
                f = int(parts[1][2:])
            except Exception:
                pass
    return b, f


def main():
    print(f"free before: {V.free_gb()} GB")
    n0 = V.log_len()

    V.api_send(DEST, SRC, auto="1")
    print(f"armed: {DEST}  <-  {SRC}\n")

    starts = 0
    peak_claimed = 0
    regressions = []
    samples = 0
    t0 = time.time()
    done = False

    while time.time() - t0 < MAX_SECS:
        fi = V.api_clients().get("file_info", {})
        st = fi.get("status")
        claimed = fi.get("copied_bytes", 0) or 0
        total = fi.get("total_bytes", 0) or 0
        prog = fi.get("progress", 0.0) or 0.0

        log = V.log_since(n0)
        new_starts = log.count("[COPY] Starting folder copy:")
        if new_starts > starts:
            print(f"  *** RESTART #{new_starts} detected at t={int(time.time()-t0)}s "
                  f"(previous peak was {peak_claimed/1024**3:.2f} GB) ***")
            starts = new_starts
            peak_claimed = 0

        if claimed + 1_000_000 < peak_claimed and claimed > 0:
            regressions.append((peak_claimed, claimed))
            print(f"  *** copied_bytes WENT BACKWARDS: "
                  f"{peak_claimed/1024**3:.2f} GB -> {claimed/1024**3:.2f} GB ***")
        peak_claimed = max(peak_claimed, claimed)

        samples += 1
        if samples % 4 == 1 or st not in V.BUSY_STATES:
            disk, nfiles = dest_bytes()
            print(f"  t={int(time.time()-t0):4}s {st:14} {prog:6.2f}%  "
                  f"claimed={claimed/1024**3:6.2f}GB / {total/1024**3:6.2f}GB  "
                  f"disk={disk/1024**3:6.2f}GB files={nfiles}  starts={starts}")

        if st not in V.BUSY_STATES and samples > 3:
            # give it a moment in case another attempt is about to begin
            time.sleep(8)
            fi2 = V.api_clients().get("file_info", {})
            if fi2.get("status") not in V.BUSY_STATES:
                log = V.log_since(n0)
                if log.count("[COPY] Starting folder copy:") == starts:
                    done = True
                    break
        time.sleep(POLL)

    log = V.log_since(n0)
    disk, nfiles = dest_bytes()
    fi = V.api_clients().get("file_info", {})

    print("\n================ RESULT ================")
    print(f"finished cleanly (no further attempts): {done}")
    print(f"full-copy starts (restarts from zero) : {log.count('[COPY] Starting folder copy:')}")
    print(f"delta copies                          : {log.count('[COPY] Delta copy:')}")
    print(f"cancellations                         : {log.count('Copy cancelled')}")
    print(f"copy errors                           : {log.count('[COPY ERROR]')}")
    print(f"copied_bytes regressions              : {len(regressions)}")
    print(f"final status                          : {fi.get('status')}")
    print(f"final error_msg                       : {fi.get('error_msg')!r}")
    print(f"final warning_msg                     : {(fi.get('warning_msg') or '')[:160]}")
    print(f"destination                           : {nfiles} files, {disk/1024**3:.2f} GB")
    print(f"free after                            : {V.free_gb()} GB")

    print("\n---- copy-related log lines ----")
    for line in log.splitlines():
        if any(k in line for k in ("[COPY]", "[COPY ERROR]", "MSG_SET_AUTO_FLASH",
                                   "FILE_INFO")):
            print("  " + line[:150])
    return 0


if __name__ == "__main__":
    sys.exit(main())
