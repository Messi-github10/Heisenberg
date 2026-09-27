#!/usr/bin/env python3
"""Check source-level working-image rules shared by all filter shaders."""

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shader-dir", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    entries = manifest.get("shaders", [])
    sources = [entry["source"] for entry in entries]
    if len(sources) != len(set(sources)):
        raise RuntimeError("shader manifest contains duplicate source names")

    for source_name in sources:
        source_path = args.shader_dir / source_name
        text = source_path.read_text(encoding="utf-8")
        if "#version 450" not in text:
            raise RuntimeError(f"{source_name}: missing GLSL 450 declaration")
        if "local_size_x = 16" not in text or "local_size_y = 16" not in text:
            raise RuntimeError(f"{source_name}: unexpected workgroup declaration")
        if "rgba8" in text:
            raise RuntimeError(f"{source_name}: rgba8 violates the working image contract")
        if "rgba16f" not in text and "histogram" not in source_name:
            raise RuntimeError(f"{source_name}: missing rgba16f image contract")

    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text(
        f"checked {len(sources)} shader sources\n", encoding="ascii")
    print("Passed shader source contract tests.")


if __name__ == "__main__":
    main()
