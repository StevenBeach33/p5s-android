#!/usr/bin/env python3
"""Checks games/p5s/import's scanner on synthetic Unity-shaped data (no game data involved).
Builds p5s_scan with g++, makes: a plain NVN block, a Shader-like object (nested and flat arrays) whose sub-blobs
are LZ4-compressed, and UnityFS bundles (blocks info at the start and at the end) around it, and expects every
block back. Usage: python games/p5s/tools/test_scan.py   (needs g++, pip install lz4)"""
import os, struct, subprocess, sys, tempfile, random
import lz4.block

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
IMP = os.path.join(ROOT, "games", "p5s", "import")
EXIT = bytes.fromhex("0f0007000000e3")[::-1] if False else struct.pack("<Q", 0xe30000000007000f)
u32 = lambda v: struct.pack("<I", v)


def nvn_block(stage, salt):
    """control (0x880) + code (0x100) of a program that passes the scan's checks."""
    ctrl = bytearray(0x880)
    ctrl[0:4] = u32(0x98761234)
    ctrl[0x704:0x708] = u32(0x100)
    ctrl[0x708:0x70c] = u32(0x30)
    ctrl[0x6f8:0x6fc] = u32(0x40)
    code = bytearray(0x100)
    code[0:4] = u32(0x12345678)
    code[0x30:0x34] = u32({"vertex": 1, "pixel": 5}[stage] << 10)
    code[0x60:0x68] = EXIT
    code[0x68:0x70] = bytes([salt, 2, 3, 4, 5, 6, 7, 8])
    return bytes(ctrl) + bytes(0x10) + bytes(code)


def shader_object(blobs, nested=True):
    comp = [lz4.block.compress(b, store_size=False) for b in blobs]
    offs, o = [], 0
    for c in comp:
        offs.append(o)
        o += len(c)
    arr = lambda v: u32(len(v)) + b"".join(u32(x) for x in v)
    wrap = (lambda v: u32(1) + arr(v)) if nested else arr
    body = wrap(offs) + wrap([len(c) for c in comp]) + wrap([len(b) for b in blobs])
    blob = b"".join(comp)
    junk = random.Random(5).randbytes(776)  # objects are 4-aligned in a file
    return junk + u32(1) + u32(38) + body + u32(len(blob)) + blob + b"\0" * 3 + junk


def unityfs(payload, info_at_end, version=7):
    blocks = [payload[i:i + 1500] for i in range(0, len(payload), 1500)]
    comp = [lz4.block.compress(b, store_size=False) for b in blocks]
    info = bytes(16) + struct.pack(">I", len(blocks))
    info += b"".join(struct.pack(">IIH", len(b), len(c), 2) for b, c in zip(blocks, comp))
    info += struct.pack(">I", 1) + struct.pack(">QQI", 0, len(payload), 4) + b"x\0"
    cinfo = lz4.block.compress(info, store_size=False)
    flags = 2 | 0x40 | (0x80 if info_at_end else 0)
    head = b"UnityFS\0" + struct.pack(">I", version) + b"5.x.x\0" + b"2021.3.0f1\0"
    rest = struct.pack(">I", len(cinfo)) + struct.pack(">I", len(info)) + struct.pack(">I", flags)
    head2 = head + struct.pack(">Q", 0) + rest
    if version >= 7:
        head2 += b"\0" * (-len(head2) % 16)
    data = b"".join(comp)
    return head2 + (data + cinfo if info_at_end else cinfo + data)


def run(name, data, expect_blocks, tmp, expect_stage=None):
    src = os.path.join(tmp, "in_" + name)
    out = os.path.join(tmp, "out_" + name)
    open(src, "wb").write(data)
    r = subprocess.run([os.path.join(tmp, "p5s_scan"), src, out], capture_output=True, text=True)
    got = [l for l in r.stdout.splitlines() if l.startswith("code blocks:")]
    n = int(got[0].split()[2]) if got else -1
    ok = n == expect_blocks
    print(f"{'ok ' if ok else 'FAIL'} {name}: {n} blocks (expected {expect_blocks})")
    if not ok:
        print(r.stdout, r.stderr)
    return ok


def main():
    tmp = tempfile.mkdtemp()
    cc = subprocess.run(["g++", "-std=c++20", "-O2", f"-I{ROOT}/android/wonder/importer", f"-I{IMP}",
                         f"{IMP}/p5s_scan_cli.cpp", f"{IMP}/p5s_shaders.cpp", "-o", os.path.join(tmp, "p5s_scan"), "-lz"],
                        capture_output=True, text=True)
    if cc.returncode:
        sys.exit(cc.stderr)
    v, p = nvn_block("vertex", 11), nvn_block("pixel", 22)
    ok = True
    ok &= run("plain", bytes(100) + v + bytes(300) + p + bytes(50), 2, tmp)
    nested = shader_object([v, p], nested=True)
    flat = shader_object([v, p], nested=False)
    ok &= run("nested_blob", nested, 2, tmp)
    ok &= run("flat_blob", flat, 2, tmp)
    ok &= run("not_shaders", random.Random(1).randbytes(100000), 0, tmp)
    for end in (False, True):
        ok &= run(f"unityfs_info_{'end' if end else 'start'}", unityfs(nested, end), 2, tmp)
    ok &= run("unityfs_v6", unityfs(nested, False, version=6), 2, tmp)
    # KIDS archive (IDRK): the shaders as consecutive 1000-byte zlib chunks (a shader spans several), junk 0x78 0x9c around
    import zlib
    payload = bytes(500) + v + bytes(2000) + p + bytes(300)
    parts = [zlib.compress(payload[i:i + 1000]) for i in range(0, len(payload), 1000)]
    junk = b"\x78\x9c" + random.Random(5).randbytes(40)
    kids = b"IDRK" + bytes(4096) + junk * 50 + b"".join(parts) + junk * 20 + bytes(100)
    ok &= run("kids_zlib", kids, 2, tmp)
    ok &= run("kids_zlib_two_runs", kids + random.Random(9).randbytes(777) + b"".join(parts), 2, tmp)
    # a folder shaped like Strikers's RomFS: media with hash-suffixed names are skipped, bundles are not
    d = os.path.join(tmp, "romfs", "Data", "StreamingAssets", "aa", "Switch")
    os.makedirs(os.path.join(d, "CriStreamingData"))
    open(os.path.join(d, "CriStreamingData", "movie_a.usm" + "e25f1723b64bc8668d9cf728f44ae84d"), "wb").write(nested)
    open(os.path.join(d, "CriStreamingData", "bgm_b.acb" + "426d4179f008ce22468f70db9e2fde33"), "wb").write(nested)
    open(os.path.join(d, "0007c150b15aa530c201cb3ab8e74ed2.bundle"), "wb").write(unityfs(nested, False))
    r = subprocess.run([os.path.join(tmp, "p5s_scan"), os.path.join(tmp, "romfs"), os.path.join(tmp, "out_romfs")],
                       capture_output=True, text=True)
    n = [int(l.split()[2]) for l in r.stdout.splitlines() if l.startswith("code blocks:")]
    good = n == [2] and "files scanned: 1 " in r.stdout
    print(f"{'ok ' if good else 'FAIL'} romfs_layout: media skipped, bundle scanned ({r.stdout.splitlines()[:1]})")
    ok &= good
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
