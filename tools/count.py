#!/usr/bin/env python3
"""AxiomOS source line counter.

Counts physical lines of the source tree, grouped by language and by directory,
so the "115k lines" target can be tracked turn by turn.  Build output, VM images,
tool caches and third-party toolchains are excluded.

Usage:
    python tools/count.py            # summary table
    python tools/count.py --detail   # per-file ranking
    python tools/count.py --json     # machine readable
"""

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXCLUDE_DIRS = {
    ".git", "build", "vm", ".toolchain", "target", "node_modules",
    "__pycache__", ".skills", "dist", "out",
}

LANG_BY_EXT = {
    ".c": "C",
    ".h": "Header",
    ".asm": "NASM",
    ".S": "Assembly",
    ".rs": "Rust",
    ".ld": "Linker",
    ".py": "Python",
    ".ps1": "PowerShell",
    ".sh": "Shell",
    ".md": "Docs",
    ".toml": "Config",
    ".json": "Config",
    ".nsh": "Config",
    ".txt": "Docs",
}

# Files larger than this are treated as binary blobs and skipped.
MAX_TEXT_BYTES = 2 * 1024 * 1024

# Binary-ish extensions that must never count as source.
BINARY_EXT = {
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".efi", ".elf", ".img",
    ".iso", ".vhd", ".vmdk", ".exe", ".o", ".a", ".rlib", ".zip", ".gz",
    ".tar", ".7z", ".pdf", ".woff", ".ttf", ".bin",
}

CODE_LANGS = {"C", "Header", "NASM", "Assembly", "Rust", "Linker"}
DOC_LANGS = {"Docs", "Markdown"}
TOOL_LANGS = {"Python", "PowerShell", "Shell", "Config"}


def iter_sources(root):
    for base, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for name in files:
            path = os.path.join(base, name)
            ext = os.path.splitext(name)[1]
            low = ext.lower()
            if low in BINARY_EXT:
                continue
            lang = LANG_BY_EXT.get(ext)
            if lang is None:
                continue
            try:
                if os.path.getsize(path) > MAX_TEXT_BYTES:
                    continue
                with open(path, "rb") as fh:
                    data = fh.read()
            except OSError:
                continue
            # Cheap binary sniff: NUL bytes in the first 4 KiB.
            if b"\x00" in data[:4096]:
                continue
            lines = data.count(b"\n")
            if data and not data.endswith(b"\n"):
                lines += 1
            yield path, os.path.relpath(path, root), ext, lang, lines


def top_dir(rel):
    parts = rel.split(os.sep)
    return parts[0] if len(parts) > 1 else "."


def collect(root):
    files = []
    for path, rel, ext, lang, lines in iter_sources(root):
        files.append({
            "path": rel,
            "lang": lang,
            "ext": ext,
            "lines": lines,
            "dir": top_dir(rel),
        })
    return files


def summarize(files):
    by_lang = {}
    by_dir = {}
    for f in files:
        by_lang[f["lang"]] = by_lang.get(f["lang"], 0) + f["lines"]
        d = by_dir.setdefault(f["dir"], {"code": 0, "tools": 0, "docs": 0, "files": 0})
        d["files"] += 1
        if f["lang"] in CODE_LANGS:
            d["code"] += f["lines"]
        elif f["lang"] in DOC_LANGS:
            d["docs"] += f["lines"]
        else:
            d["tools"] += f["lines"]
    code = sum(v for k, v in by_lang.items() if k in CODE_LANGS)
    tools = sum(v for k, v in by_lang.items() if k in TOOL_LANGS)
    docs = sum(v for k, v in by_lang.items() if k in DOC_LANGS)
    total = code + tools + docs
    return by_lang, by_dir, code, tools, docs, total


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--detail", action="store_true", help="rank every file")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--target", type=int, default=115000, help="line target")
    args = ap.parse_args(argv)

    files = collect(ROOT)
    by_lang, by_dir, code, tools, docs, total = summarize(files)

    if args.json:
        print(json.dumps({
            "code": code, "tools": tools, "docs": docs, "total": total,
            "target": args.target,
            "by_language": by_lang,
            "by_directory": by_dir,
            "files": len(files),
        }, indent=2))
        return 0

    print("AxiomOS source census  (root: %s)" % ROOT)
    print()
    print("%-12s %6s %9s" % ("language", "files", "lines"))
    print("%-12s %6s %9s" % ("-" * 12, "-" * 6, "-" * 9))
    for lang in sorted(by_lang, key=lambda k: -by_lang[k]):
        n = sum(1 for f in files if f["lang"] == lang)
        print("%-12s %6d %9d" % (lang, n, by_lang[lang]))
    print("%-12s %6s %9s" % ("", "", "-" * 9))
    print("%-12s %6d %9d" % ("total", len(files), total))
    print()

    print("%-12s %6s %9s %9s" % ("directory", "files", "code", "total"))
    print("%-12s %6s %9s %9s" % ("-" * 12, "-" * 6, "-" * 9, "-" * 9))
    for d in sorted(by_dir, key=lambda k: -(by_dir[k]["code"] + by_dir[k]["tools"] + by_dir[k]["docs"])):
        s = by_dir[d]
        print("%-12s %6d %9d %9d" % (d, s["files"], s["code"], s["code"] + s["tools"] + s["docs"]))
    print()

    print("code %d  tools %d  docs %d  total %d" % (code, tools, docs, total))
    print("target %d -> %d lines missing (%.1f%%)" %
          (args.target, max(0, args.target - code),
           100.0 * code / args.target if args.target else 0.0))

    if args.detail:
        print()
        print("%-58s %6s" % ("file", "lines"))
        for f in sorted(files, key=lambda f: -f["lines"])[:40]:
            print("%-58s %6d" % (f["path"][:58], f["lines"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
