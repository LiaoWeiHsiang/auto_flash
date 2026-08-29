#!/usr/bin/env python3
"""Build a small synthetic installer that reproduces the real installer's pathologies.

Reproduced faithfully (these are what broke the real 44GB flow):
  1. Duplicate basenames across NOR-variant subfolders (gpt_main0.bin, zeros_1sector.bin,
     rawprogram0.xml all exist at root AND inside nor_oob/ and dualnor_*/nor*/ with
     DIFFERENT content and sizes, so a wrong-variant overwrite is detectable).
  2. A subfolder XML variant (nor_oob/rawprogram0.xml) that references subfolder-relative
     paths using BACKSLASHES (nor_oob\\gpt_main0.bin) - flattening lets this clobber the
     real root manifest and then demands files that flatten never creates.
  3. The zip stores its entries with BACKSLASH separators, which is what the real
     Installer.zip does (non-standard; ZIP spec mandates '/'). Any '/'-based logic
     silently misbehaves on it.

Emits <out>/installer_src/ (folder source) and <out>/installer_src.zip (zip source, backslash
entries) describing the same logical installer, so folder-copy and unzip can be compared.
"""

import os
import shutil
import struct
import sys
import zipfile


def blob(seed: int, size: int) -> bytes:
    """Deterministic filler whose first bytes encode the seed, so a wrong-variant
    overwrite is detectable by content as well as by size."""
    head = struct.pack("<I", seed)
    body = bytes(((seed * 31 + i) & 0xFF) for i in range(min(size, 4096) - len(head)))
    out = head + body
    if size > len(out):
        out = out + (out * (size // len(out) + 1))[: size - len(out)]
    return out[:size]


# (relative path with '/', seed, size). Sizes differ per variant on purpose.
FILES = [
    # --- root: the real manifest set ---
    ("EMMCDL.exe", 1, 233_760),
    ("MainOS.bin", 2, 24 * 1024 * 1024),   # the "big file" so progress is measurable
    ("tools.fv", 3, 491_520),
    ("gpt_main0.bin", 4, 24_576),
    ("gpt_backup0.bin", 5, 20_480),
    ("zeros_1sector.bin", 6, 4_096),
    ("smbios.bin", 7, 8_192),
    ("xbl_s.melf", 8, 131_072),
    ("bootfw1_64.bin", 9, 262_144),
    ("bootfw2.bin", 10, 262_144),
    ("SYSFW_VERSION.bin", 11, 1_024),
    # --- nor_oob variant: SAME basenames, DIFFERENT sizes+content ---
    ("nor_oob/gpt_main0.bin", 104, 28_672),
    ("nor_oob/gpt_backup0.bin", 105, 24_576),
    ("nor_oob/zeros_1sector.bin", 106, 8_192),
    # --- dualnor variants: more same-basename collisions ---
    ("dualnor_32_16mb/nor0/gpt_main0.bin", 204, 32_768),
    ("dualnor_32_16mb/nor0/zeros_1sector.bin", 206, 12_288),
    ("dualnor_32_16mb/nor1/gpt_main0.bin", 304, 36_864),
    ("dualnor_32_16mb/nor1/zeros_1sector.bin", 306, 16_384),
    ("dualnor_64_16mb/nor0/gpt_main0.bin", 404, 40_960),
    ("dualnor_64_16mb/nor0/zeros_1sector.bin", 406, 20_480),
]

# Root manifest. Mostly bare root names, PLUS one subfolder-relative reference with a
# backslash - real Qualcomm manifests do this, and it is the only way to exercise delta
# repair of a file that lives inside a variant subfolder.
ROOT_RAWPROGRAM = r"""<?xml version="1.0"?>
<data>
  <program filename="gpt_main0.bin" label="PrimaryGPT" />
  <program filename="gpt_backup0.bin" label="BackupGPT" />
  <program filename="zeros_1sector.bin" label="Zeros" />
  <program filename="" label="EraseOnly" />
  <program filename="smbios.bin" label="smbios" />
  <program filename="xbl_s.melf" label="xbl_s" />
  <program filename="nor_oob\gpt_main0.bin" label="OobPrimaryGPT" />
  <program filename="nor_oob\zeros_1sector.bin" label="OobZeros" />
</data>
"""

# The variant manifest that causes the damage when flattened: it references
# nor_oob\\-prefixed paths with BACKSLASHES, exactly like the real one.
NOR_OOB_RAWPROGRAM = r"""<?xml version="1.0"?>
<data>
  <program filename="nor_oob\gpt_main0.bin" label="PrimaryGPT" />
  <program filename="nor_oob\gpt_backup0.bin" label="BackupGPT" />
  <program filename="nor_oob\zeros_1sector.bin" label="Zeros" />
  <program filename="" label="EraseOnly" />
  <program filename="bootfw1_64.bin" label="bootfw1" />
  <program filename="bootfw2.bin" label="bootfw2" />
  <program filename="SYSFW_VERSION.bin" label="sysfw" />
</data>
"""

WDFLASH = """<?xml version="1.0"?>
<data>
  <program filename="MainOS.bin" label="MainOS" />
  <program filename="tools.fv" label="tools" />
  <program filename="" label="EraseOnly" />
</data>
"""

XML_FILES = [
    ("rawprogram0.xml", ROOT_RAWPROGRAM),
    ("WDFlash.xml", WDFLASH),
    ("nor_oob/rawprogram0.xml", NOR_OOB_RAWPROGRAM),
    ("dualnor_32_16mb/nor0/rawprogram0.xml", ROOT_RAWPROGRAM),
]


def build(out_dir: str) -> None:
    src = os.path.join(out_dir, "installer_src")
    if os.path.isdir(src):
        shutil.rmtree(src)

    manifest = []  # (relpath_with_fwd_slash, bytes)

    for rel, seed, size in FILES:
        manifest.append((rel, blob(seed, size)))
    for rel, text in XML_FILES:
        manifest.append((rel, text.encode("utf-8")))

    # 1) folder source
    for rel, data in manifest:
        p = os.path.join(src, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    # 2) zip source, entries stored with BACKSLASH separators (matches the real zip)
    zip_path = os.path.join(out_dir, "installer_src.zip")
    if os.path.exists(zip_path):
        os.remove(zip_path)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as zf:
        for rel, data in manifest:
            zf.writestr(rel.replace("/", "\\"), data)

    total = sum(len(d) for _, d in manifest)
    uniq_base = {os.path.basename(r).lower() for r, _ in manifest}
    print(f"files={len(manifest)} total_bytes={total} unique_basenames={len(uniq_base)}")
    print(f"folder={src}")
    print(f"zip={zip_path} zip_size={os.path.getsize(zip_path)}")
    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()
    print(f"zip entries use backslash: {sum(1 for n in names if chr(92) in n)}/{len(names)}")
    print(f"zip entries containing forward slash: {sum(1 for n in names if '/' in n)}")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "/tmp/af_testdata")
