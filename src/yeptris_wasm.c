/* The wasm surface: one contained bridge over the zero-copy core.
 * Input bytes are copied INTO the wasm heap; the document's views live
 * inside the heap for the duration of the call; the answers (JSON from
 * YAML, YAML from JSON) leave before the document is freed. The host
 * never holds a heap pointer past the call — the C buffer-lifetime
 * contract stays internal. */
#include <emscripten/emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yeptris/dom.h"
#include "yeptris/emit.h"
#include "yeptris/error.h"
#include "yeptris/json.h"
#include "yeptris/parse.h"
#include "yeptris/version.h"

static char *g_out;
static size_t g_out_len;

static char last_error[256];
static int last_error_line;
static int last_error_column;

static void capture_error(void) {
    uint32_t line = 0;
    uint32_t col = 0;
    const char *msg = yeptris_last_error(&line, &col);
    snprintf(last_error, sizeof last_error, "%s", msg ? msg : "parse failed");
    last_error_line = (int)line;
    last_error_column = (int)col;
}

/* YAML in -> canonical JSON out (yeptris_serialize_json). */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_load(const char *data, size_t len) {
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(data, len, &st);
    if (doc == NULL || st != YEPTRIS_OK) {
        capture_error();
        return (int)st;
    }
    size_t out_len = 0;
    char *out = yeptris_serialize_json(doc, &out_len);
    yeptris_document_free(doc);
    if (out == NULL) {
        snprintf(last_error, sizeof last_error, "serialization failed");
        last_error_line = 0;
        last_error_column = 0;
        return YEPTRIS_ERROR_MEMORY;
    }
    g_out = out;
    g_out_len = out_len;
    return YEPTRIS_OK;
}

/* JSON in -> canonical YAML out (JSON is a subset of YAML 1.2 flow, so
 * the one parse engine takes it; the emitter's canonical form is
 * byte-stable across round trips). */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_dump(const char *data, size_t len) {
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(data, len, &st);
    if (doc == NULL || st != YEPTRIS_OK) {
        capture_error();
        return (int)st;
    }
    size_t out_len = 0;
    char *out = yeptris_serialize(doc, &out_len);
    yeptris_document_free(doc);
    if (out == NULL) {
        snprintf(last_error, sizeof last_error, "serialization failed");
        last_error_line = 0;
        last_error_column = 0;
        return YEPTRIS_ERROR_MEMORY;
    }
    g_out = out;
    g_out_len = out_len;
    return YEPTRIS_OK;
}

EMSCRIPTEN_KEEPALIVE
const char *yeptris_wasm_last_error(void) { return last_error; }

EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_last_error_line(void) { return last_error_line; }

EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_last_error_column(void) { return last_error_column; }

EMSCRIPTEN_KEEPALIVE
const char *yeptris_wasm_out(void) { return g_out; }

EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_out_len(void) { return (int)g_out_len; }

EMSCRIPTEN_KEEPALIVE
void yeptris_wasm_free(void *p) { yeptris_free(p); }

EMSCRIPTEN_KEEPALIVE
const char *yeptris_wasm_version(void) { return yeptris_version(); }
