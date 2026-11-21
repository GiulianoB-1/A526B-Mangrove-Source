#!/system/bin/sh
# P480 A3c hardware evidence collector for Android/Termux.
# READ-ONLY with respect to kernel interfaces: no sysfs/debugfs writes,
# no module insertion, no audio playback/capture, no battery control.
# Writes only to a new user-selected output directory for analysis.
# DO NOT use this script as permission to flash a diagnostic image.
#
# Usage after a separately authorized hardware test:
#   sh p480_collect_audio_readonly.sh
#   sh p480_collect_audio_readonly.sh /sdcard/Download/p480-check
#
# The old P477B build once rebooted on /sys/kernel/debug/gpio reads.
# Consequently this collector never accesses debugfs at all.

set -u

OUT="${1:-${PWD}/p480-a3c-readonly-$(date -u +%Y%m%dT%H%M%SZ)}"
case "$OUT" in
  /sys|/sys/*|/proc|/proc/*|/dev|/dev/*)
    echo "Refusing to write report inside a kernel interface: $OUT" >&2
    exit 1
    ;;
esac
if [ -e "$OUT" ]; then
  echo "Report destination already exists. Use a new directory: $OUT" >&2
  exit 1
fi
mkdir -p "$OUT" || exit 1

# Capture one safe procfs/sysfs-visible file if the kernel permits it.
read_snapshot() {
  name="$1"
  source="$2"
  if [ -r "$source" ]; then
    cat "$source" > "$OUT/$name" 2>&1 || {
      echo "Unable to read $source" > "$OUT/$name"
    }
  else
    echo "Unavailable or permission denied: $source" > "$OUT/$name"
  fi
}

# Capture read-only command output without making a failed command fatal.
run_snapshot() {
  name="$1"
  shift
  "$@" > "$OUT/$name" 2>&1 || {
    echo "The above read-only command returned a nonzero status." >> "$OUT/$name"
  }
}

read_snapshot proc_cmdline.txt /proc/cmdline
read_snapshot proc_version.txt /proc/version
read_snapshot proc_asound_cards.txt /proc/asound/cards
read_snapshot proc_asound_pcm.txt /proc/asound/pcm
read_snapshot proc_asound_devices.txt /proc/asound/devices
read_snapshot proc_asound_modules.txt /proc/asound/modules
run_snapshot uname.txt uname -a

# These are enumeration-only operations. Do not read debugfs GPIO,
# force PM transitions, probe MMIO registers, or enable tracing.
if [ -d /dev/snd ]; then
  run_snapshot dev_snd.txt ls -la /dev/snd
else
  echo "/dev/snd is absent" > "$OUT/dev_snd.txt"
fi
if [ -d /sys/class/sound ]; then
  run_snapshot sys_class_sound.txt ls -la /sys/class/sound
else
  echo "/sys/class/sound is absent" > "$OUT/sys_class_sound.txt"
fi

if command -v getprop >/dev/null 2>&1; then
  {
    echo "ro.product.device: $(getprop ro.product.device)"
    echo "ro.build.version.release: $(getprop ro.build.version.release)"
    echo "ro.build.version.sdk: $(getprop ro.build.version.sdk)"
    echo "ro.boot.hardware: $(getprop ro.boot.hardware)"
  } > "$OUT/android_identity_general.txt"
else
  echo "getprop unavailable" > "$OUT/android_identity_general.txt"
fi

# dmesg may require root. Do not automatically request root privileges.
# Store only filtered audio and boot markers, not the complete system log.
MATCH='P480|P479 H4|A3c|Kona|kona|WCD938|wcd938|BOLERO|bolero|SWR|soundwire|SoundWire|q6asm|q6adm|q6afe|Q6ASM|Q6AFE|LPASS|ADSP|ASoC|snd_soc|msm-pcm|msm-dai'
if command -v dmesg >/dev/null 2>&1; then
  dmesg 2> "$OUT/dmesg_read_error.txt" |
    grep -Ei "$MATCH" > "$OUT/kernel_audio_messages.txt" || true
else
  echo "dmesg unavailable" > "$OUT/dmesg_read_error.txt"
fi

# Android's kernel ring-buffer logcat view is not present on every build.
# -d means dump without clearing; it does not change device operation.
if command -v logcat >/dev/null 2>&1; then
  logcat -d -b kernel -t 6000 2> "$OUT/logcat_kernel_error.txt" |
    grep -Ei "$MATCH" > "$OUT/logcat_audio_messages.txt" || true
else
  echo "logcat unavailable" > "$OUT/logcat_kernel_error.txt"
fi

{
  echo "P480 A3c read-only evidence collection"
  echo "Time (UTC): $(date -u)"
  echo "NO FLASH / NO WRITES TO KERNEL INTERFACES"
  echo "No debugfs access and no automatic root escalation"
  echo ""
  echo "Audio cards (observed, not a hardware PASS):"
  grep -E '^[[:space:]]*[0-9]+[[:space:]]+\[' "$OUT/proc_asound_cards.txt" || true
  echo ""
  echo "PCM entries (observed):"
  grep -Ec '^[0-9]+-[0-9]+:' "$OUT/proc_asound_pcm.txt" || true
  echo ""
  echo "HLT expected: no standalone nohlt token"
  if grep -Eq '(^|[[:space:]])nohlt([[:space:]]|$)' "$OUT/proc_cmdline.txt"; then
    echo "WARNING: nohlt detected in running kernel command line"
  else
    echo "No nohlt token detected (not proof of boot image identity)"
  fi
  echo ""
  echo "Review dmesg_read_error.txt and logcat_kernel_error.txt for permission limits."
  echo "Do not label PASS before checking actual boot, stability, PCM routing,"
  echo "WCD938x bind, and the preserved battery/charging protections."
} > "$OUT/SUMMARY.txt"

# Preserve the chronological records, including repeats from failed probes.
# Do not pretend that deduplicated records from separate attempts are one
# registered card. CARD register rc=0 identifies a successful instance.
if [ -f "$OUT/kernel_audio_messages.txt" ]; then
  grep -E 'P480 A3c (LINK_KEPT |LINK_SKIPPED |LINK_DIR_TRIM |INIT_MOVED |CARD register )' \
    "$OUT/kernel_audio_messages.txt" > "$OUT/as_built_link_attempts.txt" || true
  grep -E 'P480 A3c (ROUTE_KEPT |ROUTE_SKIPPED |ROUTES |INIT_MOVED |PORT_MAP=|AUDRX_INIT |WSA_INIT_SKIPPED |CARD register |CARD_NUMBER |DEVICES |KCONTROLS )' \
    "$OUT/kernel_audio_messages.txt" > "$OUT/as_built_route_attempts.txt" || true
fi

# Optional TAR output in the same user directory. Nothing touches partitions.
if command -v tar >/dev/null 2>&1; then
  case "$OUT" in
    /*) PARENT="${OUT%/*}"; LEAF="${OUT##*/}" ;;
    *) PARENT="."; LEAF="$OUT" ;;
  esac
  (cd "$PARENT" && tar -czf "$LEAF.tar.gz" "$LEAF") 2>/dev/null ||
    echo "Optional tar bundle skipped" >&2
fi

echo "Read-only P480 evidence saved to $OUT"
echo "Share SUMMARY.txt, proc_asound_*.txt and filtered messages for review."
