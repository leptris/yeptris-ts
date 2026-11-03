#!/usr/bin/env bash
# Builds the ES module + wasm from the vendored engine sources.
#   ENGINESRC defaults to ../yeptris/src (sibling checkout)
set -euo pipefail
cd "$(dirname "$0")"
ENGINESRC="${ENGINESRC:-../yeptris/src}"
ENGINEROOT="$(dirname "$ENGINESRC")"
mkdir -p dist gen/yeptris
# version.h is CMake-generated; generate the same file here (the
# CMakeLists project VERSION is the single source of truth)
V="$(sed -nE 's/^ *VERSION ([0-9]+)\.([0-9]+)\.([0-9]+)$/\1 \2 \3/p' "$ENGINEROOT/CMakeLists.txt" | head -1)"
set -- $V
sed -e "s/@PROJECT_VERSION_MAJOR@/$1/" \
    -e "s/@PROJECT_VERSION_MINOR@/$2/" \
    -e "s/@PROJECT_VERSION_PATCH@/$3/" \
    -e "s/@PROJECT_VERSION@/$1.$2.$3/" \
    "$ENGINESRC/include/yeptris/version.h.in" > gen/yeptris/version.h
# the heap views (HEAPU8/HEAPU32) ride the Module always in current
# emcc — listing them in EXPORTED_RUNTIME_METHODS is invalid now
# (the floating emsdk's emcc warns "invalid item", tripping the
# zero-warning gate; src/index.js reads m.HEAPU8/m.HEAPU32 directly)
emcc -O3 -std=c11 \
  -I"$ENGINESRC/include" -I"$ENGINESRC" -I"$ENGINESRC/yeptris" -Igen \
  src/yeptris_wasm.c \
  "$ENGINESRC"/yeptris/*.c \
  "$ENGINESRC"/yeptris/*/*.c \
  "$ENGINESRC"/yeptris/*/*/*.c \
  -s MODULARIZE=1 -s EXPORT_ES6=1 -s ENVIRONMENT=web,node -s EXPORTED_RUNTIME_METHODS="['UTF8ToString','stringToUTF8','lengthBytesUTF8']" \
  -s EXPORTED_FUNCTIONS='["_yeptris_wasm_load","_yeptris_wasm_load_all","_yeptris_wasm_dump","_yeptris_wasm_dump_ex","_yeptris_wasm_out","_yeptris_wasm_out_len","_yeptris_wasm_out_free","_yeptris_wasm_last_error","_yeptris_wasm_last_error_line","_yeptris_wasm_last_error_column","_yeptris_wasm_events","_yeptris_wasm_event_records","_yeptris_wasm_event_count","_yeptris_wasm_event_arena","_yeptris_wasm_event_arena_len","_yeptris_wasm_events_free","_yeptris_wasm_validate","_yeptris_wasm_json_schema_validate","_yeptris_wasm_schema_compile","_yeptris_wasm_schema_validate","_yeptris_wasm_schema_free","_yeptris_wasm_free","_malloc","_yeptris_wasm_version"]' \
  -s WASM_BIGINT=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -o dist/yeptris.mjs
ls -la dist/
