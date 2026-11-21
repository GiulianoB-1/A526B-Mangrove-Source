#!/usr/bin/env python3
"""P480 A3c: inspect the exact embedded WX boot DTB, read-only.

This examines Android boot-v2's *actual* DTB, including concatenated
flattened device trees. It compares the enabled Kona sound card's DT
platform mappings to the selected A3c FE/BE providers, using only
Python's standard library. It does not modify DTB/DTBO or boot bytes.

A match means the DTS-side binding prerequisites are present.
It DOES NOT prove Linux driver binding or working audio on hardware.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

FDT_MAGIC = b"\xd0\x0d\xfe\xed"
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


def be32(data: bytes, pos: int) -> int:
    if pos < 0 or pos + 4 > len(data):
        raise ValueError("FDT u32 out of bounds")
    return struct.unpack_from(">I", data, pos)[0]


def align4(n: int) -> int:
    return (n + 3) & ~3


def strings(data: bytes) -> list[str]:
    if not data:
        return []
    # FDT string-lists use NUL-delimited strings. Reject truncated lists.
    if data[-1] != 0:
        raise ValueError("unterminated DT string list")
    return [part.decode("utf-8") for part in data[:-1].split(b"\x00")]


def u32s(data: bytes) -> list[int]:
    if len(data) % 4:
        raise ValueError("misaligned phandle/u32 array")
    return list(struct.unpack(">" + "I" * (len(data) // 4), data))


def parse_fdt(blob: bytes, offset: int) -> dict:
    if offset + 40 > len(blob) or blob[offset:offset + 4] != FDT_MAGIC:
        raise ValueError("missing/short FDT header")
    length = be32(blob, offset + 4)
    off_struct = be32(blob, offset + 8)
    off_strings = be32(blob, offset + 12)
    version = be32(blob, offset + 20)
    size_strings = be32(blob, offset + 32)
    size_struct = be32(blob, offset + 36)
    if not (16 <= version <= 21):
        raise ValueError("unsupported FDT header version")
    if not (40 <= length <= len(blob) - offset):
        raise ValueError("bad FDT totalsize")
    if off_struct + size_struct > length or off_strings + size_strings > length:
        raise ValueError("FDT section extends past totalsize")
    dt = blob[offset:offset + length]
    names = dt[off_strings:off_strings + size_strings]
    pos = off_struct
    end = off_struct + size_struct
    stack: list[str] = []
    nodes: list[dict] = []
    current: dict | None = None
    finished = False
    while pos + 4 <= end:
        token = be32(dt, pos)
        pos += 4
        if token == FDT_BEGIN_NODE:
            if b"\0" not in dt[pos:end]:
                raise ValueError("unterminated FDT node")
            last = dt.index(b"\0", pos, end)
            name = dt[pos:last].decode("utf-8")
            pos = align4(last + 1)
            stack.append(name)
            path = "/" + "/".join(filter(None, stack))
            current = {"path": path, "props": {}}
            nodes.append(current)
        elif token == FDT_END_NODE:
            if not stack:
                raise ValueError("unbalanced FDT nodes")
            stack.pop()
            current = nodes[-1] if False else None
            # Find the parent by path, not the preceding traversal node.
            if stack:
                parent = "/" + "/".join(filter(None, stack))
                current = next((n for n in reversed(nodes)
                                if n["path"] == parent), None)
        elif token == FDT_PROP:
            if not stack or pos + 8 > end:
                raise ValueError("FDT property out of structure")
            size = be32(dt, pos)
            name_off = be32(dt, pos + 4)
            pos += 8
            if pos + size > end or name_off >= len(names):
                raise ValueError("FDT property outside sections")
            nend = names.find(b"\0", name_off)
            if nend < 0:
                raise ValueError("FDT property name unterminated")
            key = names[name_off:nend].decode("ascii")
            target = current
            if target is None:
                raise ValueError("FDT property without node")
            target["props"][key] = dt[pos:pos + size]
            pos = align4(pos + size)
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            finished = True
            break
        else:
            raise ValueError(f"unexpected FDT token {token}")
    if not finished or stack:
        raise ValueError("incomplete FDT")
    return {"offset": offset, "length": length, "nodes": nodes}


def boot_trees(blob: bytes) -> list[dict]:
    trees: list[dict] = []
    pos = 0
    while True:
        pos = blob.find(FDT_MAGIC, pos)
        if pos < 0:
            break
        try:
            tree = parse_fdt(blob, pos)
        except (ValueError, UnicodeError):
            pos += 4
            continue
        trees.append(tree)
        pos += tree["length"]
    if not trees:
        raise ValueError("no valid, complete FDT in boot-final.dtb")
    return trees


def inspect(tree: dict, required: list[str]) -> list[dict]:
    phandles = {}
    for node in tree["nodes"]:
        for name in ("phandle", "linux,phandle"):
            if name in node["props"] and len(node["props"][name]) == 4:
                value = be32(node["props"][name], 0)
                if value:
                    phandles[value] = node["path"]

    rows = []
    for node in tree["nodes"]:
        p = node["props"]
        compatible = strings(p["compatible"]) if "compatible" in p else []
        if not any(c in ("qcom,kona-asoc-snd",
                         "qcom,kona-asoc-snd-stub") for c in compatible):
            continue
        platform_names = strings(p.get("asoc-platform-names", b""))
        phandle_refs = u32s(p.get("asoc-platform", b""))
        required_refs = {}
        for name in required:
            try:
                index = platform_names.index(name)
                value = phandle_refs[index] if index < len(phandle_refs) else 0
            except ValueError:
                index, value = -1, 0
            required_refs[name] = {
                "index": index,
                "phandle": value,
                "target": phandles.get(value),
                "present": index >= 0 and bool(value) and value in phandles,
            }
        routes = strings(p.get("qcom,audio-routing", b""))
        model = strings(p.get("qcom,model", b""))
        status = strings(p.get("status", b""))
        active = not status or status[0] in ("okay", "ok")
        checks = {
            "compatible_real_kona": "qcom,kona-asoc-snd" in compatible,
            "enabled_sound_card_node": active,
            "exact_A526B_card_model": model == ["lito-lagoonmtp-snd-card"],
            "audio_routing_nonempty_even_pairs": bool(routes) and len(routes) % 2 == 0,
            "platform_name_phandle_lists_aligned": len(platform_names) == len(phandle_refs),
            "selected_29_FE_BE_platforms_all_resolve": all(
                v["present"] for v in required_refs.values()),
        }
        rows.append({
            "tree_offset": tree["offset"],
            "path": node["path"],
            "compatible": compatible,
            "status": status[0] if status else "okay (default)",
            "model": model,
            "audio_routing_string_count": len(routes),
            "platform_mapping_count": len(platform_names),
            "required_platform_names": required_refs,
            "checks": checks,
            "prerequisites_pass": all(checks.values()),
        })
    return rows


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    selected = manifest["selected_link_ids"]
    if len(selected) != 29:
        raise SystemExit("P480 DT audit: expected 29 pinned native FE/BE links")
    required = sorted({name for entry in selected
                       for name in entry["platforms"]})
    data = args.dtb.read_bytes()
    trees = boot_trees(data)
    records = [row for tree in trees for row in inspect(tree, required)]
    success = any(row["prerequisites_pass"] for row in records)
    report = {
        "schema": "P480-A3c-embedded-WX-DTB-source-contract-v1",
        "dtb_sha256": hashlib.sha256(data).hexdigest(),
        "dtb_bytes": len(data),
        "fdt_tree_count": len(trees),
        "selected_dai_link_count": len(selected),
        "required_platform_names": required,
        "card_candidates": records,
        "status": "DT_SOUND_BINDING_PREREQUISITES_PRESENT" if success
                  else "DT_SOUND_BINDING_PREREQUISITES_FAILED",
        "hardware_validated": False,
        "flash_authorized": False,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"P480 A3c real WX DTB: {len(trees)} trees, "
          f"{len(records)} Kona card candidates, "
          f"{len(required)} selected platform names, "
          f"preflight={'PASS' if success else 'FAIL'}")
    for row in records:
        print(f"  {row['path']} {row['model']} status={row['status']} "
              f"checks={row['checks']}")
    if not success:
        raise SystemExit("P480 A3c DTB mismatch: card cannot be claimed "
                         "bind-ready; see reported node properties")
    print("P480 A3c DTB matching PASS: ONLY source-side prerequisites; "
          "hardware audio untested; NO FLASH")


if __name__ == "__main__":
    main()
