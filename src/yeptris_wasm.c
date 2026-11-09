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
#include "yeptris/events.h"
#include "yeptris/json.h"
#include "yeptris/parse.h"
#include "yeptris/validate.h"
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

/* The dump-options face (emit.h's yeptris_emit_options): canonical
 * selects the engine's fixed form (flow collections, quoted strings,
 * typed words, shortest floats); best_width folds flow collections
 * after a value once the line exceeds it (<= 0 rides the library
 * default, 80 — the libyaml parity). The defaults (0, 0) are
 * byte-identical to the no-options yeptris_serialize: opts_width
 * answers 80 for best_width <= 0 and canonical 0 is the fidelity
 * mode. */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_dump_ex(const char *data, size_t len, int canonical, int best_width) {
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(data, len, &st);
    if (doc == NULL || st != YEPTRIS_OK) {
        capture_error();
        return (int)st;
    }
    yeptris_emit_options opts;
    memset(&opts, 0, sizeof opts);
    opts.size = (uint32_t)sizeof opts; /* the size-versioned contract */
    opts.canonical = canonical;
    opts.best_width = best_width;
    size_t out_len = 0;
    char *out = yeptris_serialize_ex(doc, &opts, &out_len);
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
    return yeptris_wasm_dump_ex(data, len, 0, 0);
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

/* ---- events (the recorder bulk drain) ----------------------------------
 *
 * The engine ships three event models over the one parse: push (a
 * callback per event), pull (a StAX cursor) and the recorder (fixed
 * 44-byte records plus one copied string arena). Across the wasm
 * boundary the RECORDER is the fit: the whole stream answers as two
 * spans — the record array and the arena — which the host reads
 * straight from the heap in one pass. That is O(1) crossings per
 * stream; push would cross once per event and pull would need a
 * cursor protocol whose strings die at the next call. This mirrors
 * exactly what the ctypes/FFI readers of the ruby/py/rs faces do with
 * the same record layout.
 *
 * Lifecycle is the load pattern: one C call (feed, final), the drain
 * retained in globals until the host has decoded it, then an explicit
 * free. */
static YeptrisRecorder g_ev_rec;
static size_t g_ev_count;
static size_t g_ev_arena_len;

EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_events(const char *data, size_t len) {
    if (g_ev_rec != NULL) { /* defensive: the previous drain was not freed */
        yeptris_recorder_free(g_ev_rec);
        g_ev_rec = NULL;
    }
    g_ev_count = 0;
    g_ev_arena_len = 0;
    YeptrisRecorder rec = yeptris_recorder_new();
    if (rec == NULL) {
        capture_plain("recorder allocation failed");
        return YEPTRIS_ERROR_MEMORY;
    }
    /* One chunk, final: the recorder v1 buffers to the stream end, so
     * a whole-input feed IS the drain (mid-stream feeds arrive with the
     * engine's resumable stepping and keep this shape). */
    YeptrisStatus st = yeptris_recorder_feed(rec, data, len, 1);
    if (st != YEPTRIS_OK) {
        uint32_t line = 0;
        uint32_t col = 0;
        yeptris_recorder_status(rec, &line, &col);
        /* the recorder carries status + 1-based position, no message
         * string (its errors are parse errors by construction) */
        snprintf(last_error, sizeof last_error, "parse failed");
        last_error_line = (int)line;
        last_error_column = (int)col;
        yeptris_recorder_free(rec);
        return (int)st;
    }
    g_ev_rec = rec;
    yeptris_recorder_records(rec, &g_ev_count);
    yeptris_recorder_arena(rec, &g_ev_arena_len);
    return YEPTRIS_OK;
}

EMSCRIPTEN_KEEPALIVE
const YeptrisEventRecord *yeptris_wasm_event_records(void) {
    return g_ev_rec != NULL ? yeptris_recorder_records(g_ev_rec, NULL) : NULL;
}

EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_event_count(void) { return (int)g_ev_count; }

EMSCRIPTEN_KEEPALIVE
const char *yeptris_wasm_event_arena(void) {
    return g_ev_rec != NULL ? yeptris_recorder_arena(g_ev_rec, NULL) : NULL;
}

EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_event_arena_len(void) { return (int)g_ev_arena_len; }

EMSCRIPTEN_KEEPALIVE
void yeptris_wasm_events_free(void) {
    yeptris_recorder_free(g_ev_rec);
    g_ev_rec = NULL;
    g_ev_count = 0;
    g_ev_arena_len = 0;
}

/* ---- the tolerant per-document stream iterator (parse.h) ----------------
 *
 * loadAll is all-or-nothing: one bad document fails the whole stream.
 * The stream iterator walks document by document: a parse-class failure
 * surfaces as ONE failed item (the failed document's own line/col via
 * the iterator's status accessor) and the walk RESUMES at the next
 * line-start marker — the valid documents after a bad one are kept.
 * The MEMORY/ENCODING classes are stream-fatal; the iterator is done.
 *
 * The wasm boundary needs a RETAINED walk (one next() per JS call),
 * which means the shim retains two things across calls: the iterator
 * handle and a COPY of the input — the engine iterator only borrows
 * its buffer until free, and the host's feed pointer dies when the
 * JS call that opened the walk returns. Lifecycle mirrors the events
 * face: open (feed, copy, construct), next() repeatedly, close always
 * (the JS generator's finally is the owner).
 *
 * The base four functions (new/next/status/free) — this face ships
 * against the v0.6.60 engine, whose iterator predates the slice
 * accessors; the frame document crosses as JSON through the same
 * g_out discipline every answer uses (a frame is a dcount==1 forest,
 * so serialize_json answers it directly — the load path).
 *
 * The options door (ruby/py parity): open_ex carries (schema_kind,
 * max_depth) and fills the engine's YeptrisParseOptions for the walk —
 * the resolver family every frame types through and the per-frame
 * nesting cap. The options struct is RETAINED (g_it_opts): the engine
 * iterator only borrows its options (parse.h) and dereferences them
 * on every next(), so a stack copy would die when open returns.
 * schema_kind < 0 keeps the NULL ride — the engine defaults, the v1
 * contract — and zeroed fields are the parse defaults, so only the
 * two live fields ride (strict/tab_policy/recover stay reserved pins).
 * The door is a NEW export, never a signature change of the v1 open
 * above: this wasm surface's ABI is append-only, and the host probes
 * the door's presence instead of trusting an export's arity. */
static yeptris_stream_iter *g_it;
static char *g_it_buf; /* the borrowed input's retained copy */
static size_t g_it_buf_len;
static int g_it_frame;  /* 1: g_out carries a document JSON; 0: none */
static int g_it_index;  /* the last next() step's consumed stream index */
static YeptrisParseOptions g_it_opts; /* borrowed by the iterator */

static int each_open_common(const char *data, size_t len, int schema_kind, int max_depth) {
    if (g_it != NULL) { /* defensive: the previous walk was not closed */
        yeptris_stream_iter_free(g_it);
        g_it = NULL;
    }
    free(g_it_buf);
    g_it_buf = NULL;
    g_it_buf_len = 0;
    g_it_frame = 0;
    g_it_index = 0;
    g_it_buf = (char *)malloc(len > 0 ? len : 1);
    if (g_it_buf == NULL) {
        capture_plain("stream iterator allocation failed");
        return YEPTRIS_ERROR_MEMORY;
    }
    if (len > 0) {
        memcpy(g_it_buf, data, len);
    }
    g_it_buf_len = len;
    if (schema_kind < 0) {
        /* the v1 contract: NULL opts rides the engine defaults */
        g_it = yeptris_stream_iter_new(g_it_buf, len, NULL);
    } else {
        memset(&g_it_opts, 0, sizeof g_it_opts); /* zero = the parse defaults */
        g_it_opts.schema = (YeptrisSchema)schema_kind;
        g_it_opts.max_depth = max_depth;
        g_it = yeptris_stream_iter_new(g_it_buf, len, &g_it_opts);
    }
    if (g_it == NULL) {
        free(g_it_buf);
        g_it_buf = NULL;
        g_it_buf_len = 0;
        capture_plain("stream iterator allocation failed");
        return YEPTRIS_ERROR_MEMORY;
    }
    return YEPTRIS_OK;
}

/* The v1 open: NULL opts — the engine defaults, the face's shipped
 * contract, byte-identical on every dist era. */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_each_open(const char *data, size_t len) {
    return each_open_common(data, len, -1, 0);
}

/* The options door (the v1 contract grown, not changed — see above). */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_each_open_ex(const char *data, size_t len, int schema_kind, int max_depth) {
    return each_open_common(data, len, schema_kind, max_depth);
}

EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_each_next(void) {
    if (g_it == NULL) {
        capture_plain("the walk is closed");
        return YEPTRIS_ERROR_ARG;
    }
    YeptrisDocument doc = NULL;
    size_t index = 0;
    YeptrisStatus st = yeptris_stream_iter_next(g_it, &doc, &index);
    g_it_index = (int)index;
    if (st == YEPTRIS_OK && doc == NULL) { /* the stream is over */
        g_it_frame = 0;
        return YEPTRIS_OK;
    }
    if (st == YEPTRIS_OK) {
        size_t out_len = 0;
        char *out = yeptris_serialize_json(doc, &out_len);
        yeptris_document_free(doc);
        if (out == NULL) {
            capture_plain("serialization failed");
            return YEPTRIS_ERROR_MEMORY;
        }
        publish_out(out, out_len);
        g_it_frame = 1;
        return YEPTRIS_OK;
    }
    /* A failed document (parse/depth class; the walk may continue) or
     * a stream-fatal verdict (MEMORY/ENCODING; it may not). The why
     * rides the engine's error channel; the position rides the
     * iterator's per-document status accessor — the authoritative
     * source for the failed document's own mark. */
    g_it_frame = 0;
    capture_error();
    uint32_t line = 0;
    uint32_t col = 0;
    yeptris_stream_iter_status(g_it, &line, &col);
    last_error_line = (int)line;
    last_error_column = (int)col;
    return (int)st;
}

/* 1 when the last successful next() published a document JSON into
 * g_out; 0 when the stream is over (the JS door's end sentinel). */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_each_frame(void) { return g_it_frame; }

/* The 0-based stream index the last next() consumed — failures
 * consume an index too, so the yield order is the stream's. */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_each_index(void) { return g_it_index; }

EMSCRIPTEN_KEEPALIVE
void yeptris_wasm_each_close(void) {
    yeptris_stream_iter_free(g_it);
    g_it = NULL;
    free(g_it_buf);
    g_it_buf = NULL;
    g_it_buf_len = 0;
    g_it_frame = 0;
    g_it_index = 0;
}

/* ---- validation (validate.h) -------------------------------------------
 *
 * The engine ships three validation doors — the SCHEMA-FAMILY walk
 * (yeptris_validate, the YeptrisSchema kind rides as a plain int
 * parameter), the one-shot JSON SCHEMA walk (yeptris_json_schema_
 * validate) and the COMPILED economy (yeptris_schema_compile /
 * yeptris_schema_validate / yeptris_schema_free: parse the schema
 * once, validate many streams).
 *
 * Diagnostics cross as DATA, never as pointers: the whole report
 * drains once into the same g_out arena the JSON answers use, spelled
 * as one JSON array of raw records
 *
 *   {"code":N,"path":"...","message":"...","line":N,"col":N,"offset":N}
 *
 * `path` is the engine's JSON-pointer instance path ("/a/b/0", ~0/~1
 * escaped; "" at the root), `message` its bounded detail. The
 * code -> keyword mapping is binding-owned (the same law the ruby/py
 * faces follow) and lives in src/index.js — the shim carries the raw
 * record only. The report frees inside the shim on every path; the
 * JS answer arrives via the g_out discipline (read + out_free), so
 * nothing engine-owned survives the call.
 *
 * Statuses through the wasm boundary: YEPTRIS_OK (valid) and
 * YEPTRIS_ERROR_SCHEMA (invalid: the report carries diagnostics) both
 * publish the array; every other status is a call failure —
 * last_error carries the why, g_out is untouched. The host reads
 * g_out for statuses 0 and 9 only.
 *
 * The compiled handle crosses as an opaque i32 (a wasm32 pointer):
 * the host stores it and hands it back; free rides
 * yeptris_wasm_schema_free. A compile failure answers 0 with
 * last_error set. The engine carries NO schema KIND on the compile
 * door (a compiled handle is a JSON-Schema document by
 * validate.h's contract) — the family kind selection lives on the
 * validate door, exactly as the C surface spells it. */

/* A growable append buffer for the report JSON. */
typedef struct {
    char *p;
    size_t len, cap;
    int failed;
} yw_buf;

static void yw_buf_grow(yw_buf *b, size_t need) {
    if (b->failed || b->len + need <= b->cap) {
        return;
    }
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + need) {
        ncap *= 2;
    }
    char *np = (char *)realloc(b->p, ncap);
    if (np == NULL) {
        free(b->p);
        b->p = NULL;
        b->failed = 1;
        return;
    }
    b->p = np;
    b->cap = ncap;
}

static void yw_buf_puts(yw_buf *b, const char *s) {
    size_t n = strlen(s);
    yw_buf_grow(b, n + 1);
    if (b->failed) {
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

/* Appends s as one JSON string literal: quote, escape (the bounded
 * detail text can carry quotes, backslashes and control characters —
 * the instance's own scalars appear in it), close. */
static void yw_buf_putjson(yw_buf *b, const char *s) {
    yw_buf_grow(b, 1);
    if (b->failed) {
        return;
    }
    b->p[b->len++] = '"';
    for (const unsigned char *c = (const unsigned char *)s; *c != '\0'; c++) {
        char esc[8];
        switch (*c) {
        case '"':
            yw_buf_puts(b, "\\\"");
            break;
        case '\\':
            yw_buf_puts(b, "\\\\");
            break;
        case '\b':
            yw_buf_puts(b, "\\b");
            break;
        case '\f':
            yw_buf_puts(b, "\\f");
            break;
        case '\n':
            yw_buf_puts(b, "\\n");
            break;
        case '\r':
            yw_buf_puts(b, "\\r");
            break;
        case '\t':
            yw_buf_puts(b, "\\t");
            break;
        default:
            if (*c < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*c);
                yw_buf_puts(b, esc);
            } else {
                yw_buf_grow(b, 1);
                if (b->failed) {
                    return;
                }
                b->p[b->len++] = (char)*c;
            }
            break;
        }
        if (b->failed) {
            return;
        }
    }
    yw_buf_grow(b, 2);
    if (b->failed) {
        return;
    }
    b->p[b->len++] = '"';
    b->p[b->len] = '\0';
}

static void yw_buf_putint(yw_buf *b, long v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%ld", v);
    yw_buf_puts(b, tmp);
}

/* Drains the whole report into g_out as the JSON array and frees the
 * report — exactly once, on every path. rep may be NULL (an empty
 * answer: the parse-failed statuses never reach here, but an engine
 * that hands back no report for a valid empty stream does). */
static int yeptris_wasm_publish_report(yeptris_validate_report *rep) {
    size_t n = rep != NULL ? yeptris_validate_report_count(rep) : 0;
    yw_buf b;
    memset(&b, 0, sizeof b);
    yw_buf_puts(&b, "[");
    for (size_t i = 0; i < n; i++) {
        const yeptris_diagnostic *d = yeptris_validate_report_at(rep, i);
        if (d == NULL) {
            break;
        }
        if (i != 0) {
            yw_buf_puts(&b, ",");
        }
        yw_buf_puts(&b, "{\"code\":");
        yw_buf_putint(&b, (long)d->code);
        yw_buf_puts(&b, ",\"path\":");
        yw_buf_putjson(&b, d->path);
        yw_buf_puts(&b, ",\"message\":");
        yw_buf_putjson(&b, d->detail);
        yw_buf_puts(&b, ",\"line\":");
        yw_buf_putint(&b, (long)d->line);
        yw_buf_puts(&b, ",\"col\":");
        yw_buf_putint(&b, (long)d->col);
        yw_buf_puts(&b, ",\"offset\":");
        yw_buf_putint(&b, (long)d->offset);
        yw_buf_puts(&b, "}");
    }
    yw_buf_puts(&b, "]");
    yeptris_validate_report_free(rep);
    if (b.failed) {
        return YEPTRIS_ERROR_MEMORY;
    }
    /* Ownership moves to g_out: yeptris_wasm_out_free releases it via
     * yeptris_free (plain free) — the same allocator this realloc/malloc
     * pair rides, so the hand-off is symmetric. */
    publish_out(b.p, b.len);
    return YEPTRIS_OK;
}

/* The status a validate call answers with: OK and ERROR_SCHEMA both
 * publish the report; the caller (JS) reads g_out for those two and
 * treats the rest as a call failure. */
static int yeptris_wasm_validate_status(YeptrisStatus st, yeptris_validate_report *rep) {
    if (st != YEPTRIS_OK && st != YEPTRIS_ERROR_SCHEMA) {
        capture_error();
        return (int)st;
    }
    if (yeptris_wasm_publish_report(rep) != 0) {
        capture_plain("diagnostic serialization failed");
        return YEPTRIS_ERROR_MEMORY;
    }
    return (int)st;
}

/* The SCHEMA-FAMILY walk: schema_kind is YeptrisSchema (resolve.h) —
 * the engine validates that the kind IS a validation family and
 * answers YEPTRIS_ERROR_ARG otherwise. */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_validate(const char *data, size_t len, int schema_kind) {
    yeptris_validate_report *rep = NULL;
    YeptrisStatus st = yeptris_validate(data, len, (YeptrisSchema)schema_kind, &rep);
    return yeptris_wasm_validate_status(st, rep);
}

/* The one-shot JSON SCHEMA walk (draft 2020-12 subset; validate.h). */
EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_json_schema_validate(const char *schema, size_t schema_len, const char *data,
                                      size_t len) {
    yeptris_validate_report *rep = NULL;
    YeptrisStatus st = yeptris_json_schema_validate(schema, schema_len, data, len, &rep);
    return yeptris_wasm_validate_status(st, rep);
}

/* The COMPILED economy. The handle is an opaque wasm32 pointer the
 * host stores verbatim; 0 = compile failed (last_error carries the
 * why). */
EMSCRIPTEN_KEEPALIVE
yeptris_schema *yeptris_wasm_schema_compile(const char *bytes, size_t n) {
    yeptris_schema *s = yeptris_schema_compile(bytes, n);
    if (s == NULL) {
        capture_error();
    }
    return s;
}

EMSCRIPTEN_KEEPALIVE
int yeptris_wasm_schema_validate(yeptris_schema *s, const char *data, size_t len) {
    yeptris_validate_report *rep = NULL;
    YeptrisStatus st = yeptris_schema_validate(s, data, len, &rep);
    return yeptris_wasm_validate_status(st, rep);
}

EMSCRIPTEN_KEEPALIVE
void yeptris_wasm_schema_free(yeptris_schema *s) { yeptris_schema_free(s); }
