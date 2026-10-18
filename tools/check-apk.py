#!/usr/bin/env python3
"""Packaging check of a built XenDroid APK (plan A08). Pure Python; aapt2 optional.

Fails when the APK:
  * is not a valid ZIP, or has no APK Signature Scheme v2+ block (unsigned);
  * lacks the emulator core for arm64-v8a, or that library is not an AArch64
    shared object exporting JNI_OnLoad (where every native method is registered);
  * carries proprietary or private data: DLLs (Lossless.dll), LSFG/shader caches,
    game images (ISO/XEX/ZAR/XCP) or Xbox profile data (GPD);
  * misses one of the license texts the combined GPL build must ship;
  * (with --aapt2) declares another package minSdk/targetSdk than expected.

Usage: python3 tools/check-apk.py APK [--aapt2 PATH] [--min-sdk 29] [--target-sdk 35]
"""
import argparse
import struct
import subprocess
import sys
import zipfile

CORE_LIBRARY = "lib/arm64-v8a/libe.so"
LICENSES = {
    "assets/engine-licenses/dxbc-license.txt",
    "assets/engine-licenses/fidelityfx-notice.txt",
    "assets/engine-licenses/lsfg-GPL-3.0.txt",
    "assets/engine-licenses/mbedtls-license.txt",
    "assets/engine-licenses/sgsr-BSD-3-Clause.txt",
    "assets/engine-licenses/winfg-MIT.txt",
    "assets/engine-licenses/winfg-third-party.txt",
    "assets/engine-licenses/xendroid-AGPL-3.0.txt",
    "assets/engine-licenses/xendroid-notices.txt",
    "assets/font-licenses/Barlow-OFL.txt",
    "assets/font-licenses/JetBrainsMono-OFL.txt",
}
FORBIDDEN_SUFFIXES = (".dll", ".cache", ".iso", ".xex", ".zar", ".xcp", ".gpd")
FORBIDDEN_FRAGMENTS = ("lossless",)


def signing_block_present(path):
    """APK Signing Block ("APK Sig Block 42") right before the central directory."""
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        tail_len = min(size, 65536 + 22)
        f.seek(size - tail_len)
        tail = f.read()
        eocd = tail.rfind(b"PK\x05\x06")
        if eocd < 0:
            return False
        cd_offset = struct.unpack_from("<I", tail, eocd + 16)[0]
        if cd_offset < 24:
            return False
        f.seek(cd_offset - 16)
        return f.read(16) == b"APK Sig Block 42"


def elf_dynamic_symbols(data):
    """(machine, is_shared_object, names of defined dynamic symbols) of an ELF64 LE file."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None, False, set()
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 58)
    sections = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from("<I", data, base + 4)[0]
        sh_offset, sh_size = struct.unpack_from("<QQ", data, base + 24)
        sh_link = struct.unpack_from("<I", data, base + 40)[0]
        sh_entsize = struct.unpack_from("<Q", data, base + 56)[0]
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))
    names = set()
    for sh_type, offset, size, link, entsize in sections:
        if sh_type != 11 or not entsize:            # SHT_DYNSYM
            continue
        str_offset = sections[link][1]
        for j in range(size // entsize):
            sym = offset + j * entsize
            st_name = struct.unpack_from("<I", data, sym)[0]
            st_shndx = struct.unpack_from("<H", data, sym + 6)[0]
            if st_name and st_shndx:                 # defined here, not imported
                end = data.index(b"\0", str_offset + st_name)
                names.add(data[str_offset + st_name:end].decode("ascii", "replace"))
    return e_machine, e_type == 3, names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("apk")
    parser.add_argument("--aapt2")
    parser.add_argument("--min-sdk", default="29")
    parser.add_argument("--target-sdk", default="35")
    args = parser.parse_args()
    problems = []

    try:
        apk = zipfile.ZipFile(args.apk)
    except (zipfile.BadZipFile, OSError) as e:
        print(f"check-apk: cannot read {args.apk}: {e}")
        return 1
    with apk:
        bad = apk.testzip()
        if bad:
            problems.append(f"corrupt entry: {bad}")
        names = apk.namelist()
        for name in names:
            lower = name.lower()
            if lower.endswith(FORBIDDEN_SUFFIXES) or any(f in lower for f in FORBIDDEN_FRAGMENTS):
                problems.append(f"must not be packaged: {name}")
        missing = sorted(LICENSES - set(names))
        if missing:
            problems.append(f"license texts missing: {', '.join(missing)}")
        if CORE_LIBRARY not in names:
            problems.append(f"{CORE_LIBRARY} missing")
        else:
            machine, shared, symbols = elf_dynamic_symbols(apk.read(CORE_LIBRARY))
            if machine != 183 or not shared:
                problems.append(f"{CORE_LIBRARY} is not an AArch64 shared object (machine {machine})")
            if "JNI_OnLoad" not in symbols:
                problems.append(f"{CORE_LIBRARY} does not export JNI_OnLoad: no native method would be registered")
        # Any native library for another ABI makes Android offer the app to devices
        # that have no emulator core for it: they install it and crash at the first load.
        abis = sorted({n.split("/")[1] for n in names if n.startswith("lib/") and n.count("/") >= 2})
        if abis != ["arm64-v8a"]:
            problems.append(f"native libraries for {', '.join(abis) or 'no ABI'}; the core exists for arm64-v8a only")
    if not signing_block_present(args.apk):
        problems.append("no APK Signature Scheme v2/v3 block: the APK is unsigned or v1-only")

    if args.aapt2:
        badging = subprocess.run([args.aapt2, "dump", "badging", args.apk], capture_output=True, text=True).stdout
        # aapt2 prints minSdkVersion:'N'; the older aapt printed sdkVersion:'N'.
        if f"minSdkVersion:'{args.min_sdk}'" not in badging and f"\nsdkVersion:'{args.min_sdk}'" not in badging:
            problems.append(f"minSdk is not {args.min_sdk}")
        if f"targetSdkVersion:'{args.target_sdk}'" not in badging:
            problems.append(f"targetSdk is not {args.target_sdk}")
        if "native-code: 'arm64-v8a'\n" not in badging:
            problems.append("declared native code is not exactly arm64-v8a")

    for p in problems:
        print("check-apk:", p)
    if problems:
        return 1
    print(f"check-apk: {args.apk}: {len(names)} entries, core arm64 with JNI_OnLoad, signed, "
          f"{len(LICENSES)} license texts, no proprietary or private data")
    return 0


if __name__ == "__main__":
    sys.exit(main())
