#!/usr/bin/env python3
"""Prove, against the REAL 44GB UNC archive, that backslash-separated entries become
real nested directories - without extracting 44 GB.

The full-extraction run had to be cancelled at ~40%, and this archive stores its
root-level files first, so no subfolder entry had been reached yet (dirs=0 was
inconclusive, not a failure). Here we drive the SAME selection + extraction logic the
product generates, restricted to the nor_oob\\* entries (~50 KB), and check that a real
'nor_oob' directory appears with per-file sizes matching the archive.
"""

import sys
import time

sys.path.insert(0, "tests")
import verify_copy_unzip as V

REAL_ZIP = r"\\trueforge-bm\workspace\8480\5300\CRD_NORNVME\Installer.zip"
OUT = r"C:\af_test\subdir_probe"

# the product's generated script, structure-preserving, restricted by $wanted
PS = r"""
$ProgressPreference='SilentlyContinue'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$ErrorActionPreference = 'Stop'
$wanted = @('nor_oob\gpt_main0.bin','nor_oob\gpt_backup0.bin','nor_oob\zeros_1sector.bin')
$flatten = $false
$zip = [System.IO.Compression.ZipFile]::OpenRead('__ZIP__')
$dest = '__DEST__'
New-Item -ItemType Directory -Path $dest -Force | Out-Null
$sel = New-Object System.Collections.Specialized.OrderedDictionary
$cand = New-Object System.Collections.ArrayList
foreach ($e in $zip.Entries) {
  $full = $e.FullName.Replace('/','\')
  if ([string]::IsNullOrWhiteSpace($full)) { continue }
  if ($full.EndsWith('\')) { continue }
  $base = $full.Substring($full.LastIndexOf('\') + 1)
  if ([string]::IsNullOrEmpty($base)) { continue }
  if ($flatten) { $rel = $base } else { $rel = $full }
  $bad = $false
  foreach ($seg in $rel.Split('\')) { if ($seg -eq '..' -or $seg -eq '.') { $bad = $true } }
  if ($bad -or $rel.StartsWith('\') -or $rel.Contains(':')) { Write-Output "SKIP:$full"; continue }
  [void]$cand.Add(@{ E = $e; R = $rel; F = $full.ToLower(); B = $base.ToLower() })
}
if ($wanted.Count -gt 0) {
  $hit = @{}
  foreach ($c in $cand) { if ($wanted -contains $c.F) { $sel[$c.R.ToLower()] = $c; $hit[$c.F] = $true } }
  foreach ($w in $wanted) {
    if ($hit.ContainsKey($w)) { continue }
    foreach ($c in $cand) { if ($c.B -eq $w) { $sel[$c.R.ToLower()] = $c; $hit[$w] = $true; break } }
  }
} else {
  foreach ($c in $cand) { $sel[$c.R.ToLower()] = $c }
}
$items = @($sel.Values)
$totalBytes = [int64]0
foreach ($i in $items) { $totalBytes += [int64]$i.E.Length }
Write-Output ("SELECTED=" + $items.Count)
Write-Output ("TOTALBYTES=" + $totalBytes)
foreach ($i in $items) {
  $destFile = Join-Path $dest $i.R
  $destDir = Split-Path $destFile -Parent
  if (!(Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
  [System.IO.Compression.ZipFileExtensions]::ExtractToFile($i.E, $destFile, $true)
  Write-Output ("WROTE=" + $i.R)
}
$zip.Dispose()
Write-Output "DONE"
"""

EXPECT = {
    r"nor_oob\gpt_main0.bin": 24576,
    r"nor_oob\gpt_backup0.bin": 20480,
    r"nor_oob\zeros_1sector.bin": 4096,
}


def main():
    ok = []

    def check(cond, label, detail=""):
        ok.append(bool(cond))
        print(f"  {'PASS' if cond else 'FAIL'}  {label}" + (f"   [{detail}]" if detail else ""))

    V.remote_ps(f"if (Test-Path '{OUT}') {{ Remove-Item '{OUT}' -Recurse -Force }}")
    out = V.remote_ps(PS.replace("__ZIP__", REAL_ZIP).replace("__DEST__", OUT))
    print(out.strip()[:900])

    check("DONE" in out, "extraction of selected entries completed")
    check("SELECTED=3" in out, "exactly the 3 requested subfolder entries were selected")
    check("TOTALBYTES=49152" in out,
          "declared scope is the 3 files only (49152 bytes)",
          "24576+20480+4096")

    state = V.remote_ps(f"""
$d = @(Get-ChildItem '{OUT}' -Recurse -Directory)
Write-Output ("DIRS=" + $d.Count)
foreach ($x in $d) {{ Write-Output ("DIRNAME=" + $x.Name) }}
$f = @(Get-ChildItem '{OUT}' -Recurse -File)
foreach ($x in $f) {{
  Write-Output ("FILE=" + $x.FullName.Substring({len(OUT)} + 1) + "|" + $x.Length)
}}
""")
    print(state.strip()[:900])

    check("DIRS=1" in state, "exactly one real subdirectory created")
    check("DIRNAME=nor_oob" in state,
          "the subdirectory is really named nor_oob (backslash entry became a folder)")

    got = {}
    for line in state.splitlines():
        if line.startswith("FILE="):
            rel, _, sz = line[5:].rpartition("|")
            got[rel] = int(sz)
    for rel, sz in EXPECT.items():
        check(got.get(rel) == sz, f"{rel} extracted at correct size {sz}",
              f"got {got.get(rel)}")

    V.remote_ps(f"if (Test-Path '{OUT}') {{ Remove-Item '{OUT}' -Recurse -Force }}")
    print(f"\n{sum(ok)}/{len(ok)} passed")
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
