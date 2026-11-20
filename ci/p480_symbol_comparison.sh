#!/usr/bin/env bash
# P480 CI reviewer evidence, no flashing and no source modifications.
# Build the untouched p479-baseline in this current native CI environment.
# Compare actual linked vmlinux symbols using Linux scripts/bloat-o-meter.
set -Eeuo pipefail

report_dir="$PWD/package/symbol-review"
mkdir -p "$report_dir"
echo 'NO FLASH: symbol-size comparison is not a semantic-equivalence proof.' > "$report_dir/README-NO-FLASH.txt"

# The three P480/repaired sources were compiled earlier by this same job.
for variant in CONTROL NO-A3C A3C; do
  test -s "out_$variant/vmlinux" || {
    echo "::error::Missing $variant vmlinux; source comparison cannot proceed"
    exit 1
  }
  test -s "out_$variant/System.map"
  test -s "out_$variant/.config"
done
test -s scripts/bloat-o-meter
test "$(git rev-list -n1 p479-baseline)" = a040e6cf1e7232fc3e91516416fb7fffa3fc9025
git merge-base --is-ancestor 55babd9c651375580b3c5c10d7f37ac6213e005c HEAD

# Pure, unmodified baseline at its immutable tag. Never apply the
# 55babd9 PIL/SSR header fix to this worktree.
base="$RUNNER_TEMP/p479-pristine-$(printf %s "$GITHUB_RUN_ID")"
git worktree add --detach "$base" a040e6cf1e7232fc3e91516416fb7fffa3fc9025
test "$(git -C "$base" rev-parse HEAD)" = a040e6cf1e7232fc3e91516416fb7fffa3fc9025
grep -Fq '#include <soc/qcom/subsystem_restart.h>' "$base/drivers/a52_pil/subsys-pil-tz.c"
grep -Fq '#include "../../a52-compat/include/soc/qcom/subsystem_restart.h"' \
  "$RUNNER_TEMP/p479-control/drivers/a52_pil/subsys-pil-tz.c"

mkdir -p out_BASELINE
cp "$base/ci/build.config" out_BASELINE/.config
echo "P479 BASELINE immutable source $(git -C "$base" rev-parse HEAD)"
echo "P479 BASELINE config before olddefconfig $(sha256sum out_BASELINE/.config)"
make -C "$base" O="$PWD/out_BASELINE" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LLVM=1 LLVM_IAS=1 olddefconfig

# Build the pure baseline with only olddefconfig's generated adjustments.
# Persist the true diff, rather than changing the immutable source.
diff -u "$base/ci/build.config" out_BASELINE/.config \
  > "$report_dir/A-P479-original-config-vs-pure-baseline-built.diff" || test "$?" -eq 1
echo 'Building immutable P479 baseline with unmodified source.'
make -C "$base" O="$PWD/out_BASELINE" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LLVM=1 LLVM_IAS=1 -j"$(nproc)" Image vmlinux

test -s out_BASELINE/vmlinux
test -s out_BASELINE/System.map
test -s out_BASELINE/.config
grep -aFq 'P479 H4 CABLE type=' out_BASELINE/arch/arm64/boot/Image
grep -Eq '[[:space:]]p479_h4_tick([.][^[:space:]]+)?$' out_BASELINE/System.map
git -C "$base" diff --quiet
git -C "$base" diff --cached --quiet

# Preserve the FULL diffs, not excerpts.
python3 - "$report_dir" <<'PY'
import difflib,hashlib,json,os,re,subprocess,sys
from pathlib import Path

r=Path(sys.argv[1])
base=Path(os.environ["RUNNER_TEMP"])/("p479-pristine-"+os.environ["GITHUB_RUN_ID"])
committed=base/"ci/build.config"
pairs={
    "A-P479-original-vs-P479-baseline-built":(committed,Path("out_BASELINE/.config")),
    "B-P479-original-vs-55babd9":(committed,Path("out_CONTROL/.config")),
    "B-P479-baseline-built-vs-55babd9":(Path("out_BASELINE/.config"),Path("out_CONTROL/.config")),
    "C-55babd9-vs-P480-NO-A3C":(Path("out_CONTROL/.config"),Path("out_NO-A3C/.config")),
    "D-P480-NO-A3C-vs-P480-A3C":(Path("out_NO-A3C/.config"),Path("out_A3C/.config")),
}
config={}
for name,(before,after) in pairs.items():
    a=before.read_text().splitlines(keepends=True)
    b=after.read_text().splitlines(keepends=True)
    diff="".join(difflib.unified_diff(a,b,fromfile=str(before),tofile=str(after)))
    (r/(name+".config.diff")).write_text(diff if diff else "# No config differences\n")
    config[name]={
        "same_bytes":a==b,"diff_line_count":len(diff.splitlines()) if diff else 0,
        "old_sha256":hashlib.sha256(before.read_bytes()).hexdigest(),
        "new_sha256":hashlib.sha256(after.read_bytes()).hexdigest(),
    }
# Exact expected committed diff, using full ordered lines, not a set diff.
lines=committed.read_text().splitlines()
expected=lines.copy()
expected.insert(expected.index("CONFIG_CC_HAS_ASM_GOTO=y"),"CONFIG_CC_CAN_LINK=y")
expected.insert(expected.index("CONFIG_CC_HAS_ASM_GOTO=y"),"CONFIG_CC_CAN_LINK_STATIC=y")
expected.insert(expected.index("CONFIG_LOCALVERSION=\"-Mangrove\""),"# CONFIG_UAPI_HEADER_TEST is not set")
expected="\n".join(expected)+"\n"
config["expected_only_three_generated_flags"]=(
    Path("out_CONTROL/.config").read_text()==expected
)
config["pure_baseline_has_same_three_only"]=(
    Path("out_BASELINE/.config").read_text()==expected
)
no_a3c=Path("out_NO-A3C/.config").read_text()
a3c=Path("out_A3C/.config").read_text()
config["55babd9_vs_no_a3c_exact"]=(
    Path("out_CONTROL/.config").read_text()==no_a3c
)
config["a3c_is_only_config_switch"]=(
    a3c.replace("CONFIG_P480_A3C=y\n","# CONFIG_P480_A3C is not set\n")==no_a3c
)
ref_map="26ae9bda166d764bf695d6c51b8799632bc6f1ea276747a9f8fd1d0caf6d3a3e"
def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
maps={}
for name,folder in [
    ("P479_BASELINE","out_BASELINE"),("P479_55babd9","out_CONTROL"),
    ("P480_NO_A3C","out_NO-A3C"),("P480_A3C","out_A3C")]:
    maps[name]={
        "System_map_sha256":sha(f"{folder}/System.map"),
        "matches_original_P479_System_map":sha(f"{folder}/System.map")==ref_map,
        "vmlinux_sha256":sha(f"{folder}/vmlinux"),
        "Image_sha256":sha(f"{folder}/arch/arm64/boot/Image"),
    }
(r/"P479-config-and-System-map.json").write_text(json.dumps({
    "original_P479_System_map_sha256":ref_map,
    "comparison_configurations":config,
    "four_linked_builds":maps,
    "unmodified_baseline_source":"a040e6cf1e7232fc3e91516416fb7fffa3fc9025",
    "repair_source":"55babd9c651375580b3c5c10d7f37ac6213e005c",
    "PIL_SSR_header_fix_kept":True,
    "flash_authorized":False
},indent=2,sort_keys=True)+"\n")
print("P479 original System.map SHA256:",ref_map)
print("Built pure baseline System.map SHA256:",maps["P479_BASELINE"]["System_map_sha256"])
print("Does pure baseline match original P479:",maps["P479_BASELINE"]["matches_original_P479_System_map"])
print("Expected three config flags only:",config["expected_only_three_generated_flags"])
print("Pure baseline olddefconfig three flags only:",config["pure_baseline_has_same_three_only"])
print("55babd9 versus NO-A3C config identical:",config["55babd9_vs_no_a3c_exact"])
print("A3C only Kconfig switch:",config["a3c_is_only_config_switch"])
PY

# Bloat-o-meter reads linked vmlinux symbol sizes using nm. -c separates
# text, data and read-only data. These files contain the FULL output.
python3 scripts/bloat-o-meter -c out_BASELINE/vmlinux out_CONTROL/vmlinux \
  > "$report_dir/B-P479-baseline-vs-55babd9.bloat.txt"
python3 scripts/bloat-o-meter -c out_CONTROL/vmlinux out_NO-A3C/vmlinux \
  > "$report_dir/C-55babd9-vs-P480-NO-A3C.bloat.txt"
python3 scripts/bloat-o-meter -c out_NO-A3C/vmlinux out_A3C/vmlinux \
  > "$report_dir/D-P480-NO-A3C-vs-P480-A3C.bloat.txt"

# Classify the observed bloat output conservatively. Being size-identical
# cannot prove unchanged code bytes or runtime behavior.
python3 - "$report_dir" <<'PY'
import json,re,sys
from pathlib import Path
r=Path(sys.argv[1])
files={
"B":r/"B-P479-baseline-vs-55babd9.bloat.txt",
"C":r/"C-55babd9-vs-P480-NO-A3C.bloat.txt",
"D":r/"D-P480-NO-A3C-vs-P480-A3C.bloat.txt",
}
result={}
for kind,p in files.items():
    sections={"Function":[],"Data":[],"RO Data":[]}
    section=None
    for line in p.read_text().splitlines():
        h=re.match(r"^(Function|Data|RO Data)\s+old\s+new\s+delta\s*$",line)
        if h:section=h.group(1);continue
        if line.startswith("Total:") or line.startswith("add/remove:"):
            continue
        if section and line.strip():
            tokens=line.split()
            if len(tokens)==4 and re.fullmatch(r"[+-]\d+",tokens[3]):
                sections[section].append({
                    "symbol":tokens[0],"before":tokens[1],
                    "after":tokens[2],"size_delta_bytes":int(tokens[3])
                })
    changed=sum(len(v) for v in sections.values())
    result[kind]={
        "comparison":{
           "B":"pure baseline vs repaired 55babd9",
           "C":"repaired 55babd9 vs P480 NO-A3C",
           "D":"P480 NO-A3C vs P480 A3C"}[kind],
        "changed_symbol_sizes":changed,
        "sections":{k:len(v) for k,v in sections.items()},
        "symbols":sections,
        "interpretation":"NEEDS_SOURCE_REVIEW" if kind in ("B","D") else (
            "SIZE_IDENTICAL_NOT_SEMANTIC_PROOF" if changed==0 else "UNEXPECTED_SYMBOL_SIZE_DIFFERENCES"),
        "flash_authorized":False,
    }
(r/"P480-bloat-classification.json").write_text(json.dumps(result,indent=2,sort_keys=True)+"\n")
print("Three required bloat-o-meter comparisons:")
for k,v in result.items():
    print(k,v["comparison"],"changed-sized symbols",v["changed_symbol_sizes"],
          "by section",v["sections"],v["interpretation"])
with open(__import__('os').environ["GITHUB_STEP_SUMMARY"],"a") as s:
    s.write("\n### P480 four-source bloat-o-meter report (NO FLASH)\n")
    s.write("Original P479 equivalence already accepted separately; these reports analyze linked symbols.\n\n")
    s.write("| Comparison | Changed-size symbols | Interpretation |\n|---|---:|---|\n")
    for k,v in result.items():
        s.write(f"| {k}: {v['comparison']} | {v['changed_symbol_sizes']} | {v['interpretation']} |\n")
    s.write("\nAll raw outputs and the full .config diffs are uploaded for Claude review. NO FLASH.\n")
PY

(cd "$report_dir" && sha256sum *.txt *.json *.diff > SHA256SUMS && sha256sum -c SHA256SUMS)
echo 'All three full Linux bloat-o-meter reports generated. NO FLASH.' >> "$GITHUB_STEP_SUMMARY"
