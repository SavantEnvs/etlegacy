#!/usr/bin/env bash
#
# mayhem/test.sh — RUN the known-answer self test mayhem/build.sh built (/mayhem/etl_selftest from
# mayhem/tests/etl_selftest.c; upstream ships no unit tests). It checks decoded values, not exit
# codes: MD4 test vectors, MSG/Huffman round trips, delta-coded entity/playerstate/usercmd round
# trips, decoded pixels of the committed TGA/BMP/PCX seeds, botlib precompiler tokens, and contents /
# traces / checksum of the committed box.bsp. Every expected test must print "PASS <name>"; a missing
# line counts as a failure. exit 0 = pass.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}


TESTS="md4_empty md4_abc md4_80 msg_roundtrip msg_overread huff_compresses huff_roundtrip
delta_entity delta_entity_remove delta_playerstate delta_usercmd
tga_decode tga_pixel bmp_decode bmp_pixel pcx_decode pcx_pixel pc_load pc_tokens
cm_checksum cm_models cm_bounds cm_contents_inside cm_contents_outside cm_trace cm_entities"

RUNNER="$SRC/etl_selftest"
if [ ! -x "$RUNNER" ]; then
  echo "test.sh: $RUNNER missing — mayhem/build.sh should have built it" >&2
  emit_ctrf "etl-selftest" 0 1
  exit 1
fi

out=$("$RUNNER" "$SRC/mayhem" 2>&1)
echo "$out"
passed=0 failed=0
for t in $TESTS; do
  if printf '%s\n' "$out" | grep -qx "PASS $t"; then
    passed=$((passed + 1))
  else
    echo "FAILED: $t"
    failed=$((failed + 1))
  fi
done
emit_ctrf "etl-selftest" "$passed" "$failed" 0
