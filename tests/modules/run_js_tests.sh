#!/usr/bin/env bash
# Run JavaScript module unit tests in Node.js.
# Called by scripts/install.sh before deploying to hardware.
# Exit code 1 if any suite fails.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TESTS_DIR="$REPO_ROOT/tests/modules"

if ! command -v node >/dev/null 2>&1; then
    echo "  [JS tests] Node.js not found — skipping"
    exit 0
fi

NODE_VERSION=$(node --version 2>/dev/null | sed 's/v//' | cut -d. -f1)
if [ "${NODE_VERSION:-0}" -lt 18 ]; then
    echo "  [JS tests] Node.js v18+ required (found v${NODE_VERSION}) — skipping"
    exit 0
fi

echo "  [JS tests] Running JS module tests..."

PASS=0
FAIL=0

run_suite() {
    local file="$1"
    local name
    name=$(basename "$file")
    if node "$file" 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "  [JS tests] FAILED: $name"
    fi
}

run_suite "$TESTS_DIR/ablem8/test_virtual_knobs.mjs"

echo "  [JS tests] $PASS suite(s) passed, $FAIL failed"

if [ "$FAIL" -gt 0 ]; then
    echo "  [JS tests] Fix failures before deploying to hardware."
    exit 1
fi
