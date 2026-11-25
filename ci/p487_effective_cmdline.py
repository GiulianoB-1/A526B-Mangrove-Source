#!/usr/bin/env python3
"""Samsung ABL primary cmdline only; fail closed for the exact pinned template."""
import json
from pathlib import Path
from p480_boot_repack import parse_boot

def require(ok, why):
    if not ok:
        raise SystemExit("A52 effective cmdline BLOCKED: " + why)

def effective_cmdline(source, config, required):
    before = parse_boot(source)
    pin = json.loads(Path("ci/P479-boot-provenance.json").read_text())["cmdline"]
    require(before["cmdline"] == pin, "unrecognized source cmdline")
    require(len(pin.encode("ascii")) == 548, "template size")
    primary = source[64:576]
    require(primary[:511] == pin.encode("ascii")[:511], "unexpected primary bytes")
    # ABL uses 511 bytes even if the AOSP field has all 512 bytes nonzero.
    old = primary[:511].split(b"\0", 1)[0].decode("ascii")
    require(old.endswith(" printk.time=1 watchdo"), "unexpected effective tail")
    require("CONFIG_PRINTK_TIME=y" in config, "timestamp default not proven")
    require("CONFIG_CMDLINE_EXTEND=y" in config, "built-in prefix not proven")
    builtin = [x for x in config if x.startswith("CONFIG_CMDLINE=")]
    require(len(builtin) == 1 and "no_console_suspend" in builtin[0].split('"')[1].split(),
            "duplicate no_console_suspend not proven")
    tokens = old.split()
    require(tokens.count("no_console_suspend") == tokens.count("printk.time=1") == 1,
            "unexpected duplicates")
    tokens = [t for t in tokens if t not in ("no_console_suspend","printk.time=1","watchdo")]
    require(all(t.split("=")[0] not in {x.split("=")[0] for x in tokens} for t in required),
            "duplicate override")
    new = " ".join(tokens + required)
    data = new.encode("ascii")
    require(len(data) <= 511, "effective length overflow")
    result = bytearray(source)
    result[64:576] = data.ljust(512, b"\0")
    result[608:1632] = b"\0" * 1024
    require(result[:64] == source[:64] and result[576:608] == source[576:608] and
            result[1632:] == source[1632:], "outside cmdline changed")
    actual = bytes(result[64:575]).split(b"\0", 1)[0].decode("ascii")
    require(actual == new and all(actual.split().count(t) == 1 for t in required),
            "required token not effective")
    require(parse_boot(result)["cmdline"] == actual, "extra cmdline unexpectedly present")
    return bytes(result), actual, {
        "printk.time=1": "CONFIG_PRINTK_TIME=y",
        "no_console_suspend": "same token in CONFIG_CMDLINE prefix under EXTEND",
        "watchdo": "truncated fragment of watchdog.handle_boot_enabled=1 at ABL boundary",
        "watchdog.handle_boot_enabled=1": "never delivered as complete token",
        "cpuidle.off=1": "outside ABL 511-byte limit"
    }
