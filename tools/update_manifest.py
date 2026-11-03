#!/usr/bin/env python3
"""Regenerate the release manifest, excluding restored projects and caches."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = ("src/pl/native/", "src/ps/native/")


def release_files():
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel == "manifest_sha256.txt" or rel.startswith(EXCLUDED):
            continue
        if any(p in {".git", "__pycache__", ".Xil"} for p in path.relative_to(ROOT).parts):
            continue
        if path.suffix in {".pyc", ".pyo"}:
            continue
        yield path, rel


def main():
    lines = []
    for path, rel in release_files():
        h = hashlib.sha256()
        with path.open("rb") as stream:
            for data in iter(lambda: stream.read(1024 * 1024), b""):
                h.update(data)
        lines.append(h.hexdigest() + "  " + rel)
    (ROOT / "manifest_sha256.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("Release manifest entries:", len(lines))


if __name__ == "__main__":
    main()
