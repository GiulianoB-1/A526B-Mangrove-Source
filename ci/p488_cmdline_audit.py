#!/usr/bin/env python3
"""P488 review: source/config capacity, not an ABL or hardware proof."""
import pathlib
import re
import json
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
header = (root / "arch/arm64/include/uapi/asm/setup.h").read_text()
limit = int(re.search(r"^#define\s+COMMAND_LINE_SIZE\s+(\d+)", header, re.M)[1])
cfg = (root / "ci/build.config").read_text()
prefix = re.search(r'^CONFIG_CMDLINE="([^"\n]*)"$', cfg, re.M)[1]
assert "CONFIG_CMDLINE_EXTEND=y" in cfg.splitlines()
# The evidence pack reports 368 bytes for the longer diagnostic prefix.
# Use the larger of that evidence and the actual pinned source prefix.
budget_prefix = max(len(prefix.encode()), 368)
header_bytes, abl_bytes = 511, 2066
# Two separating spaces and the terminating NUL.
required = budget_prefix + header_bytes + abl_bytes + 3
assert limit == 4096 and required <= limit, (limit, required)
# Inventory references for human review, including the actual source tree.
inventory = {}
for term in ("COMMAND_LINE_SIZE", "boot_command_line", "saved_command_line", "bootargs"):
    out = subprocess.check_output(["git", "grep", "-l", "-F", term, "--", "*.c", "*.h", "*.S", "*.py", "*.sh", "*.yml"], cwd=root, text=True)
    inventory[term] = out.splitlines()
result = dict(limit=limit, actual_prefix_bytes=len(prefix.encode()), budget_prefix_bytes=budget_prefix,
              abl_header_bytes=header_bytes, abl_added_bytes=abl_bytes,
              required_with_nul=required, margin=limit-required, references=inventory,
              evidence="sanitized TG capture and reconstructed Mangrove; not a live Mangrove proof")
print(json.dumps(result, indent=2))
