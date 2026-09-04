#!/bin/bash
# clang++ wrapper for building GSIM.
#
# GSIM builds with -Werror, and clang-23 emits a gcc-install-dir warning that older compilers do
# not, so a stock clang++ invocation fails on a warning that says nothing about GSIM. Silence just
# that diagnostic and pin the gcc toolchain so libstdc++ headers resolve deterministically.
#
# Point GSIM_CLANGXX (or MERLIN_CLANG, which merlin already sets) at the clang++ to use; falls back
# to whatever clang++ is on PATH. GSIM_GCC_INSTALL_DIR overrides the pinned gcc toolchain; by
# default the newest /usr/lib/gcc/<triple>/<version> on the box is used.
set -euo pipefail

CXX="${GSIM_CLANGXX:-${MERLIN_CLANG:-clang++}}"
case "$CXX" in
  */clang) CXX="${CXX}++" ;;
esac

if [ -z "${GSIM_GCC_INSTALL_DIR:-}" ]; then
  triple="$(uname -m)-linux-gnu"
  GSIM_GCC_INSTALL_DIR="$(ls -d /usr/lib/gcc/"$triple"/* 2>/dev/null | sort -V | tail -1)"
fi

exec "$CXX" \
  ${GSIM_GCC_INSTALL_DIR:+--gcc-install-dir="$GSIM_GCC_INSTALL_DIR"} \
  -Wno-gcc-install-dir-libstdcxx -Wno-error=gcc-install-dir-libstdcxx "$@"
