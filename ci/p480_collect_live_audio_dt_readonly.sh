#!/system/bin/sh
# P480 r02 audio DT evidence collector for Android or Termux.
# Read-only: no flash, no debugfs, no root escalation and no kernel writes.
# Use on the currently booted known-good TouchGrass or an authorized image.
#
# Usage:
#   sh p480_collect_live_audio_dt_readonly.sh
#   sh p480_collect_live_audio_dt_readonly.sh /sdcard/Download/p480-audio-dt
#
# Does not establish driver binding or approve flashing.

set -u
if [ "$#" -gt 0 ]; then
  OUT="$1"
else
  OUT="$PWD/p480-live-audio-dt-$(date -u +%Y%m%dT%H%M%SZ)"
fi
case "$OUT" in
  /proc|/proc/*|/sys|/sys/*|/dev|/dev/*)
    echo "Refusing output inside a kernel interface: $OUT" >&2
    exit 1
    ;;
esac
if [ -e "$OUT" ]; then
  echo "Output already exists; choose a new directory: $OUT" >&2
  exit 1
fi
mkdir -p "$OUT" || exit 1

# This exact node path is known from the pinned Phase446WX base DTB.
# Do not recursively scan device tree or touch MMIO/debugfs.
NODE=""
ROOT=""
for candidate in /proc/device-tree /sys/firmware/devicetree/base; do
  test -d "$candidate" || continue
  ROOT="$candidate"
  target="$candidate/soc/qcom,msm-audio-apr/qcom,q6core-audio/sound"
  if [ -d "$target" ]; then
    NODE="$target"
    break
  fi
done

capture_strings() {
  prop="$1"
  dest="$2"
  if [ -n "$NODE" ] && [ -r "$NODE/$prop" ]; then
    # Device-tree string properties use NUL separators.
    tr '\000' '\n' < "$NODE/$prop" > "$OUT/$dest" ||
      echo "ERROR reading $prop" > "$OUT/$dest"
  else
    echo "Unavailable or permission denied: $prop" > "$OUT/$dest"
  fi
}

capture_strings "qcom,model" "qcom-model.txt"
capture_strings "qcom,audio-routing" "qcom-audio-routing.txt"
capture_strings "compatible" "compatible.txt"
capture_strings "status" "status.txt"
capture_strings "asoc-platform-names" "asoc-platform-names.txt"
capture_strings "asoc-cpu-names" "asoc-cpu-names.txt"
capture_strings "asoc-codec-names" "asoc-codec-names.txt"

if [ -n "$ROOT" ] && [ -r "$ROOT/model" ]; then
  tr '\000' '\n' < "$ROOT/model" > "$OUT/board-model.txt" ||
    echo "Board model unreadable" > "$OUT/board-model.txt"
else
  echo "Board model unavailable" > "$OUT/board-model.txt"
fi

# Collect no personal identifiers, boot partitions, kernel logs, or dumps.
if command -v getprop >/dev/null 2>&1; then
  {
    echo "ro.product.device=$(getprop ro.product.device)"
    echo "ro.boot.hardware=$(getprop ro.boot.hardware)"
    echo "ro.build.version.release=$(getprop ro.build.version.release)"
  } > "$OUT/android-general.txt"
fi

{
  echo "P480 READ-ONLY LIVE AUDIO DEVICE TREE"
  echo "No flashing, root escalation, kernel writes, or debugfs reads"
  echo "DT root: $ROOT"
  echo "Sound node: $NODE"
  echo "Observed card model:"
  cat "$OUT/qcom-model.txt"
  echo "Observed route-string count:"
  if [ -n "$NODE" ] && [ -r "$NODE/qcom,audio-routing" ]; then
    wc -l < "$OUT/qcom-audio-routing.txt"
  else
    echo "unavailable"
  fi
  echo "Pinned TG A52XQ r02 SOURCE has card lito-lagoonmtp-snd-card"
  echo "and 54 final routing strings (27 pairs) in fragment@126."
  echo "Offline comparison of ALL 27 route pairs is still necessary."
  echo "This observation does not authorize flashing or prove A3c audio."
} > "$OUT/SUMMARY.txt"

if command -v tar >/dev/null 2>&1; then
  PARENT="$(dirname "$OUT")"
  LEAF="$(basename "$OUT")"
  (cd "$PARENT" && tar -czf "$LEAF.tar.gz" "$LEAF") 2>/dev/null ||
    echo "Optional tar archive skipped" >&2
fi

echo "Read-only live audio DT evidence saved to $OUT"
echo "Share SUMMARY.txt and qcom-audio-routing.txt or the tar.gz archive."
