#!/usr/bin/env bash

# Compiles one test function from a C program ($SRC) into IR at O0 or O1, 
# runs VarGuard, builds into an executable, and runs it. The test program
# must support conditional compilation (such as t2.c).
#
# Usage: scripts/vg_runner2.sh -o0|-o1 -t<n>
# Produces: (test_name).ll, processed.ll, and test.
# Default: test_name is t2. Set SRC to change it.

set -euo pipefail

SRC=tests/t2.c
ORIGINAL_IR="$(basename "${SRC%.c}").ll"
PROCESSED_IR="processed.ll"
OPT=./build/bin/opt

usage() {
  echo "Usage: $0 -o0|-o1 -t<n>" >&2
  exit 1
}

OPT_LEVEL=""
TEST_NUM=""

# Get optimization level and test number
while [[ $# -gt 0 ]]; do
  case "$1" in
    -o0|-O0|-o1|-O1)
      OPT_LEVEL="${1,,}"
      shift
      ;;
    -t)
      [[ $# -ge 2 ]] || usage
      TEST_NUM="$2"
      shift 2
      ;;
    -t*)
      TEST_NUM="${1#-t}"
      shift
      ;;
    *)
      usage
      ;;
  esac
done

# Print optimization level
case "$OPT_LEVEL" in
  -o0)
    LABEL="NON-OPTIMIZED"
    OPT_LEVEL_FLAGS=(-O0 -Xclang -disable-O0-optnone)
    ;;
  -o1)
    LABEL="OPTIMIZATION LVL 1"
    OPT_LEVEL_FLAGS=(-O1)
    ;;
  *)
    usage
    ;;
esac

# Reject test numbers not in the test file
[[ "$TEST_NUM" =~ ^[0-9]+$ ]] || usage

# Create IR, compile, build, run
echo "[$LABEL, test$TEST_NUM]"
echo "[1/4] Creating regular IR from $SRC (test$TEST_NUM only)..."
clang -std=c2y "${OPT_LEVEL_FLAGS[@]}" -DTEST="$TEST_NUM" -fno-stack-protector -S -emit-llvm "$SRC" -o "$ORIGINAL_IR"

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

