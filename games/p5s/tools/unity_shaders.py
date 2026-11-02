#!/usr/bin/env python3
"""Persona 5 Strikers (Unity, Switch): NVN shaders out of the game's RomFS, for the SPIR-V harness.

Writes the layout the harness reads (spirv_harness --batch), the same as games/p5r/tools/gsc_shaders.py:
  <out>/<group>/manifest.tsv, code_<sha1>.bin, ctrl_<sha1>.bin

Unity keeps a shader's compiled programs in its Shader object: per platform, one or more sub-program blobs, each
LZ4-compressed inside `compressedBlob` (offsets / compressedLengths / decompressedLengths). On Switch a blob is NVN's
own output, so it holds standard NVN control sections (magic 0x98761234) and code blocks (0x12345678). They are found
the way tools/maxwell/exefs_shaders.py finds them: by magic, then checked against the section's own size fields, so
the Unity wrapper around them does not matter.

Steps per file of the RomFS (extracted from your own dump with the importer, not by this script):
  1. scan the raw bytes (a bundle or asset that stores blobs uncompressed);
  2. if UnityPy can open it: scan every Shader's decompressed sub-program blobs, and its raw object data.

NOT TESTED AGAINST A REAL DUMP: the Unity field layout (UnityPy's Shader) and where Strikers keeps its shaders are
assumptions until this runs on a real RomFS. Run with --dry-run first and read the summary: if "blocks found" is 0
while "blobs with NVN magic" is not, the pairing needs a look; if both are 0, the shaders are somewhere this script
does not read (the report lists what it saw).

Usage:
  python games/p5s/tools/unity_shaders.py --romfs private/p5s/extract/romfs --out private/p5s/shaders_extracted
  python games/p5s/tools/unity_shaders.py --romfs <dir> --out <dir> --dry-run      # only the summary
  python games/p5s/tools/unity_shaders.py --selftest                                # synthetic block, no game data
Needs: pip install lz4 UnityPy
"""
import argparse
import collections
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "maxwell"))
import exefs_shaders  # noqa: E402

HEADER = ("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\t"
          "const_size\tcode_sha1_16\tcode_file\tctrl_file\n")
CTRL_MAGIC = struct.pack("<I", 0x98761234)
DEFAULT_GREP = r"\.(assets|bundle|unity3d|ress|resource|bin|dat)$|(^|/)[^./]+$"


def lz4_block(blob, size):
    try:
        import lz4.block
        return lz4.block.decompress(blob, uncompressed_size=size)
    except Exception:
        return None


def flat(x):
    return list(x) if isinstance(x, (list, tuple)) else [x]


def shader_blobs(s):
    """(label, bytes) for each sub-program blob of a UnityPy Shader, LZ4-decompressed (a blob whose compressed and
    decompressed lengths agree is taken as stored)."""
    platforms = list(getattr(s, "platforms", None) or [])
    offsets, clen, dlen = (getattr(s, n, None) for n in ("offsets", "compressedLengths", "decompressedLengths"))
    blob = bytes(getattr(s, "compressedBlob", b"") or b"")
    if not (platforms and offsets is not None and clen is not None and dlen is not None and blob):
        return
    for i, plat in enumerate(platforms):
        for j, (o, c, d) in enumerate(zip(flat(offsets[i]), flat(clen[i]), flat(dlen[i]))):
            chunk = blob[o:o + c]
            raw = chunk if c == d else lz4_block(chunk, d)
            yield f"p{plat}_{j}", raw if raw is not None else chunk


class Extractor:
    def __init__(self, out, dry):
        self.out, self.dry = out, dry
        self.seen = set()
        self.stats = collections.Counter()
        self.stages = collections.Counter()
        self.platforms = collections.Counter()
        self.unity_versions = collections.Counter()
        self.type_counts = collections.Counter()

    def scan(self, group, buf):
        """Find NVN blocks in buf, write the new ones. Returns the number of new code blocks."""
        if CTRL_MAGIC not in buf:
            return 0
        self.stats["buffers with NVN magic"] += 1
        found, u32 = exefs_shaders.find(buf)
        self.stats["blocks found"] += len(found)
        gdir = os.path.join(self.out, group)
        rows, new = [], 0
        for p, c in found:
            s = u32(c + 0x704)
            code, ctrl = buf[p:p + s], buf[c:c + 0x880]
            h = hashlib.sha1(code).hexdigest()[:16]
            if h in self.seen:
                continue
            self.seen.add(h)
            try:
                stage = exefs_shaders.stage_of(u32, p, c)
            except KeyError:
                self.stats["blocks with an unsupported stage (skipped)"] += 1
                continue
            self.stages[stage] += 1
            new += 1
            cf = f"ctrl_{hashlib.sha1(ctrl).hexdigest()[:16]}.bin"
            if not self.dry:
                os.makedirs(gdir, exist_ok=True)
                open(os.path.join(gdir, f"code_{h}.bin"), "wb").write(code)
                open(os.path.join(gdir, cf), "wb").write(ctrl)
            rows.append(f"{group}/{p:#x}/{stage}\t{stage}\t{p:#x}\t{s:#x}\t{c:#x}\t{0x880:#x}\t{u32(c + 0x708):#x}\t"
                        f"{u32(c + 0x6f8):#x}\t{u32(c + 0x700):#x}\t{u32(c + 0x6fc):#x}\t{h}\tcode_{h}.bin\t{cf}\n")
        if rows and not self.dry:
            mp = os.path.join(gdir, "manifest.tsv")
            with open(mp, "a", newline="\n") as m:
                if m.tell() == 0:
                    m.write(HEADER)
                m.writelines(rows)
        return new

    def unity_file(self, path, group):
        try:
            import UnityPy
        except ImportError:
            self.stats["UnityPy missing (Unity-aware scan skipped)"] += 1
            return
        try:
            env = UnityPy.load(path)
        except Exception as e:  # not a Unity file, or a version UnityPy cannot read
            self.stats["files UnityPy could not open"] += 1
            self.last_error = f"{path}: {e}"
            return
        objects = list(env.objects)
        if not objects:
            self.stats["files with no Unity objects"] += 1
            return
        self.stats["Unity files opened"] += 1
        for obj in objects:
            self.type_counts[obj.type.name] += 1
            if obj.type.name != "Shader":
                continue
            self.stats["Shader objects"] += 1
            try:
                s = obj.read()
                self.unity_versions[".".join(map(str, getattr(obj.assets_file, "version", None) or ()))
                                    or str(getattr(obj.assets_file, "unity_version", "?"))] += 1
                name = re.sub(r"[^A-Za-z0-9_]+", "_", str(getattr(s, "m_Name", "") or f"shader{obj.path_id}")).strip("_")
                for plat in getattr(s, "platforms", None) or []:
                    self.platforms[plat] += 1
                blobs = list(shader_blobs(s))
                self.stats["sub-program blobs"] += len(blobs)
                for label, raw in blobs:
                    if CTRL_MAGIC in raw:
                        self.stats["blobs with NVN magic"] += 1
                    self.scan(f"{group}_{name}_{label}"[:120], raw)
                self.scan(f"{group}_{name}_obj"[:120], obj.get_raw_data())
            except Exception as e:
                self.stats["Shader objects that failed to read"] += 1
                self.last_error = f"{path} shader {obj.path_id}: {e}"

    def walk(self, root, grep):
        rx = re.compile(grep, re.I)
        for dp, _, names in os.walk(root):
            for n in sorted(names):
                path = os.path.join(dp, n)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                if not rx.search(rel):
                    continue
                self.stats["files scanned"] += 1
                group = re.sub(r"[^A-Za-z0-9_]+", "_", rel).strip("_").lower()[:60]
                with open(path, "rb") as f:
                    buf = f.read()
                self.scan("raw_" + group, buf)
                del buf
                self.unity_file(path, group)

    def report(self):
        lines = ["== Strikers shader scan =="]
        lines += [f"{k}: {v}" for k, v in sorted(self.stats.items())]
        lines.append(f"distinct code blocks: {len(self.seen)}  stages: {dict(self.stages)}")
        lines.append(f"Shader platform ids seen: {dict(self.platforms)}")
        lines.append(f"Unity versions seen: {dict(self.unity_versions)}")
        lines.append("object types (top 12): " + ", ".join(f"{k}={v}" for k, v in self.type_counts.most_common(12)))
        if getattr(self, "last_error", None):
            lines.append(f"last error: {self.last_error}")
        return "\n".join(lines)


def selftest():
    """A synthetic NVN block (control + code, constraints of exefs_shaders.find) in an LZ4 blob in a fake buffer."""
    u32 = lambda v: struct.pack("<I", v)
    ctrl = bytearray(0x880)
    ctrl[0:4] = u32(0x98761234)
    ctrl[0x704:0x708] = u32(0x100)    # code size
    ctrl[0x708:0x70c] = u32(0x30)     # program start
    ctrl[0x6f8:0x6fc] = u32(0x40)     # program length
    code = bytearray(0x100)
    code[0:4] = u32(0x12345678)
    code[0x30:0x34] = u32(1 << 10)    # SPH: vertex
    code[0x60:0x68] = exefs_shaders.EXIT
    code[0x68:0x70] = b"\xef\xbe\xad\xde\x01\x02\x03\x04"
    payload = bytes(0x40) + bytes(ctrl) + bytes(0x80) + bytes(code) + bytes(0x40)
    out = os.path.join(ROOT, "private", "_selftest_p5s")
    ok = True
    for label, buf in (("stored", payload), ("lz4", lz4_block_roundtrip(payload))):
        ex = Extractor(os.path.join(out, label), dry=False)
        n = ex.scan("t", buf)
        print(f"selftest {label}: {n} block(s), stages {dict(ex.stages)}")
        ok &= n == 1 and ex.stages == {"vertex": 1}
    sys.exit(0 if ok else 1)


def lz4_block_roundtrip(payload):
    import lz4.block
    comp = lz4.block.compress(payload, store_size=False)
    return lz4_block(comp, len(payload))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--romfs", help="directory with the RomFS files of the game (extracted from your own dump)")
    ap.add_argument("--out")
    ap.add_argument("--grep", default=DEFAULT_GREP, help="regex on RomFS-relative paths to scan")
    ap.add_argument("--dry-run", action="store_true", help="write nothing, only the summary")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
    if not a.romfs or (not a.out and not a.dry_run):
        ap.error("--romfs and --out are required (--out optional with --dry-run)")
    out = os.path.normpath(a.out or os.path.join(ROOT, "private", "_dry"))
    if not os.path.realpath(out).startswith(os.path.realpath(os.path.join(ROOT, "private")) + os.sep):
        sys.exit(f"refusing to write outside private/: {out}")
    ex = Extractor(out, a.dry_run)
    ex.walk(a.romfs, a.grep)
    text = ex.report()
    print(text)
    if not a.dry_run:
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, "report.txt"), "w").write(text + "\n")


if __name__ == "__main__":
    main()
