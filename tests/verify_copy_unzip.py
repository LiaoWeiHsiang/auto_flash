#!/usr/bin/env python3
"""End-to-end verification harness for auto_flash installer copy / unzip.

Drives the real Windows client on a remote box through the host_server HTTP API and
asserts on both the observable state (server /clients JSON) and ground truth
(actual files + sizes on the remote disk, and the client's own log).

What it pins down, per the reported bugs:
  * progress/total_bytes must equal the bytes actually written (no inflation from
    duplicate basenames, no compressed-vs-uncompressed mixup)
  * progress must never exceed 100% and never move backwards
  * a run must not silently restart from zero (bounded extraction/copy count)
  * a permanently unsatisfiable source must terminate, not loop forever
  * the destination must be a VALID installer: subfolder structure intact and the
    root manifest not clobbered by a subfolder variant

Every test deletes the destination before and after itself, so disk use stays bounded.

Usage: python3 tests/verify_copy_unzip.py [--only T1,T2] [--rounds 2]
"""

import argparse
import base64
import json
import re
import subprocess
import sys
import time
import urllib.request

REMOTE = "10.235.50.110:9200"
RCMD = "./remote_cmd/build-linux/remote_cmd_host"
SERVER = "http://localhost:8080"
CLIENT_IP = "10.235.50.110"

BASE = r"C:\af_test"
SRC_FOLDER = BASE + r"\installer_src"
SRC_ZIP = BASE + r"\installer_src.zip"
SRC_ZIP_MISSING = BASE + r"\installer_missing.zip"
DEST = BASE + r"\dest"
LOG = BASE + r"\run_verify.log"

# ground truth from utils/zip/make_test_installer.py
FULL_FILES = 24
FULL_BYTES = 26_827_627
FULL_SUBDIRS = 6
SZ = {
    r"tools.fv": 491_520,
    r"gpt_main0.bin": 24_576,
    r"nor_oob\gpt_main0.bin": 28_672,
    r"nor_oob\zeros_1sector.bin": 8_192,
    r"EMMCDL.exe": 233_760,
    r"MainOS.bin": 25_165_824,
}

NOISE = ("hamoatw", "CMD.EXE was started", "UNC paths are not supported",
         "[remote_cmd]", "#< CLIXML", "<Objs Version")


def _clean(out: str) -> str:
    return "\n".join(l for l in out.splitlines()
                     if l.strip() and not any(n in l for n in NOISE))


def remote(cmd: str) -> str:
    # the client log can contain bytes that are not valid UTF-8 (console code page),
    # so decode leniently rather than blowing up the whole test run
    r = subprocess.run([RCMD, REMOTE, cmd], capture_output=True, timeout=900)
    out = (r.stdout or b"").decode("utf-8", "replace") + \
          (r.stderr or b"").decode("utf-8", "replace")
    return _clean(out)


def remote_ps(script: str) -> str:
    full = ("$ProgressPreference='SilentlyContinue'\n"
            "$ErrorActionPreference='Stop'\n" + script)
    b64 = base64.b64encode(full.encode("utf-16-le")).decode()
    return remote(f"powershell -NoProfile -EncodedCommand {b64}")


def api_clients() -> dict:
    with urllib.request.urlopen(SERVER + "/clients", timeout=10) as r:
        for c in json.load(r):
            if c.get("ip") == CLIENT_IP:
                return c
    return {}


def api_send(installer: str, download: str, auto: str = "1") -> None:
    body = json.dumps({
        "ip": CLIENT_IP, "auto_flash": auto,
        "installer_path": installer, "download_path": download,
        "chipset": "kenai", "storage": "nvme", "flash_stage": "both",
    }).encode()
    req = urllib.request.Request(SERVER + "/send", data=body,
                                headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=10).read()


def log_len() -> int:
    out = remote_ps(f"if (Test-Path '{LOG}') {{ (Get-Content '{LOG}').Count }} else {{ 0 }}")
    for tok in out.split():
        if tok.strip().isdigit():
            return int(tok)
    return 0


def log_since(n: int) -> str:
    return remote_ps(
        f"if (Test-Path '{LOG}') {{ Get-Content '{LOG}' | Select-Object -Skip {n} }}")


def dest_state() -> dict:
    out = remote_ps(f"""
if (-not (Test-Path '{DEST}')) {{ Write-Output 'ABSENT'; exit }}
$f = @(Get-ChildItem '{DEST}' -Recurse -File)
$d = @(Get-ChildItem '{DEST}' -Recurse -Directory)
$sum = 0; foreach ($x in $f) {{ $sum += $x.Length }}
Write-Output ("FILES=" + $f.Count)
Write-Output ("BYTES=" + $sum)
Write-Output ("DIRS=" + $d.Count)
$rp = Join-Path '{DEST}' 'rawprogram0.xml'
if (Test-Path $rp) {{
  $c = Get-Content $rp -Raw
  # 'bootfw1_64.bin' appears only in the nor_oob variant manifest, never in the root
  # one, so it is a reliable marker that a subfolder variant clobbered the root file.
  Write-Output ("ROOT_XML_IS_VARIANT=" + ($c -match 'bootfw1_64'))
}} else {{ Write-Output 'ROOT_XML_IS_VARIANT=MISSING' }}
""")
    st = {"raw": out}
    if "ABSENT" in out:
        st["absent"] = True
        return st
    for line in out.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            st[k.strip()] = v.strip()
    return st


def file_size(rel: str):
    out = remote_ps(f"""
$p = Join-Path '{DEST}' '{rel}'
if (Test-Path $p) {{ (Get-Item $p).Length }} else {{ 'MISSING' }}""")
    t = out.strip().split()
    if not t:
        return None
    return int(t[-1]) if t[-1].isdigit() else t[-1]


def clean_dest() -> None:
    remote_ps(f"if (Test-Path '{DEST}') {{ Remove-Item '{DEST}' -Recurse -Force }}")


def quiesce(park_src=None, timeout=600):
    """Stop the client from acting, and wait for any in-flight copy to finish.

    `park_src` must be the source the NEXT test will use. The client applies the
    auto_flash flag and the download_path from separate messages, so parking on a
    different source lets it race: it can see auto_flash=1 while download_path is still
    the parked one and start copying the wrong source entirely. Parking on the upcoming
    source makes that race harmless.
    """
    api_send(DEST, park_src or SRC_ZIP, auto="0")
    t0 = time.time()
    while time.time() - t0 < timeout:
        if api_clients().get("file_info", {}).get("status") != "copying":
            time.sleep(2)  # let the worker thread write its final status
            if api_clients().get("file_info", {}).get("status") != "copying":
                return True
        time.sleep(1)
    return False


def reset(src) -> int:
    """Clean slate: stop acting, remove destination, return the log watermark."""
    quiesce(src)
    clean_dest()
    return log_len()


def wait_for_log(log_mark: int, needle: str, timeout=240):
    """Block until `needle` shows up in the log after `log_mark`."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in log_since(log_mark):
            return True
        time.sleep(2)
    return False


def free_gb() -> float:
    out = remote_ps("[math]::Round((Get-PSDrive C).Free / 1GB, 2)")
    for tok in out.split():
        try:
            return float(tok)
        except ValueError:
            continue
    return -1.0


def settle(timeout=900, stable=4):
    """Poll until status stops changing and is not 'copying'.
    Returns (final_client_json, samples) where samples tracks progress honesty."""
    last, same, samples = None, 0, []
    t0 = time.time()
    while time.time() - t0 < timeout:
        c = api_clients()
        fi = c.get("file_info", {})
        st = fi.get("status")
        samples.append((fi.get("progress", 0.0), fi.get("copied_bytes", 0),
                        fi.get("total_bytes", 0), st))
        if st == last and st != "copying":
            same += 1
            if same >= stable:
                return c, samples
        else:
            same = 0
            last = st
        time.sleep(1)
    return api_clients(), samples


def run_and_settle(log_mark: int, timeout=900, grace=22, start_wait=120):
    """Wait for the client to PICK UP the work, then for it to genuinely finish.

    'Finished' cannot be read off the status alone: status oscillates
    missing_files -> copying -> copy_complete -> missing_files across a multi-attempt
    repair sequence, so any 'status stable for N polls' rule can return mid-sequence.
    Instead: wait until not copying, then hold a grace window and require the client's
    own log to stay unchanged through it. If the log moves, the sequence is still going,
    so hold again (bounded).
    """
    t0 = time.time()
    started = False
    while time.time() - t0 < start_wait:
        fi = api_clients().get("file_info", {})
        if fi.get("status") == "copying":
            started = True
            break
        if any(k in log_since(log_mark) for k in
               ("[COPY] Detected ZIP file", "[COPY] Starting folder copy:",
                "[COPY] Delta copy:", "[COPY] *** Giving up:")):
            started = True
            break
        time.sleep(1)

    samples = []
    t1 = time.time()
    for _ in range(40):                      # bounded number of grace holds
        # 1) wait until nothing is actively copying
        while time.time() - t1 < timeout:
            c = api_clients()
            fi = c.get("file_info", {})
            samples.append((fi.get("progress", 0.0), fi.get("copied_bytes", 0),
                            fi.get("total_bytes", 0), fi.get("status")))
            if fi.get("status") != "copying":
                break
            time.sleep(0.3)

        # 2) hold a grace window; if the log moves the client is not done
        before = log_len()
        held = time.time()
        while time.time() - held < grace:
            c = api_clients()
            fi = c.get("file_info", {})
            samples.append((fi.get("progress", 0.0), fi.get("copied_bytes", 0),
                            fi.get("total_bytes", 0), fi.get("status")))
            time.sleep(0.3)
        if log_len() == before and \
                api_clients().get("file_info", {}).get("status") != "copying":
            break
        if time.time() - t1 > timeout:
            break
    return api_clients(), samples, started


def log_exact_totals(log_text: str):
    """Exact byte totals the client itself declared for each copy/extract in the window.

    Sampling /clients cannot catch these reliably - a delta copy of a few KB finishes
    well inside one poll interval - so the assertion reads the authoritative numbers
    the client logged.
    """
    out = []
    for line in log_text.splitlines():
        m = re.search(r"exact bytes: (\d+)", line)
        if m:
            out.append(int(m.group(1)))
    return out


class Result:
    def __init__(self, name):
        self.name, self.checks = name, []

    def check(self, ok, label, detail=""):
        self.checks.append((bool(ok), label, detail))
        print(f"    {'PASS' if ok else 'FAIL'}  {label}"
              + (f"   [{detail}]" if detail else ""))

    @property
    def ok(self):
        return all(c[0] for c in self.checks)


def assert_progress_honest(r: Result, samples, log_text=None, expect_total=None):
    prog = [s[0] for s in samples]
    over = [p for p in prog if p > 100.0001]
    r.check(not over, "progress never exceeds 100%", f"max={max(prog, default=0):.2f}%")

    backslides = []
    for i in range(1, len(samples)):
        p_prev, p_cur = prog[i - 1], prog[i]
        if p_cur + 0.01 < p_prev and p_cur > 0.0:
            backslides.append((p_prev, p_cur))
    r.check(not backslides, "progress never moves backwards",
            f"{len(backslides)} backslide(s): {backslides[:3]}")

    if expect_total is not None and log_text is not None:
        totals = log_exact_totals(log_text)
        r.check(expect_total in totals,
                f"declared copy scope == {expect_total} bytes", f"logged={totals}")


def count_runs(log_text: str) -> dict:
    return {
        "unzip_exec": log_text.count("[UNZIP] Executing extraction..."),
        "zip_detect": log_text.count("[COPY] Detected ZIP file"),
        "folder_start": log_text.count("[COPY] Starting folder copy:"),
        "delta_folder": log_text.count("[COPY] Delta copy:"),
        "gave_up": log_text.count("[COPY] *** Giving up:"),
        "attempts": log_text.count("[COPY] *** This is copy attempt #"),
    }


# ---------------------------------------------------------------- test cases

def t_full(src, label, expect_key):
    r = Result(label)
    n0 = reset(src)
    api_send(DEST, src)
    c, samples, started = run_and_settle(n0)
    log = log_since(n0)
    runs = count_runs(log)
    st = dest_state()

    r.check(started, "client picked up the work")
    r.check(st.get("FILES") == str(FULL_FILES), f"all {FULL_FILES} files present",
            f"got {st.get('FILES')}")
    r.check(st.get("BYTES") == str(FULL_BYTES), f"on-disk bytes == {FULL_BYTES}",
            f"got {st.get('BYTES')}")
    r.check(st.get("DIRS") == str(FULL_SUBDIRS),
            f"subfolder structure preserved ({FULL_SUBDIRS} dirs)", f"got {st.get('DIRS')}")
    r.check(st.get("ROOT_XML_IS_VARIANT") == "False",
            "root rawprogram0.xml NOT clobbered by nor_oob variant",
            f"got {st.get('ROOT_XML_IS_VARIANT')}")
    r.check(file_size(r"nor_oob\gpt_main0.bin") == SZ[r"nor_oob\gpt_main0.bin"],
            "nor_oob variant kept its own content (not root's)")
    r.check(runs[expect_key] == 1, f"exactly ONE {expect_key} (no restart loop)",
            f"got {runs[expect_key]}, attempts={runs['attempts']}")
    r.check(runs["gave_up"] == 0, "no give-up triggered")
    assert_progress_honest(r, samples, log_text=log, expect_total=FULL_BYTES)
    fi = c.get("file_info", {})
    r.check(fi.get("status") != "copying", "settled (not stuck copying)",
            f"status={fi.get('status')}")
    clean_dest()
    return r


def t_delta(src, label, expect_key, broken):
    """broken: dict rel -> 'delete' | int(truncate_to)"""
    r = Result(label)
    # stage 1: get to a complete destination
    m0 = reset(src)
    api_send(DEST, src)
    c, _, _ = run_and_settle(m0)
    st = dest_state()
    if st.get("BYTES") != str(FULL_BYTES):
        r.check(False, "precondition: complete destination", st.get("raw", "")[:200])
        clean_dest()
        return r

    # stage 2: break specific files (client must be idle while we do this)
    quiesce(src)
    ops = []
    for rel, how in broken.items():
        p = f"(Join-Path '{DEST}' '{rel}')"
        if how == "delete":
            ops.append(f"Remove-Item {p} -Force")
        else:
            ops.append(f"$fs=[System.IO.File]::Open({p},'Open','Write');"
                       f"$fs.SetLength({how});$fs.Close()")
    remote_ps("\n".join(ops))

    expect_bytes = sum(SZ[rel] for rel in broken)

    n0 = log_len()
    # nudge the client: re-send config so it re-evaluates promptly
    api_send(DEST, src)
    c, samples, started = run_and_settle(n0)
    log = log_since(n0)
    runs = count_runs(log)
    st = dest_state()

    r.check(started, "client noticed the broken files and started a repair")
    r.check(st.get("FILES") == str(FULL_FILES), "destination complete again",
            f"files={st.get('FILES')}")
    r.check(st.get("BYTES") == str(FULL_BYTES), f"on-disk bytes back to {FULL_BYTES}",
            f"got {st.get('BYTES')}")
    for rel in broken:
        r.check(file_size(rel) == SZ[rel], f"repaired {rel} to {SZ[rel]}",
                f"got {file_size(rel)}")
    r.check(runs[expect_key] == 1, f"exactly ONE {expect_key} (no restart loop)",
            f"got {runs[expect_key]}")
    r.check(runs["gave_up"] == 0, "no give-up triggered")
    # the decisive delta assertion: it moved only the broken bytes, not the whole installer
    assert_progress_honest(r, samples, log_text=log, expect_total=expect_bytes)
    r.check(expect_bytes < FULL_BYTES / 10,
            "delta scope is a small fraction of the installer",
            f"{expect_bytes} vs {FULL_BYTES}")
    clean_dest()
    return r


def t_unsatisfiable():
    """Source genuinely lacks a required file -> must terminate, not loop."""
    r = Result("T5 zip source missing a required file -> bounded give-up")
    n0 = reset(SRC_ZIP_MISSING)
    api_send(DEST, SRC_ZIP_MISSING)

    # This sequence is: full extract -> notice tools.fv absent -> a couple of cheap delta
    # retries -> permanent give-up. Waiting for the terminal marker is the only reliable
    # way to know it finished; a fixed settle window either cuts it short or wastes time.
    gave_up = wait_for_log(n0, "[COPY] *** Giving up:", timeout=240)
    time.sleep(3)
    log = log_since(n0)
    runs = count_runs(log)
    c = api_clients()
    fi = c.get("file_info", {})

    r.check(gave_up, "give-up guard fired within 240s")
    r.check(runs["unzip_exec"] <= 4,
            "bounded extraction attempts (no unbounded re-read)",
            f"unzip_exec={runs['unzip_exec']}")
    r.check("does not provide: tools.fv" in log,
            "give-up names the actually-missing file")
    r.check("Missing files not found in source: tools.fv" in log,
            "delta extraction reported the unresolved file")

    # and it must STAY stopped
    before = runs["unzip_exec"]
    time.sleep(30)
    after = count_runs(log_since(n0))["unzip_exec"]
    r.check(after == before, "stays stopped after giving up (no further attempts)",
            f"{before} -> {after}")
    clean_dest()
    return r


TESTS = {
    "T1": lambda: t_full(SRC_ZIP, "T1 zip source, full extraction", "unzip_exec"),
    "T2": lambda: t_delta(SRC_ZIP, "T2 zip source, delta repair", "unzip_exec",
                          {r"nor_oob\gpt_main0.bin": "delete", r"tools.fv": 1000}),
    "T3": lambda: t_full(SRC_FOLDER, "T3 folder source, full copy", "folder_start"),
    "T4": lambda: t_delta(SRC_FOLDER, "T4 folder source, delta repair", "delta_folder",
                          {r"nor_oob\zeros_1sector.bin": "delete", r"gpt_main0.bin": 99}),
    "T5": t_unsatisfiable,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--rounds", type=int, default=1)
    a = ap.parse_args()

    names = [n.strip() for n in a.only.split(",") if n.strip()] or list(TESTS)

    fg = free_gb()
    print(f"remote free space: {fg} GB")
    if 0 <= fg < 5:
        print("ABORT: less than 5GB free on the remote disk")
        return 2

    allres = []
    for rnd in range(1, a.rounds + 1):
        print(f"\n{'='*72}\nROUND {rnd}/{a.rounds}\n{'='*72}")
        for n in names:
            print(f"\n>>> {n}")
            try:
                res = TESTS[n]()
            except Exception as e:
                res = Result(f"{n} (exception)")
                res.check(False, "test raised", repr(e)[:300])
            allres.append((rnd, n, res))

    print(f"\n{'='*72}\nSUMMARY\n{'='*72}")
    bad = 0
    for rnd, n, res in allres:
        n_ok = sum(1 for c in res.checks if c[0])
        print(f"round{rnd} {n:4} {'PASS' if res.ok else 'FAIL'}  "
              f"({n_ok}/{len(res.checks)})  {res.name}")
        if not res.ok:
            bad += 1
            for ok, label, detail in res.checks:
                if not ok:
                    print(f"         - {label}  [{detail}]")
    print(f"\n{len(allres)-bad}/{len(allres)} passed")
    print(f"remote free space after: {free_gb()} GB")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
