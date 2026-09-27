import argparse
import json
import subprocess
from pathlib import Path


def resource_kind(resource_type):
    return {
        "images": "storage_image",
        "textures": "sampled_image",
        "sampled_images": "sampled_image",
        "separate_images": "separate_image",
        "separate_samplers": "separate_sampler",
        "ubos": "ubo",
        "ssbos": "ssbo",
    }[resource_type]


def reflected_resources(reflection):
    result = []
    for key in ("images", "textures", "sampled_images", "separate_images",
                "separate_samplers", "ubos", "ssbos"):
        kind = resource_kind(key)
        for resource in reflection.get(key, []):
            item = dict(resource)
            item["kind"] = kind
            if key == "images":
                if resource.get("readonly"):
                    item["access"] = "read"
                elif resource.get("writeonly"):
                    item["access"] = "write"
                else:
                    item["access"] = "readwrite"
            result.append(item)
    return result


def by_binding(resources):
    return {(item.get("set", 0), item["binding"]): item for item in resources}


def validate_shader(entry, spirv_cross, shader_dir):
    spirv = shader_dir / entry["spirv"]
    if not spirv.is_file():
        raise RuntimeError(f"missing SPIR-V: {spirv}")
    completed = subprocess.run(
        [str(spirv_cross), str(spirv), "--reflect"],
        check=True, capture_output=True, text=True,
    )
    reflection = json.loads(completed.stdout)
    actual = by_binding(reflected_resources(reflection))
    expected = by_binding(entry["resources"])
    if set(actual) != set(expected):
        raise RuntimeError(
            f"{entry['source']}: descriptor bindings differ; "
            f"expected {sorted(expected)}, reflected {sorted(actual)}")

    types = reflection.get("types", {})
    for key, wanted in expected.items():
        found = actual[key]
        label = f"{entry['source']} binding {key[1]}"
        for field in ("name", "kind"):
            if found.get(field) != wanted.get(field):
                raise RuntimeError(
                    f"{label}: {field} is {found.get(field)!r}, "
                    f"expected {wanted.get(field)!r}")
        for field in ("set", "format", "access"):
            if field in wanted and found.get(field) != wanted[field]:
                raise RuntimeError(
                    f"{label}: {field} is {found.get(field)!r}, "
                    f"expected {wanted[field]!r}")
        if wanted.get("kind") in ("ubo", "ssbo"):
            if found.get("block_size") != wanted.get("block_size"):
                raise RuntimeError(
                    f"{label}: block_size is {found.get('block_size')}, "
                    f"expected {wanted.get('block_size')}")
            reflected_type = types.get(found.get("type"), {})
            members = {member["name"]: member for member in reflected_type.get("members", [])}
            for member_name, offset in wanted.get("members", {}).items():
                if member_name not in members or members[member_name].get("offset") != offset:
                    raise RuntimeError(
                        f"{label}: member {member_name!r} offset mismatch")
            if "array_stride" in wanted:
                member = members.get("bins")
                if not member or member.get("array_stride") != wanted["array_stride"]:
                    raise RuntimeError(f"{label}: array stride mismatch")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spirv-cross", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--shader-dir", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("version") != 1:
        raise RuntimeError("unsupported shader manifest version")
    for entry in manifest.get("shaders", []):
        validate_shader(entry, args.spirv_cross, args.shader_dir)
    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text(
        f"validated {len(manifest.get('shaders', []))} shaders\n",
        encoding="ascii",
    )
    print(f"Validated {len(manifest.get('shaders', []))} Heisenberg compute shaders.")


if __name__ == "__main__":
    main()
