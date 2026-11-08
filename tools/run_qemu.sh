#!/usr/bin/env bash
# Boot the MintOS26 GPT UEFI image in QEMU/OVMF on Linux or macOS (Homebrew).
#
#   tools/run_qemu.sh [--headless] [--mem MiB] [--no-data] [--blank] [--] [extra qemu args]
#
# Disks are attached to the q35 ICH9 AHCI controller with snapshot=on, so the
# guest can never modify the build images (including the Disk Utility
# formatter). The NIC is an Intel 82540EM (e1000) on QEMU user networking.
# COM1 goes to build/com1-qemu.log. QEMU's e1000 implements loopback only via
# PHY BMCR loopback, so `net test` results are not VirtualBox evidence.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build"
headless=0; mem=1024; data=1; blank=0; extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    --headless) headless=1 ;;
    --mem) mem="$2"; shift ;;
    --no-data) data=0 ;;
    --blank) blank=1 ;;
    --) shift; extra+=("$@"); break ;;
    *) extra+=("$1") ;;
  esac
  shift
done

qemu="$(command -v qemu-system-x86_64 || true)"
[ -n "$qemu" ] || { echo "qemu-system-x86_64 not found (macOS: brew install qemu; Debian/Ubuntu: apt install qemu-system-x86 ovmf)"; exit 1; }

code=""; vars_template=""
for dir in "$(dirname "$qemu")/../share/qemu" /opt/homebrew/share/qemu /usr/local/share/qemu \
           /usr/share/OVMF /usr/share/ovmf /usr/share/edk2/x64 /usr/share/edk2/ovmf /usr/share/qemu; do
  [ -d "$dir" ] || continue
  for c in edk2-x86_64-code.fd OVMF_CODE_4M.fd OVMF_CODE.fd; do
    if [ -z "$code" ] && [ -f "$dir/$c" ]; then code="$dir/$c"; fi
  done
  for v in edk2-i386-vars.fd OVMF_VARS_4M.fd OVMF_VARS.fd; do
    if [ -z "$vars_template" ] && [ -f "$dir/$v" ]; then vars_template="$dir/$v"; fi
  done
done
[ -n "$code" ] && [ -n "$vars_template" ] || { echo "OVMF firmware (code + vars) not found"; exit 1; }

disk="$(ls -t "$build"/mintos26-*-uefi.img 2>/dev/null | head -n1 || true)"
[ -n "$disk" ] || { echo "No build/mintos26-*-uefi.img; run make all first"; exit 1; }
vars="$build/qemu-ovmf-vars.fd"
[ -f "$vars" ] || cp "$vars_template" "$vars"

args=(-machine q35 -m "$mem" -cpu max
      -drive "if=pflash,format=raw,readonly=on,file=$code"
      -drive "if=pflash,format=raw,file=$vars"
      -drive "if=none,id=boot,format=raw,snapshot=on,file=$disk"
      -device ide-hd,drive=boot,bus=ide.0,bootindex=0)
if [ "$data" = 1 ] && [ -f "$build/mintos26-data-512m.img" ]; then
  args+=(-drive "if=none,id=data,format=raw,snapshot=on,file=$build/mintos26-data-512m.img"
         -device ide-hd,drive=data,bus=ide.1)
fi
if [ "$blank" = 1 ]; then
  blank_img="$build/qemu-blank-512m.img"
  [ -f "$blank_img" ] || dd if=/dev/zero of="$blank_img" bs=1048576 count=0 seek=512 2>/dev/null
  args+=(-drive "if=none,id=blank,format=raw,snapshot=on,file=$blank_img"
         -device ide-hd,drive=blank,bus=ide.2)
fi
args+=(-nic user,model=e1000 -serial "file:$build/com1-qemu.log" -vga std)
if [ "$headless" = 1 ]; then args+=(-display none); fi
echo "Booting $(basename "$disk") with $(basename "$code"); COM1 -> build/com1-qemu.log"
echo "At the selector press 2 (or M) for native MintDE; Enter/1 or no input selects UEFI Desktop."
exec "$qemu" "${args[@]}" ${extra[@]+"${extra[@]}"}
