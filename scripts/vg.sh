#!/usr/bin/env bash
# Compiles a C program ($SRC) into IR at O0 or O1, runs VarGuard, builds into an executable, and runs it.
# Usage: scripts/varguard.sh -o0|-o1
# Produces: (test_name).ll, processed.ll, and test.
# Default: test_name is t2. Set SRC to change it.

set -euo pipefail

SRC=tests/t2.c
ORIGINAL_IR="$(basename "${SRC%.c}").ll"
PROCESSED_IR="processed.ll"
OPT=./build/bin/opt

case "${1:-}" in
  -o0|-O0)
    LABEL="NON-OPTIMIZED"
    OPT_LEVEL_FLAGS=(-O0 -Xclang -disable-O0-optnone)
    ;;
  -o1|-O1)
    LABEL="OPTIMIZATION LVL 1"
    OPT_LEVEL_FLAGS=(-O1)
    ;;
  *)
    echo "Usage: $0 -o0|-o1" >&2
    exit 1
    ;;
esac

echo "[$LABEL]"
echo "[1/4] Creating regular IR from $SRC..."
clang -std=c2y "${OPT_LEVEL_FLAGS[@]}" -fno-stack-protector -S -emit-llvm "$SRC" -o "$ORIGINAL_IR"

echo "[2/4] Creating processed IR (running VarGuard pass)..."
"$OPT" -pass-remarks=var-guard -passes='require<libcall-lowering-info>,function(var-guard)' "$ORIGINAL_IR" -S -o "$PROCESSED_IR"

echo "[3/4] Compiling processed IR into an executable..."
clang -fno-stack-protector "$PROCESSED_IR" -o test

echo "[4/4] Running the executable..."
set +e
./test
status=$?
set -e
echo "exit: $status"
echo "Done! Use objdump -d test to inspect the assembly."
