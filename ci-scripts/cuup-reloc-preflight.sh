#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Preflight for the change-of-gNB-CU-UP test (doc/cuup-relocation-testing.md).
# Reports whether this host can build and run the rfsimulator deployment.
# Read-only: loads no modules, installs nothing, changes no configuration.
#
#   ./ci-scripts/cuup-reloc-preflight.sh
#
# Exit status: 0 if every hard requirement is met, 1 otherwise.

set -u

fail=0
pass() { printf '  \033[32mok\033[0m    %-22s %s\n' "$1" "${2-}"; }
warn() { printf '  \033[33mwarn\033[0m  %-22s %s\n' "$1" "${2-}"; }
bad()  { printf '  \033[31mFAIL\033[0m  %-22s %s\n' "$1" "${2-}"; fail=1; }
info() { printf '        %-22s %s\n' "$1" "${2-}"; }

cpuflag() { grep -qm1 "^flags.*[[:space:]]$1[[:space:]]" /proc/cpuinfo; }

echo
echo "CPU"
info "model" "$(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
arch=$(uname -m)
[ "$arch" = x86_64 ] && pass "architecture" "$arch" \
                     || bad  "architecture" "$arch (this procedure assumes x86_64)"

# oai_dfts.c calls _mm256_* unconditionally, with no 128-bit fallback and no
# runtime dispatch, so AVX2 is a hard requirement -- not a performance option.
for f in sse4_2 avx avx2; do
  cpuflag "$f" && pass "$f" || bad "$f" "required: openair1/PHY/TOOLS/oai_dfts.c needs it"
done
# Without XSAVE the kernel cannot enable YMM state, so AVX instructions fault
# even if the AVX bits themselves were present. (Linux does not report osxsave
# separately in /proc/cpuinfo, so xsave is the flag to test.)
cpuflag xsave && pass "xsave" || bad "xsave" "required to execute any AVX instruction"
for f in avx512f gfni; do
  if cpuflag "$f"; then info "$f" "present (used if available)"; else info "$f" "absent (fine)"; fi
done

if cpuflag hypervisor; then
  if cpuflag avx2; then
    info "virtualised" "yes, and AVX2 is passed through"
  else
    bad "virtualised" "yes, and AVX2 is masked -- see the note at the end"
  fi
else
  info "virtualised" "no (bare metal)"
fi

echo
echo "Resources"
cores=$(nproc)
[ "$cores" -ge 4 ] && pass "cores" "$cores" || warn "cores" "$cores (4+ recommended; the build is long)"
memgb=$(( $(grep -m1 MemTotal /proc/meminfo | awk '{print $2}') / 1024 / 1024 ))
[ "$memgb" -ge 8 ] && pass "memory" "${memgb} GiB" || warn "memory" "${memgb} GiB (8+ recommended)"
diskgb=$(df -BG --output=avail / 2>/dev/null | tail -1 | tr -dc '0-9')
if [ -n "${diskgb:-}" ]; then
  [ "$diskgb" -ge 40 ] && pass "free disk on /" "${diskgb} GiB" \
                       || warn "free disk on /" "${diskgb} GiB (40+ recommended for the images)"
fi

echo
echo "Toolchain"
if command -v docker >/dev/null; then
  if docker info >/dev/null 2>&1; then
    pass "docker" "$(docker --version | cut -d, -f1)"
  else
    bad "docker" "installed but the daemon is unreachable (not running, or user not in the docker group)"
  fi
else
  bad "docker" "not installed"
fi
docker compose version >/dev/null 2>&1 && pass "docker compose" "v2 plugin" \
  || { command -v docker-compose >/dev/null && warn "docker compose" "only the v1 standalone binary found" \
                                            || bad  "docker compose" "not available (needed for the deployment)"; }
command -v git >/dev/null && pass "git" "$(git --version | awk '{print $3}')" || bad "git" "not installed"

echo
echo "Kernel"
# F1AP, E1AP and NGAP all run over SCTP.
if grep -q '^SCTP' /proc/net/protocols 2>/dev/null; then
  pass "sctp" "loaded"
elif modinfo sctp >/dev/null 2>&1; then
  warn "sctp" "module available but not loaded (sudo modprobe sctp)"
else
  bad "sctp" "no SCTP support -- F1AP/E1AP/NGAP cannot connect"
fi

echo
echo "Source tree"
src=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fs=$(stat -f -c %T "$src")
case "$fs" in
  vboxsf|9p|cifs|smb2|nfs|fuseblk)
    warn "filesystem" "$src is on $fs"
    info "" "a shared folder costs a great deal on a header-heavy build, and"
    info "" "cannot execute binaries the build generates and then runs."
    info "" "Copy to local disk and build from there." ;;
  *) pass "filesystem" "$fs" ;;
esac
[ -f "$src/CMakeLists.txt" ] && pass "repository" "$src" || bad "repository" "CMakeLists.txt not found at $src"

echo
if [ "$fail" -eq 0 ]; then
  echo "All hard requirements met."
else
  echo "Unmet requirements above."
  if cpuflag hypervisor && ! cpuflag avx2; then
    cat <<'NOTE'

  AVX2 masked inside a VM is usually the hypervisor, not the CPU.
    VirtualBox on Windows: Hyper-V underneath prevents CPU features being
      passed through. Disable it (bcdedit /set hypervisorlaunchtype off, plus
      Core Isolation -> Memory Integrity off, plus the Hyper-V and Virtual
      Machine Platform Windows features), reboot, then
      VBoxManage modifyvm <vm> --cpu-profile host.
    VMware / Parallels / KVM: enable CPU passthrough (-cpu host on KVM).
    Cloud instances: almost all modern x86 instance types expose AVX2.
NOTE
  fi
fi
echo
exit "$fail"
