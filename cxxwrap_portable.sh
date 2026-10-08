#!/bin/bash
# clang++ wrapper for building GSIM, for a machine other than the one the certified
# model was built on.
#
# NOT interchangeable with cxxwrap.sh. The GSIM build receipt pins cxxwrap.sh by content
# hash, so those bytes are frozen: changing them invalidates the certificate even though
# the emulator binary is unaffected. Use this script when standing GSIM up somewhere new,
# and re-certify there; use cxxwrap.sh to reproduce the pinned build.
#
# GSIM builds with -Werror, and clang-23 emits a gcc-install-dir warning that older compilers do
# not, so a stock clang++ invocation fails on a warning that says nothing about GSIM. Silence just
# that diagnostic and pin the gcc toolchain so libstdc++ headers resolve deterministically.
#
# Point GSIM_CLANGXX (or MERLIN_CLANG, which merlin already sets) at the clang++ to use; falls back
# to whatever clang++ is on PATH. GSIM_GCC_INSTALL_DIR overrides the pinned gcc toolchain; by
# default the newest GCC installation with matching C++ development headers is used.
set -euo pipefail

CXX="${GSIM_CLANGXX:-${MERLIN_CLANG:-clang++}}"

if [ -z "${GSIM_GCC_INSTALL_DIR:-}" ]; then
  triple="$(uname -m)-linux-gnu"
  gcc_dirs=()
  for candidate in /usr/lib/gcc/"$triple"/*; do
    version="${candidate##*/}"
    if [[ -d "$candidate" && -f "/usr/include/c++/$version/iostream" &&
          -f "/usr/include/$triple/c++/$version/bits/c++config.h" ]]; then
      gcc_dirs+=("$candidate")
    fi
  done
  if (( ${#gcc_dirs[@]} )); then
    GSIM_GCC_INSTALL_DIR="$(printf '%s\n' "${gcc_dirs[@]}" | sort -V | tail -1)"
  fi
fi

exec "$CXX" \
  --driver-mode=g++ \
  ${GSIM_GCC_INSTALL_DIR:+--gcc-install-dir="$GSIM_GCC_INSTALL_DIR"} \
  -Wno-gcc-install-dir-libstdcxx -Wno-error=gcc-install-dir-libstdcxx "$@"
