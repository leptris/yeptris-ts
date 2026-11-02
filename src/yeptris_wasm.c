/* The wasm surface: one contained bridge over the zero-copy core.
 * Input bytes are copied INTO the wasm heap; the document's views live
 * inside the heap for the duration of the call; the answers (JSON from
 * YAML, YAML from JSON) leave before the document is freed. The host
 * never holds a heap pointer past the call — the C buffer-lifetime
 * contract stays internal. */
#include <emscripten/emscripten.h>
#include <stdint.h>
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

static void capture_plain(const char *msg) {
    snprintf(last_error, sizeof last_error, "%s", msg);
    last_error_line = 0;
    last_error_column = 0;
}

static void publish_out(char *out, size_t out_len) {
    g_out = out;
    g_out_len = out_len;
}

/* ---- multi-document streams -------------------------------------------
 *
 * yeptris_parse returns the WHOLE stream as one document set (the DOM
 * forest: yeptris_document_count / yeptris_document_root). The JSON
 * emitter answers for exactly one document (yep_emit_run's dcount==1
 * guard), so per-document JSON is produced by re-typing the forest
 * into a fresh document through the public build API:
 *
 *   - scalars copy value + style; plain re-resolves through the same
 *     schema resolver exactly like parse, quoted/block forces string
 *     — the JSON typing is identical by construction;
 *   - explicit tags ride BUILD_TAG;
 *   - aliases project to null and anchors drop — which IS the JSON
 *     projection law (emit_alias_ex writes null, JSON spells no
 *     anchors).
 *
 * The one extra nesting level the wrapper sequence adds is why the
 * recursion cap is the parser's cap + 1. */

#define YW_MAX_DEPTH 1001 /* parser default max_depth (1000) + wrapper */

typedef struct {
    YeptrisBuildEntry *entries;
    size_t n, cap;
    char *blob;
    size_t blen, bcap;
    int failed; /* 0 ok, else the YeptrisStatus */
    int depth;
} yw_rec;

static int yw_push_entry(yw_rec *r, uint8_t op, uint8_t style, size_t off, uint32_t len) {
    if (r->n == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 64;
        YeptrisBuildEntry *ne = (YeptrisBuildEntry *)realloc(r->entries, ncap * sizeof *ne);
        if (ne == NULL) {
            r->failed = YEPTRIS_ERROR_MEMORY;
            return -1;
        }
        r->entries = ne;
        r->cap = ncap;
    }
    YeptrisBuildEntry *e = &r->entries[r->n++];
    e->op = op;
    e->style = style;
    e->reserved = 0;
    e->off = (uint32_t)off;
    e->len = len;
    return 0;
}

static long yw_push_blob(yw_rec *r, const char *p, size_t len) {
    if (r->blen + len > UINT32_MAX) { /* blob offsets are 32-bit */
        r->failed = YEPTRIS_ERROR_MEMORY;
        return -1;
    }
    if (r->blen + len > r->bcap) {
        size_t ncap = r->bcap ? r->bcap : 4096;
        while (ncap < r->blen + len) {
            ncap *= 2;
        }
        char *nb = (char *)realloc(r->blob, ncap);
        if (nb == NULL) {
            r->failed = YEPTRIS_ERROR_MEMORY;
            return -1;
        }
        r->blob = nb;
        r->bcap = ncap;
    }
    if (len > 0) {
        memcpy(r->blob + r->blen, p, len);
    }
    long off = (long)r->blen;
    r->blen += len;
    return off;
}

static int yw_tag_entry(yw_rec *r, YeptrisNode node) {
    size_t tl = 0;
    const char *t = yeptris_node_tag(node, &tl);
    if (t == NULL || tl == 0) {
        return 0;
    }
    long off = yw_push_blob(r, t, tl);
    if (off < 0) {
        return -1;
    }
    return yw_push_entry(r, YEPTRIS_BUILD_TAG, 0, (size_t)off, (uint32_t)tl);
}

/* Records one subtree into the build entry/blob buffers. */
static int yw_flatten(yw_rec *r, YeptrisNode node) {
    if (r->failed != 0) {
        return -1;
    }
    if (++r->depth > YW_MAX_DEPTH) {
        r->failed = YEPTRIS_ERROR_DEPTH;
        return -1;
    }
    switch (yeptris_node_kind(node)) {
    case YEPTRIS_NODE_ALIAS: {
        /* the JSON projection law: aliases dereference to null */
        long off = yw_push_blob(r, "null", 4);
        if (off < 0 ||
            yw_push_entry(r, YEPTRIS_BUILD_SCALAR, YEPTRIS_STYLE_PLAIN, (size_t)off, 4) != 0) {
            return -1;
        }
        break;
    }
    case YEPTRIS_NODE_SCALAR: {
        size_t vl = 0;
        const char *v = yeptris_node_value(node, &vl);
        /* a parsed empty document root carries STYLE_ANY (no recorded
         * style); the build API only types PLAIN scalars, and ANY
         * means "no recorded style" — i.e. plain — so normalize */
        uint8_t sty = (uint8_t)yeptris_node_style(node);
        if (sty == YEPTRIS_STYLE_ANY) {
            sty = YEPTRIS_STYLE_PLAIN;
        }
        long off = yw_push_blob(r, v ? v : "", vl);
        if (off < 0 || yw_push_entry(r, YEPTRIS_BUILD_SCALAR, sty, (size_t)off, (uint32_t)vl) != 0) {
            return -1;
        }
        if (yw_tag_entry(r, node) != 0) {
            return -1;
        }
        break;
    }
    default: { /* sequence or mapping */
        uint8_t op = yeptris_node_kind(node) == YEPTRIS_NODE_MAPPING ? YEPTRIS_BUILD_MAP
                                                                     : YEPTRIS_BUILD_SEQ;
        if (yw_push_entry(r, op, 0, 0, 0) != 0 || yw_tag_entry(r, node) != 0) {
            return -1;
        }
        size_t n = yeptris_node_children(node, NULL, 0);
        if (n > 0) {
            YeptrisNode *kids = (YeptrisNode *)malloc(n * sizeof *kids);
            if (kids == NULL) {
                r->failed = YEPTRIS_ERROR_MEMORY;
                return -1;
            }
            size_t got = yeptris_node_children(node, kids, n);
            for (size_t i = 0; i < got; i++) {
                if (yw_flatten(r, kids[i]) != 0) {
                    free(kids);
                    return -1;
                }
            }
            free(kids);
        }
        if (yw_push_entry(r, YEPTRIS_BUILD_END, 0, 0, 0) != 0) {
            return -1;
        }
        break;
    }
    }
    r->depth--;
    return 0;
}

static void yw_rec_free(yw_rec *r) {
    free(r->entries);
    free(r->blob);
    r->entries = NULL;
    r->blob = NULL;
}

/* Builds a fresh document from the recorder and serializes it as JSON
 * (engine-owned output buffer). r is freed either way. */
static char *yw_serialize_rebuilt(yw_rec *r, size_t *out_len) {
    if (r->failed != 0) {
        yw_rec_free(r);
        return NULL;
    }
    YeptrisDocument b = yeptris_document_new();
    if (b == NULL) {
        yw_rec_free(r);
        return NULL;
    }
    YeptrisStatus bst = yeptris_document_build(b, r->entries, r->n, r->blob, r->blen);
    yw_rec_free(r);
    if (bst != YEPTRIS_OK) {
        return NULL;
    }
    char *out = yeptris_serialize_json(b, out_len);
    yeptris_document_free(b);
    return out;
}

/* Builds the stream answer: a JSON array of every document. count==0
 * builds the empty wrapper ("[]"); every other stream wraps its
 * documents in one sequence, which serialize_json spells as the
 * array. */
static int yeptris_wasm_load_all_answer(YeptrisDocument doc, size_t count) {
    yw_rec r;
    memset(&r, 0, sizeof r);
    if (yw_push_entry(&r, YEPTRIS_BUILD_SEQ, 0, 0, 0) != 0) {
        capture_plain("document rebuild failed");
        return YEPTRIS_ERROR_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        YeptrisNode root = yeptris_document_root(doc, i);
        if (root == NULL || yw_flatten(&r, root) != 0) {
            capture_plain(r.failed == YEPTRIS_ERROR_DEPTH ? "document nesting exceeds the rebuild depth cap"
                                                          : "document rebuild failed");
            yw_rec_free(&r);
            return r.failed != 0 ? r.failed : YEPTRIS_ERROR_MEMORY;
        }
    }
    if (yw_push_entry(&r, YEPTRIS_BUILD_END, 0, 0, 0) != 0) {
        capture_plain("document rebuild failed");
        return YEPTRIS_ERROR_MEMORY;
    }
    size_t out_len = 0;
    char *out = yw_serialize_rebuilt(&r, &out_len);
    if (out == NULL) {
        capture_plain("serialization failed");
        return YEPTRIS_ERROR_MEMORY;
    }
    publish_out(out, out_len);
    return YEPTRIS_OK;
}

/* YAML in -> canonical JSON out (yeptris_serialize_json). A
 * multi-document stream answers with the FIRST document — the json.h
 * contract; the emitter only writes dcount==1 forests directly, so
 * bigger streams re-type document 0 through the build API. An empty
 * stream (no documents) keeps the engine's answer: an empty output. */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_load(const char *data, size_t len) {
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(data, len, &st);
    if (doc == NULL || st != YEPTRIS_OK) {
        capture_error();
        return (int)st;
    }
    size_t out_len = 0;
    char *out = NULL;
    if (yeptris_document_count(doc) <= 1) {
        out = yeptris_serialize_json(doc, &out_len);
        yeptris_document_free(doc);
    } else {
        yw_rec r;
        memset(&r, 0, sizeof r);
        int rc = 0;
        YeptrisNode root = yeptris_document_root(doc, 0);
        if (root == NULL || yw_flatten(&r, root) != 0) {
            rc = r.failed != 0 ? r.failed : YEPTRIS_ERROR_MEMORY;
        } else {
            out = yw_serialize_rebuilt(&r, &out_len);
            rc = out == NULL ? YEPTRIS_ERROR_MEMORY : YEPTRIS_OK;
        }
        yeptris_document_free(doc);
        if (rc != YEPTRIS_OK) {
            capture_plain(rc == YEPTRIS_ERROR_DEPTH
                              ? "document nesting exceeds the rebuild depth cap"
                              : "document rebuild failed");
            return rc;
        }
    }
    if (out == NULL) {
        snprintf(last_error, sizeof last_error, "serialization failed");
        last_error_line = 0;
        last_error_column = 0;
        return YEPTRIS_ERROR_MEMORY;
    }
    publish_out(out, out_len);
    return YEPTRIS_OK;
}

/* YAML in -> a JSON array of every document in the stream (the
 * loadAll answer). An empty stream (and a failed-to-parse-with-OK
 * empty input, the engine's "zero documents" shape) answers "[]";
 * parse errors surface exactly like load's. */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_load_all(const char *data, size_t len) {
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(data, len, &st);
    if (doc == NULL) {
        if (st == YEPTRIS_OK) { /* empty/comment-only stream: no docs */
            return yeptris_wasm_load_all_answer(NULL, 0);
        }
        capture_error();
        return (int)st;
    }
    int rc = yeptris_wasm_load_all_answer(doc, yeptris_document_count(doc));
    yeptris_document_free(doc);
    return rc;
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
void yeptris_wasm_out_free(void) {
    yeptris_free(g_out);
    g_out = NULL;
    g_out_len = 0;
}

EMSCRIPTEN_KEEPALIVE
void yeptris_wasm_free(void *p) { yeptris_free(p); }

EMSCRIPTEN_KEEPALIVE
const char *yeptris_wasm_version(void) { return yeptris_version(); }
