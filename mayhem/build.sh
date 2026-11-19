#!/usr/bin/env bash
#
# mayhem/build.sh — build the ET: Legacy fuzz targets and the known-answer self test.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem; re-runnable and offline
# (the only external library, cJSON, comes from apt). The base image exports CC, CXX,
# SANITIZER_FLAGS, LIB_FUZZING_ENGINE, STANDALONE_FUZZ_MAIN, SRC.
#
# ET: Legacy's CMake build produces the engine binaries and the mod shared objects, none of which can
# be linked into a harness (the engine has a main() and global startup; the mods are dlopen'd). Each
# target therefore compiles the upstream sources it fuzzes straight from src/, the same files CMake's
# source lists (cmake/ETLSources.cmake) put into etlded / qagame, with the same defines:
#
#   image_tga / image_pcx              libFuzzer  src/renderercommon/tr_image_{tga,pcx}.c
#   image_bmp                          AFL        src/renderercommon/tr_image_bmp.c
#   botlib_precomp                     libFuzzer  src/botlib/l_{precomp,script,memory}.c
#   msg_delta                          AFL        src/qcommon/{msg,huffman}.c
#   cm_load                            AFL        src/qcommon/cm_*.c + md4.c
#   game_session                       AFL        src/game/*.c (qagame) + json.c + botlib l_*.c
#
# The AFL targets are file-input (mayhem/harnesses/afl_main.c): engine/game errors (Com_Error,
# G_Error) end the process there, as in the engine, instead of unwinding out of the fuzzed code.
# image_bmp is file-input too because libFuzzer always runs the empty input first and R_LoadBMP
# reads its 54-byte header from any buffer (it only warns when data->size < 54), so a libFuzzer
# image_bmp could never start fuzzing.
#
# UBSan: -fno-sanitize=alignment everywhere (id Tech 3 reads little-endian fields with
# *(int *)p casts on byte buffers, e.g. tr_image_bmp.c LittleLong(*(int *)buf_p), which trips on
# every BMP); -fno-sanitize=array-bounds on src/game/bg_animation.c only (BG_AnimParseAnimScript
# overruns defineStr[type][16] into the next row while parsing the stock
# etmain/animations/scripts/human_base.script, on every input, before any fuzzed data is read).
# ASan and the rest of UBSan stay on and halting.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
: "${AFL_CC:=afl-clang-fast}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
OUT="$SRC"
B="$SRC/build-fuzz"          # gitignored by upstream (/build*/)
mkdir -p "$B/inc" "$B/gen"

# Relaxed UBSan checks (see header); only meaningful when sanitizers are on.
RELAX=""
[ -n "$SANITIZER_FLAGS" ] && RELAX="-fno-sanitize=alignment"
FUZZ_CFLAGS="$SANITIZER_FLAGS $RELAX $DEBUG_FLAGS -O1 -w -I$SRC/src"

# --- generated inputs ---------------------------------------------------------------------------
# version.c includes the header CMake generates from cmake/version_generated.h.in (ETLVersion.cmake).
read -r _ VMAJ < <(grep '^VERSION_MAJOR' VERSION.txt)
read -r _ VMIN < <(grep '^VERSION_MINOR' VERSION.txt)
read -r _ VPAT < <(grep '^VERSION_PATCH' VERSION.txt)
VER="${VMAJ}.${VMIN}.${VPAT}"
VINT=$(printf '%d%02d%02d' "$VMAJ" "$VMIN" "$VPAT")
sed -e "s/@ETL_CMAKE_VERSION@/$VER/" -e "s/@ETL_CMAKE_VERSION_SHORT@/$VER/" \
    -e "s/@ETL_CMAKE_VERSION_INT@/$VINT/" -e "s/@ETL_CMAKE_PROD_VERSION@/$VMAJ,$VMIN,$VPAT,0/" \
    -e "s/@ETL_CMAKE_PROD_VERSION_STR@/$VER.0/" -e "s/@ETL_CMAKE_BUILD_TIME@/mayhem/" \
    -e "s/@ETL_COMPILE_FEATURES@//" -e "s/@ETL_UID@/mayhem/" \
    cmake/version_generated.h.in > "$B/inc/version_generated.h.tmp"
mv "$B/inc/version_generated.h.tmp" "$B/inc/version_generated.h"
# The game data the qagame harness reads (character/animation scripts, configs), from etmain/.
python3 mayhem/tools/embed_files.py "$B/gen/fz_files.c" etmain characters animations configs

# --- helpers ------------------------------------------------------------------------------------
# compile <objdir> <compiler> "<flags>" <sources...>   (parallel; prints nothing; sets OBJS)
compile() {
  local dir=$1 cc=$2 flags=$3; shift 3
  local pids=() s o
  mkdir -p "$dir"
  OBJS=()
  for s in "$@"; do
    o="$dir/$(echo "${s#"$SRC"/}" | tr '/' '_').o"
    OBJS+=("$o")
    while [ "$(jobs -rp | wc -l)" -ge "$MAYHEM_JOBS" ]; do wait -n; done
    # shellcheck disable=SC2086
    $cc $flags -c "$s" -o "$o" &
    pids+=($!)
  done
  for s in "${pids[@]}"; do wait "$s"; done
}

LSAN_OFF="$B/lsan_off.o"
# shellcheck disable=SC2086
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$LSAN_OFF"

H=mayhem/harnesses
Q="src/qcommon/q_shared.c src/qcommon/q_math.c"

# libfuzzer_target <name> "<extra flags>" <sources...>  ->  /mayhem/fuzz_<name> + -standalone
libfuzzer_target() {
  local name=$1 extra=$2; shift 2
  local d="$B/$name"
  compile "$d/fuzz" "$CC" "$FUZZ_CFLAGS -fsanitize=fuzzer-no-link $extra" "$@"
  # shellcheck disable=SC2086
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "${OBJS[@]}" "$LSAN_OFF" -lm -o "$OUT/fuzz_$name"
  compile "$d/standalone" "$CC" "$FUZZ_CFLAGS $extra" "$@" "$STANDALONE_FUZZ_MAIN"
  # shellcheck disable=SC2086
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS "${OBJS[@]}" "$LSAN_OFF" -lm -o "$OUT/fuzz_$name-standalone"
}

# the harnesses keep libFuzzer entry-point names; afl_main.c calls them as fz_init/fz_run
AFL_RENAME="-DLLVMFuzzerTestOneInput=fz_run -DLLVMFuzzerInitialize=fz_init"

# afl_target <name> "<extra flags>" "<libs>" <sources...>  ->  /mayhem/fuzz_<name> (its own reproducer)
afl_target() {
  local name=$1 extra=$2 libs=$3; shift 3
  compile "$B/$name/afl" "$AFL_CC" "$FUZZ_CFLAGS $AFL_RENAME -DFZ_ERROR_EXITS $extra" "$@" "$SRC/$H/afl_main.c"
  # shellcheck disable=SC2086
  AFL_QUIET=1 $AFL_CC $SANITIZER_FLAGS $DEBUG_FLAGS "${OBJS[@]}" "$LSAN_OFF" $libs -lm -o "$OUT/fuzz_$name"
}

# --- 1) libFuzzer targets -------------------------------------------------------------------------
for fmt in tga pcx; do
  FMT=$(echo "$fmt" | tr a-z A-Z)
  libfuzzer_target "image_$fmt" "-DIMAGE_LOADER=R_Load$FMT" \
    "$SRC/$H/fuzz_image.c" "$SRC/src/renderercommon/tr_image_$fmt.c"
done

# shellcheck disable=SC2046
libfuzzer_target botlib_precomp "-DBOTLIB" \
  "$SRC/$H/fuzz_botlib_precomp.c" "$SRC/$H/fz_common.c" \
  $(printf "$SRC/src/botlib/%s.c " l_precomp l_script l_memory) $(printf "$SRC/%s " $Q)

# --- 2) AFL file-input targets --------------------------------------------------------------------
afl_target image_bmp "-DIMAGE_LOADER=R_LoadBMP" "" \
  "$SRC/$H/fuzz_image.c" "$SRC/src/renderercommon/tr_image_bmp.c"

# shellcheck disable=SC2046
afl_target msg_delta "-DDEDICATED" "" \
  "$SRC/$H/fuzz_msg.c" "$SRC/$H/fz_common.c" "$SRC/src/qcommon/msg.c" "$SRC/src/qcommon/huffman.c" \
  $(printf "$SRC/%s " $Q)

# shellcheck disable=SC2046
afl_target cm_load "-DDEDICATED" "" \
  "$SRC/$H/fuzz_cm_load.c" "$SRC/$H/fz_common.c" \
  $(printf "$SRC/src/qcommon/%s.c " cm_load cm_patch cm_polylib cm_test cm_trace md4) \
  $(printf "$SRC/%s " $Q)

# qagame: the module's own sources (QAGAME_SRC) + json.c (etl_json), compiled as CMake does
# (GAMEDLL MODLIB, FEATURE_MULTIVIEW is a default-on mod feature); the engine's botlib precompiler
# serves the module's PC_* traps. bg_animation.c gets its own compile line (see header).
GAME_DEFS="-DGAMEDLL=1 -DMODLIB=1 -DFEATURE_MULTIVIEW -I$SRC/src/qcommon/json -I$B/inc"
GAME_SRCS=()
for s in "$SRC"/src/game/*.c; do
  [ "$(basename "$s")" = bg_animation.c ] || GAME_SRCS+=("$s")
done
compile "$B/game_session/anim" "$AFL_CC" \
  "$FUZZ_CFLAGS -DFZ_ERROR_EXITS $GAME_DEFS ${RELAX:+-fno-sanitize=array-bounds}" "$SRC/src/game/bg_animation.c"
ANIM_OBJS=("${OBJS[@]}")
compile "$B/game_session/botlib" "$AFL_CC" "$FUZZ_CFLAGS -DBOTLIB" \
  "$SRC/src/botlib/l_precomp.c" "$SRC/src/botlib/l_script.c" "$SRC/src/botlib/l_memory.c"
BOTLIB_OBJS=("${OBJS[@]}")
compile "$B/game_session/data" "$CC" "-O0 -w" "$B/gen/fz_files.c"
DATA_OBJS=("${OBJS[@]}")
# shellcheck disable=SC2046
compile "$B/game_session/afl" "$AFL_CC" "$FUZZ_CFLAGS $AFL_RENAME -DFZ_ERROR_EXITS $GAME_DEFS" \
  "$SRC/$H/fuzz_game_session.c" "${GAME_SRCS[@]}" $(printf "$SRC/%s " $Q) \
  "$SRC/src/qcommon/version.c" "$SRC/src/qcommon/json/json.c" "$SRC/vendor/sha-1/sha1.c" \
  "$SRC/$H/afl_main.c"
# shellcheck disable=SC2086
AFL_QUIET=1 $AFL_CC $SANITIZER_FLAGS $DEBUG_FLAGS "${OBJS[@]}" "${ANIM_OBJS[@]}" "${BOTLIB_OBJS[@]}" \
  "${DATA_OBJS[@]}" "$LSAN_OFF" -lcjson -lm -o "$OUT/fuzz_game_session"

# --- 3) known-answer self test, NORMAL flags (no sanitizers, no fuzzing instrumentation) ----------
# shellcheck disable=SC2046,SC2086
$CC -O1 -g -w $COVERAGE_FLAGS -I"$SRC/src" -DDEDICATED -DBOTLIB \
  mayhem/tests/etl_selftest.c "$H/fz_common.c" \
  $(printf "src/qcommon/%s.c " md4 msg huffman cm_load cm_patch cm_polylib cm_test cm_trace) \
  $(printf "src/renderercommon/tr_image_%s.c " tga bmp pcx) \
  $(printf "src/botlib/%s.c " l_precomp l_script l_memory) $Q \
  -lm -o "$OUT/etl_selftest"

ls -l "$OUT"/fuzz_* "$OUT/etl_selftest"
