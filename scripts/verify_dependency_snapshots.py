"""Verify the repository-owned OCIO and shaderc SDK snapshots without loading DLLs."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
errors = []
for name in ("OpenColorIO", "Shaderc"):
    directory = root / "Dependence" / name
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    listed = set()
    for entry in manifest["files"]:
        relative = Path(entry["path"])
        if relative.is_absolute() or ".." in relative.parts:
            errors.append(f"{name}: invalid inventory path {relative}")
            continue
        path = directory / relative
        listed.add(relative.as_posix())
        if not path.is_file():
            errors.append(f"Missing {path}")
            continue
        data = path.read_bytes()
        if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            errors.append(f"Snapshot mismatch: {path}")
    actual = {path.relative_to(directory).as_posix() for path in directory.rglob("*")
              if path.is_file() and path.name != "manifest.json"}
    for extra in sorted(actual - listed):
        errors.append(f"Unlisted snapshot file: {name}/{extra}")
    print(f"{name}: checked {len(listed)} files")
if errors:
    raise SystemExit("\n".join(errors))
print("Dependency snapshots match their inventories.")
