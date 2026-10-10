#!/usr/bin/env bash
# Asserts libbitcoind_c exports exactly the functions its header declares, nothing else.
# macOS reads dyld's exports with cctools nm; GNU nm there lists the symbol table instead.
# Usage: ci/check-exports.sh <libbitcoind_c.dylib|.so> <bitcoind_c.h> [nm]
set -euo pipefail
lib="$1"
header="$2"
nm="${3:-nm}"

case "$(uname -s)" in
  Darwin) exports() { "$nm" -gU "$1" | awk '{print $NF}' | sed 's/^_//'; } ;;
  Linux) exports() { "$nm" -D --defined-only "$1" | awk '{print $NF}'; } ;;
  *) echo "unsupported host $(uname -s)" >&2; exit 1 ;;
esac

want="$(grep -o -E 'BITCOIND_[a-z_]+\(' "$header" | tr -d '(' | sort -u)"
got="$(exports "$lib" | sort -u)"
if [ "$got" != "$want" ]; then
  echo "exports of $lib differ from $header (< header, > library):" >&2
  diff <(echo "$want") <(echo "$got") >&2 || true
  exit 1
fi
echo "exports: $(echo "$got" | wc -l | tr -d ' ') BITCOIND_* functions and nothing else"
