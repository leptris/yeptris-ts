/* engine.c — the block machine + flow kernel (TODO.impl/07).
 *
 * One engine, two kernels: block context is a line/indent loop over an
 * explicit frame stack; '[' or '{' switches to the iterative flow kernel.
 * No recursion anywhere (depth = frame stack depth, guarded).
 *
 * Key invariants:
 *  - MAP_START always precedes its key events, including flow keys
 *    (a flow node that turns out to be a key is pre-scanned with
 *    e_skip_flow before its events are emitted);
 *  - a collection frame is pushed exactly once: sibling entries at the
 *    frame's column reuse the open frame;
 *  - anchors defined before use (engine tracks names; the DOM binds nodes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/chartype.h"
#include "common/nametab.h"
#include "common/simd_text.h"
#include "engine.h"
#include "memory/allocator.h"
#include "memory/pool.h"
#include "parse/scalars.h"
#include "resolve/resolver.h"
#include "scan/json.h"
#include "scan/scan.h"
#include <yeptris/resolve.h> /* YEPTRIS_TAG_* names (#168) */

#define YEP_MAX_DEPTH 1000
#define YEP_MAX_FOLD_LINES 8192
#define YEP_MAX_SIMPLE_KEY                                                                         \
    1024 /* YAML 1.2: simple keys are one line,                                                    \
          * at most 1024 characters (libyaml parity) */

typedef enum { YEP_FRAME_SEQ = 0, YEP_FRAME_MAP } yep_frame_kind;

typedef enum {
    YEP_CTX_FRESH = 0,   /* fresh line entry: may open collections */
    YEP_CTX_AFTER_DASH,  /* value of a seq entry: may open collections (compact) */
    YEP_CTX_AFTER_COLON, /* value of a map key: collections via following lines only */
    YEP_CTX_AFTER_Q,     /* explicit-key content: may ALIGN with the '?' column */
    YEP_CTX_VALUE_LINE,  /* value of "key:" on a FOLLOWING line: indentless
                          * sequences allowed, same-line prohibitions lifted */
} yep_ctx;

/* One open collection frame (the engine's indentation stack). */
typedef struct {
    uint16_t col;
    uint8_t kind;
    uint8_t inline_doc; /* opened on the --- line: deeper keys only */
} yep_frame;

struct yep_engine {
    const yep_allocator* sys;
    yep_pool* pool; /* finish pool: folded/escaped scalar content */
    const char* p;
    size_t len;
    size_t pos;
    uint32_t line;              /* 1-based current line */
    size_t line_start;          /* offset of the current line start */
    yep_line_info li_cache;     /* per-line scan_line memo: every site
           positioned at line_start reads it (plain continuation checks,
           literal blocks, the main loop), so a line is scanned once */
    uint32_t li_cache_line;     /* 0 = no entry (line numbers are 1-based) */
    yep_line_shape shape_cache; /* per-line shape memo (TODO.restructure/49):
                                   the dispatch decision is made once */
    uint32_t shape_cache_line;  /* 0 = no entry */
    uint32_t line_base;         /* lines consumed by earlier stepped runs */
    yep_error err;
    const yep_sink* sink;

    yep_frame frames[YEP_MAX_DEPTH];
    int depth;

    yep_nametab anchors;    /* name interning: O(1) define/lookup at any scale */
    uint32_t anchor_serial; /* 1-based ordinals; the value IS the id */
    /* repeat-alias memo (TODO.restructure/71): offset/len/ordinal of
     * the last alias resolved — hit by memcmp, not by the table */
    uint32_t alias_memo_at;
    uint32_t alias_memo_len;
    uint32_t alias_memo_ord;
    /* the line-facts memo (TODO.restructure/76): one kernel pass per
     * line feeds both the line info and the shape */
    yep_line_facts facts_cache;
    uint32_t facts_cache_line;

    yep_fold_line fold[YEP_MAX_FOLD_LINES];
    size_t fold_n;

    /* properties that belong to a value parsed on following lines */
    yep_view pend_anchor, pend_tag;
    uint32_t pend_anchor_id; /* the pend anchor's ordinal */

    int doc_content;   /* any node emitted for the current document */
    int parsing_final; /* engine_run_impl's emit_end: the buffer is the
                        * complete input (pre-scan errors are real) vs a
                        * partial step feed (an unclosed flow is
                        * incomplete, not malformed) */
    int q_key_pending; /* an explicit '?' key awaits its ':' value line */
    int qmd_stack[16]; /* the '?' pending pairs' map depths — a STACK:
                        * nested '? ?' pairs stack two pendings and a
                        * scalar slot let the inner clobber the
                        * outer's level (the nightly's run 38387614146) */
    int qmd_n;
    int q_value_pending; /* an explicit key was emitted; its value is null
                            until a ':' line supplies one */
    /* %TAG handle map for the current document (engine = tag SSOT) */
    struct {
        yep_view handle, prefix;
    } tagmap[8];
    int tagmap_n;
    int saw_yaml;     /* %YAML seen for the pending document */
    size_t doc_begin; /* #179: first directive line of the pending doc
                       * (SIZE_MAX = none) — libyaml's DOCUMENT_START
                       * start_mark includes directives */
    void* step;       /* yep_stepstate: resumable stepping (07) */

    /* Flow single-pair deferral: a sequence entry's events are buffered
     * until we know whether ':' follows ("[a: b]" needs MAP_START before
     * the key's events, but ':' is only seen after them). */
    yep_event* ev_buf;
    uint32_t ev_buf_n, ev_buf_cap;
    uint32_t ev_scopes;                  /* active entry-buffer scopes */
    char* norm_buf;                      /* owned input copy with exotic breaks normalized */
    const struct yep_resolver* resolver; /* implicit-typing schema (10) */
    int max_depth;                       /* runtime nesting limit (options) */
    uint16_t rec;                        /* e_node<->e_parse_value call depth:
                                            the props/value-line chain opens NO
                                            container frame, so e->depth cannot
                                            bound it (the nightly's stack
                                            overflow crasher) */
    int last_root_flow;                  /* a root-level flow node just completed */
    int tag_undef;                       /* an unresolved !handle! was seen (QLJ7) */
    int doc_inline;                      /* content node opened on the --- line */
    uint16_t flow_floor;                 /* parent column of the current block-level flow */
    int flow_enforce;                    /* flow continuation lines must out-indent it */
    int compat_grammar;                  /* 11-COMPAT: libyaml/psych grammar parity —
                                          * the flow indent floor (9C9N/VJP3) is a
                                          * yaml-test-suite rule libyaml never
                                          * enforced, and real-world locale files
                                          * close multi-line flows at the parent's
                                          * column; the floor stays on for the
                                          * strict 1.2 engine the suite runs */
};

/* ------------------------------------------------------------ forwards */

static int e_node(yep_engine* e, yep_ctx ctx, uint16_t floor_col);
static int e_parse_value(yep_engine* e, yep_ctx ctx, uint16_t floor_col);

/* ---------------------------------------------------------------- util */

static uint32_t e_col(const yep_engine* e, size_t at) {
    return at >= e->line_start ? (uint32_t)(at - e->line_start) : 0;
}

/* #179: the (1-based line, 1-based col) of a byte offset. Unlike
 * e_col this handles offsets on PRIOR lines — event ends whose scan
 * already advanced past them (block scalars, collection closes).
 * The backward walk covers only the span's own lines, so the total
 * across a document stays linear in its size. */
static void e_pos_mark(const yep_engine* e, size_t at, uint32_t* line, uint32_t* col) {
    if (at >= e->line_start) {
        *line = e->line;
        *col = (uint32_t)(at - e->line_start) + 1;
        return;
    }
    uint32_t l = e->line;
    size_t ls = e->line_start;
    while (at < ls && ls > 0) {
        /* p[ls-1] is the newline ENDED the previous line: start the
         * walk below it, or ls never decreases (the infinite-loop bug
         * the Stepping.Directive hang found) */
        size_t q = ls - 1;
        while (q > 0 && e->p[q - 1] != '\n') {
            q--;
        }
        ls = q; /* line_start(l-1) */
        l--;
    }
    *line = l;
    *col = (uint32_t)(at - ls) + 1;
}

static int e_fail(yep_engine* e, yep_err_code code, size_t at) {
    yep_error_set(&e->err, code, e->line, e_col(e, at) + 1, at, NULL);
    return -1;
}

/* A simple key (no explicit '?') is limited to one line and 1024
 * characters. The span runs from the node's first byte (opening
 * quote, alias star — not its anchor/tag properties) to the ':'. */
static int e_simple_key_ok(yep_engine* e, size_t key_at, size_t colon_at) {
    if (colon_at - key_at > YEP_MAX_SIMPLE_KEY) {
        return e_fail(e, YEP_ERR_KEY_TOO_LONG, key_at);
    }
    return 0;
}

static void e_event_init(yep_event* ev, yep_event_type t) {
    memset(ev, 0, sizeof(*ev));
    ev->type = t;
}

static int emit_now(yep_engine* e, const yep_event* ev) {
    if (ev->type == YEP_EV_SCALAR) {
        /* Resolution happens exactly here: every scalar path flows
         * through this choke point (the resolver is the typing SSOT) */
        if (ev->tag.len > 0) {
            yep_tag_id tid = yep_tag_from_uri((const char*)ev->tag.p, ev->tag.len);
            /* psych's own dumped form is the LOCAL !binary shorthand,
             * and stdlib to_ruby accepts it beside the core URI (#168)
             * — compat schemas resolve it to BINARY as well */
            if (tid == YEPTRIS_TAG_CUSTOM && ev->tag.len == 7 &&
                memcmp(ev->tag.p, "!binary", 7) == 0 && e->resolver == yep_resolver_compat11()) {
                tid = YEPTRIS_TAG_BINARY;
            }
            ((yep_event*)ev)->tag_id = tid;
        } else if (ev->implicit && e->resolver != NULL) {
            ((yep_event*)ev)->tag_id = e->resolver->resolve(
                e->resolver->ctx, (const char*)ev->value.p, (uint32_t)ev->value.len);
        } else {
            ((yep_event*)ev)->tag_id = 0; /* str: quoted or tag-free */
        }
    }
    if (ev->type == YEP_EV_SCALAR && e->ev_scopes == 0 && e->sink != NULL &&
        e->sink->on_scalar != NULL) {
        return e->sink->on_scalar(e->sink->ctx, &ev->value, &ev->tag, &ev->anchor, ev->anchor_id,
                                  ev->tag_id, ev->style, ev->implicit, ev->flow, ev->borrowed);
    }
    if (e->ev_scopes > 0) {
        if (e->ev_buf_n == e->ev_buf_cap) {
            uint32_t cap = e->ev_buf_cap ? e->ev_buf_cap * 2 : 16;
            yep_event* nb = yep_pool_alloc(e->pool, (size_t)cap * sizeof(yep_event), 16);
            if (nb == NULL) {
                e_fail(e, YEP_ERR_MEMORY, e->pos);
                return -2;
            }
            if (e->ev_buf_n > 0) {
                memcpy(nb, e->ev_buf, (size_t)e->ev_buf_n * sizeof(yep_event));
            }
            e->ev_buf = nb;
            e->ev_buf_cap = cap;
        }
        e->ev_buf[e->ev_buf_n++] = *ev;
        return 0;
    }
    return e->sink ? e->sink->on_event(e->sink->ctx, ev) : 0;
}

/* Emits buffered events from `from` to the sink and truncates. */
static int e_buf_send(yep_engine* e, uint32_t from) {
    for (uint32_t i = from; i < e->ev_buf_n; i++) {
        if (e->sink && e->sink->on_event(e->sink->ctx, &e->ev_buf[i]) != 0) {
            return -2;
        }
    }
    e->ev_buf_n = from;
    return 0;
}

/* Inserts a flow MAP_START at `from`, wrapping the buffered entry. */
static int e_buf_wrap(yep_engine* e, uint32_t from) {
    if (e->ev_buf_n >= e->ev_buf_cap) {
        uint32_t cap = e->ev_buf_cap ? e->ev_buf_cap * 2 : 16;
        yep_event* nb = yep_pool_alloc(e->pool, (size_t)cap * sizeof(yep_event), 16);
        if (nb == NULL) {
            e_fail(e, YEP_ERR_MEMORY, e->pos);
            return -2;
        }
        if (e->ev_buf_n > 0) {
            memcpy(nb, e->ev_buf, (size_t)e->ev_buf_n * sizeof(yep_event));
        }
        e->ev_buf = nb;
        e->ev_buf_cap = cap;
    }
    memmove(&e->ev_buf[from + 1], &e->ev_buf[from],
            (size_t)(e->ev_buf_n - from) * sizeof(yep_event));
    e_event_init(&e->ev_buf[from], YEP_EV_MAP_START);
    e->ev_buf[from].flow = 1;
    e->ev_buf_n++;
    return 0;
}

static void e_line_done(yep_engine* e, size_t at) {
    size_t br = yep_scan_break_len(e->p, e->len, at);
    if (br > 0) {
        e->pos = at + br;
        e->line++;
        e->line_start = e->pos;
        e->li_cache_line = 0;
        e->shape_cache_line = 0;
        e->facts_cache_line = 0;
    } else {
        e->pos = at;
    }
}

/* The line-facts memo (TODO.restructure/76): ONE kernel pass per
 * line; li and the shape both derive from it. */
static const yep_line_facts* e_facts_here(yep_engine* e) {
    if (e->facts_cache_line != e->line) {
        yep_scan_facts(e->p, e->len, e->line_start, &e->facts_cache);
        e->facts_cache_line = e->line;
        e->li_cache_line = 0; /* derived caches are stale now */
        e->shape_cache_line = 0;
    }
    return &e->facts_cache;
}

/* scan_line for the current line, memoized for the flow loop (the
 * loop head and the post-ws check each need it every line). */
static yep_line_info e_line_info_here(yep_engine* e) {
    const yep_line_facts* f = e_facts_here(e);
    if (e->li_cache_line != e->line) {
        yep_scan_line_f(e->p, e->len, e->line_start, f, &e->li_cache);
        e->li_cache_line = e->line;
    }
    return e->li_cache;
}

/* The line's classified shape, memoized beside the line facts: the
 * engine decides dispatch ONCE per line (TODO.restructure/49). */
static const yep_line_shape* e_shape_here(yep_engine* e) {
    const yep_line_facts* f = e_facts_here(e);
    e_line_info_here(e);
    if (e->shape_cache_line != e->line) {
        yep_scan_shape_f(e->p, e->len, &e->li_cache, f, &e->shape_cache);
        e->shape_cache_line = e->line;
    }
    return &e->shape_cache;
}

static void e_skip_inline_space(yep_engine* e) {
    while (e->pos < e->len && (e->p[e->pos] == ' ' || e->p[e->pos] == '\t')) {
        e->pos++;
    }
}

/* '#' only opens a comment when a blank (or nothing) precedes it. */
static int e_comment_at(const yep_engine* e) {
    return e->p[e->pos] == '#' &&
           (e->pos == 0 || e->p[e->pos - 1] == ' ' || e->p[e->pos - 1] == '\t' ||
            e->p[e->pos - 1] == '\n' || e->p[e->pos - 1] == '\r');
}

static int e_at_eol(yep_engine* e) {
    return e->pos >= e->len || e->p[e->pos] == '\n' || e->p[e->pos] == '\r' || e_comment_at(e);
}

static int e_skip_to_eol(yep_engine* e) {
    while (e->pos < e->len && e->p[e->pos] != '\n' && e->p[e->pos] != '\r') {
        e->pos++;
    }
    return 0;
}

/* ':' terminates a node here (blank/EOL after; flow indicators are
 * handled by the flow kernel's own check). */
static int e_colon_at(yep_engine* e, size_t at) {
    if (at >= e->len || e->p[at] != ':') {
        return 0;
    }
    size_t n = at + 1;
    return n >= e->len || e->p[n] == ' ' || e->p[n] == '\n' || e->p[n] == '\r';
}

/* -------------------------------------------------------------- anchors */

/* The ordinal the nametab assigned at define time (0 when absent). */
static uint32_t anchor_id_of(yep_engine* e, yep_view name) {
    /* Repeat-alias memo (TODO.restructure/71): merge-key YAML re-uses
     * the same alias thousands of times in a row; the memo answers
     * those with one input compare, skipping the hash + a likely
     * cache-missing probe into the (multi-MB, reserved) slots table. */
    if (e->alias_memo_len == name.len && e->alias_memo_len != 0 &&
        memcmp(e->p + e->alias_memo_at, name.p, name.len) == 0) {
        return e->alias_memo_ord;
    }
    uint32_t id = yep_nametab_get(&e->anchors, name);
    if (id == YEP_NAMETAB_NIL) {
        return 0;
    }
    e->alias_memo_at = (uint32_t)(name.p - e->p);
    e->alias_memo_len = name.len;
    e->alias_memo_ord = id;
    return id;
}

static uint32_t anchor_define(yep_engine* e, yep_view name) {
    /* Last definition wins (libyaml parity); OOM leaves the anchor
     * undefined so its alias errors. The VALUE is the anchor's 1-based
     * ordinal — sinks key on it and skip the name hash entirely. */
    uint32_t id = ++e->anchor_serial;
    yep_nametab_set(&e->anchors, name, id);
    return id;
}

/* --------------------------------------------------------------- frames */

/* Opens a collection frame unless the top frame already continues at
 * exactly this column (sibling entries reuse it). anchor/tag ride the
 * START event. Returns 0 on success. */
static int e_open_seq(yep_engine* e, uint16_t col, uint32_t line, uint32_t coln, yep_view anchor,
                      yep_view tag, uint32_t anchor_id) {
    if (e->depth > 0 && e->frames[e->depth - 1].kind == YEP_FRAME_SEQ &&
        e->frames[e->depth - 1].col == col) {
        return 0;
    }
    if (e->depth >= e->max_depth) {
        return e_fail(e, YEP_ERR_DEPTH, e->pos);
    }
    e->frames[e->depth].col = col;
    e->frames[e->depth].kind = YEP_FRAME_SEQ;
    e->frames[e->depth].inline_doc = (uint8_t)e->doc_inline;
    e->doc_inline = 0;
    e->depth++;
    yep_event ev;
    e_event_init(&ev, YEP_EV_SEQ_START);
    ev.anchor = anchor;
    ev.anchor_id = anchor_id;
    ev.tag = tag;
    ev.line = line;
    ev.col = coln;
    ev.end_line = line; /* block opens are zero-span (libyaml) — except
                         * the sequence at its parent mapping's column
                         * ("k:\n- a"), which ends after the dash */
    ev.end_col = coln;
    for (int i = e->depth - 1; i >= 0; i--) {
        if (e->frames[i].kind == YEP_FRAME_MAP) {
            if (col == e->frames[i].col) {
                ev.end_col = coln + 1;
            }
            break;
        }
    }
    return emit_now(e, &ev) == 0 ? 0 : -2;
}

/* silent: the sink's on_block_open built the node — push the frame
 * without emitting (TODO.restructure/57). */
static int e_open_map(yep_engine* e, uint16_t col, uint32_t line, uint32_t coln, yep_view anchor,
                      yep_view tag, uint32_t anchor_id, int silent) {
    if (e->depth > 0 && e->frames[e->depth - 1].kind == YEP_FRAME_MAP &&
        e->frames[e->depth - 1].col == col) {
        return 0;
    }
    if (e->depth >= e->max_depth) {
        return e_fail(e, YEP_ERR_DEPTH, e->pos);
    }
    e->frames[e->depth].col = col;
    e->frames[e->depth].kind = YEP_FRAME_MAP;
    e->frames[e->depth].inline_doc = (uint8_t)e->doc_inline;
    e->doc_inline = 0;
    e->depth++;
    if (silent) {
        return 0;
    }
    yep_event ev;
    e_event_init(&ev, YEP_EV_MAP_START);
    ev.anchor = anchor;
    ev.anchor_id = anchor_id;
    ev.tag = tag;
    ev.line = line;
    ev.col = coln;
    ev.end_line = line; /* block opens are zero-span (libyaml) */
    ev.end_col = coln;
    return emit_now(e, &ev) == 0 ? 0 : -2;
}

/* Emits the null value for an explicit key that never got its ':' line. */
static int e_flush_q_value(yep_engine* e) {
    if (!e->q_value_pending || e->qmd_n == 0) {
        return 0;
    }
    int top = e->qmd_stack[e->qmd_n - 1];
    if (e->depth != top) {
        return 0; /* the pending pair's map is not the current level */
    }
    e->qmd_n--;
    /* nested '?' pairs stack one pending per level — the flag stays up
     * while the stack still holds one, so the outer null flushes at its
     * own level (the nightly's run 38387614146: `? ? k : a` lost it) */
    e->q_value_pending = (e->qmd_n > 0);
    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.style = YEP_STYLE_PLAIN;
    ev.implicit = 1;
    return emit_now(e, &ev) == 0 ? 0 : -2;
}

/* The pair-value completion: every `key: value` arm funnels its value
 * parse through here. Under an AFTER_Q parse (a nested '?' pair whose
 * value arrived inline) the innermost '?' pending resolves — without
 * this pop the stack keeps a stale entry and the outer pair's null
 * flushes inside the wrong map (the nightly's run 38387614146). */
static int e_pair_value_done(yep_engine* e, yep_ctx ctx, uint16_t key_col) {
    int rc = e_parse_value(e, YEP_CTX_AFTER_COLON, key_col);
    if (rc >= 0 && ctx == YEP_CTX_AFTER_Q && e->qmd_n > 0 &&
        e->depth == e->qmd_stack[e->qmd_n - 1]) {
        e->qmd_n--; /* the inline value resolved the innermost '?' pending */
    }
    return rc;
}

static int e_close_to(yep_engine* e, int to_depth) {
    while (e->depth > to_depth) {
        /* the pending '?' value flushes once the key's structure closed */
        int rc = e_flush_q_value(e);
        if (rc != 0) {
            return rc;
        }
        if (e->q_value_pending && e->qmd_n > 0 && e->depth == e->qmd_stack[e->qmd_n - 1]) {
            continue; /* flushed the null; this frame closes next round */
        }
        e->depth--;
        yep_event ev;
        e_event_init(&ev,
                     e->frames[e->depth].kind == YEP_FRAME_SEQ ? YEP_EV_SEQ_END : YEP_EV_MAP_END);
        { /* libyaml: a block END's mark is the next token's position
           * (zero-span) — the first non-space at the cursor, or EOF */
            size_t at = e->pos;
            while (at < e->len && e->p[at] == ' ') {
                at++;
            }
            e_pos_mark(e, at, &ev.line, &ev.col);
            ev.end_line = ev.line;
            ev.end_col = ev.col;
        }
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
    }
    return 0;
}

/* ------------------------------------------------------------- scalars */

/* Quoted scalar at the opening quote; spans lines when needed. */
static int e_quoted_floor(yep_engine* e, yep_event* ev, uint16_t min_indent, int enforce) {
    unsigned char q = (unsigned char)e->p[e->pos];
    size_t content_start = e->pos + 1;
    int has_esc = 0;
    size_t close;
    {
        const yep_text_kernels* k = yep_text_active();
        ptrdiff_t r =
            k->quote_scan(e->p + content_start, e->len - content_start, (char)q, &has_esc);
        if (r >= 0) {
            close = content_start + (size_t)r;
        } else {
            /* unterminated on this span: manual multi-line scan */
            size_t i = content_start;
            close = e->len + 1;
            while (i < e->len) {
                if (q == '"' && e->p[i] == '\\') {
                    i += 2;
                    has_esc = 1;
                    continue;
                }
                if ((unsigned char)e->p[i] == q) {
                    if (q == '\'' && i + 1 < e->len && e->p[i + 1] == '\'') {
                        i += 2;
                        continue;
                    }
                    close = i;
                    break;
                }
                i++;
            }
            if (close == e->len + 1) {
                return e_fail(e, YEP_ERR_UNTERMINATED_QUOTE, e->pos);
            }
        }
    }

    uint32_t start = (uint32_t)content_start;
    uint32_t end = (uint32_t)close;
    int multiline = 0;
    uint32_t breaks = 0; /* '\n' occurrences, folded with detection */
    {
        const yep_text_kernels* k = yep_text_active();
        const yep_stopset* brk = &yep_break_stopset; /* scan's SSOT */
        size_t i = start;
        while (i < end) {
            ptrdiff_t r = k->stopset_find(brk, e->p + i, end - i);
            if (r < 0) {
                break; /* single-line span: one SIMD call decided it */
            }
            size_t at = i + (size_t)r;
            multiline = 1;
            if (e->p[at] == '\n') {
                breaks++;
            }
            /* each following line must be content, not a document marker,
             * and in block context must out-indent the parent (QB6E) */
            size_t br = yep_scan_break_len(e->p, e->len, at);
            yep_line_info ql = yep_scan_line(e->p, e->len, at + br);
            if (ql.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END | YEP_LF_DIRECTIVE)) {
                return e_fail(e, YEP_ERR_UNTERMINATED_QUOTE, at + br);
            }
            if (enforce && !(ql.flags & YEP_LF_BLANK) && ql.indent <= min_indent) {
                return e_fail(e, YEP_ERR_BAD_INDENT, at + br + ql.indent);
            }
            i = at + br;
        }
    }
    /* escape pre-validation (double quotes only: ' has no escapes);
     * quote_scan already reported whether any backslash exists */
    for (size_t i = (q == '"' && has_esc) ? start : end; i < end; i++) {
        if (e->p[i] != '\\' || i + 1 >= end) {
            continue;
        }
        unsigned char esc = (unsigned char)e->p[i + 1];
        if (esc == 'x' || esc == 'u' || esc == 'U') {
            /* hex escapes carry exact digit counts: 2/4/8 */
            size_t need = (esc == 'x') ? 2 : (esc == 'u') ? 4 : 8;
            if (i + 1 + need >= end) {
                return e_fail(e, YEP_ERR_INVALID_ESCAPE, i); /* truncated "\uDd" */
            }
            for (size_t k = 1; k <= need; k++) {
                if (!yep_ct_is((unsigned char)e->p[i + 1 + k], YEP_CT_HEXDIGIT)) {
                    return e_fail(e, YEP_ERR_INVALID_ESCAPE, i + 1 + k);
                }
            }
            if (esc == 'u') {
                uint32_t cp = 0;
                for (size_t k = 1; k <= 4; k++) {
                    cp = (cp << 4) | (uint32_t)hexval((unsigned char)e->p[i + 1 + k]);
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* high surrogate: the low escape must follow */
                    size_t j = i + 6; /* at the low escape's backslash */
                    uint32_t lo = 0;
                    if (j + 6 <= end && e->p[j] == '\\' && e->p[j + 1] == 'u') {
                        int lok = 1;
                        for (int k = 0; k < 4; k++) {
                            int hv = hexval((unsigned char)e->p[j + 2 + (size_t)k]);
                            if (hv < 0) {
                                lok = 0;
                                break;
                            }
                            lo = (lo << 4) | (uint32_t)hv;
                        }
                        if (lok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            i = j + 5; /* at the low escape's last digit */
                            continue;
                        }
                    }
                    return e_fail(e, YEP_ERR_INVALID_ESCAPE, i);
                }
                if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return e_fail(e, YEP_ERR_INVALID_ESCAPE, i); /* lone low */
                }
            }
            if (esc == 'U') {
                /* 8 hex digits reach far past U+10FFFF — the nightly's
                 * "\UA66DA66D" crasher: parse accepted an unencodable
                 * code point and the emitted bytes never re-parsed */
                uint32_t cp = 0;
                for (size_t k = 1; k <= 8; k++) {
                    cp = (cp << 4) | (uint32_t)hexval((unsigned char)e->p[i + 1 + k]);
                }
                if (cp > 0x10FFFF) {
                    return e_fail(e, YEP_ERR_INVALID_ESCAPE, i);
                }
            }
            i += 1 + need; /* the digits are not escapes themselves */
            continue;
        }
        if (esc == '\n' || esc == '\r') {
            continue; /* line continuation: checked by finish_double */
        }
        switch (esc) {
        case '0':
        case 'a':
        case 'b':
        case 't':
        case '\t':
        case 'n':
        case 'v':
        case 'f':
        case 'r':
        case 'e':
        case ' ':
        case '"':
        case '/':
        case '\\':
        case 'N':
        case '_':
        case 'L':
        case 'P':
            break;
        default:
            return e_fail(e, YEP_ERR_INVALID_ESCAPE, i);
        }
        i++; /* skip the escaped byte */
    }

    ev->style = (q == '\'') ? YEP_STYLE_SINGLE_QUOTED : YEP_STYLE_DOUBLE_QUOTED;
    if (q == '\'') {
        ev->multiline = (uint8_t)multiline;
        char* out = yep_finish_single(e->p, start, end, multiline, e->pool, &ev->value.len);
        if (out == NULL) {
            ev->value.p = e->p + start;
            ev->value.len = end - start; /* finish_single returns before writing len */
            ev->borrowed = 1;
        } else {
            ev->value.p = out;
            ev->borrowed = 0;
        }
    } else if (!has_esc && !multiline) {
        ev->value.p = e->p + start;
        ev->value.len = end - start;
        ev->borrowed = 1;
    } else {
        ev->multiline = (uint8_t)multiline;
        char* out = yep_finish_double(e->p, start, end, multiline, e->pool, &ev->value.len);
        if (out == NULL) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        ev->value.p = out;
        ev->borrowed = 0;
    }

    size_t after = close + 1;
    e->line += breaks;
    /* approximate: after a multi-line scalar the column resets */
    if (multiline) {
        size_t ls = close;
        while (ls > start && e->p[ls - 1] != '\n') {
            ls--;
        }
        e->line_start = ls;
    }
    e->pos = after;
    e_pos_mark(e, after, &ev->end_line, &ev->end_col);
    e_skip_inline_space(e);
    return 0;
}

/* Block scalar (| or >) with header, auto/explicit indent, chomping. */
static int e_block_scalar(yep_engine* e, yep_event* ev, int parent_col) {
    int folded = (e->p[e->pos] == '>');
    e->pos++;
    int chomp = 0;
    int explicit_indent = 0;
    int saw_digit = 0;
    int saw_blank = 0;
    while (e->pos < e->len && e->p[e->pos] != '\n' && e->p[e->pos] != '\r') {
        char c = e->p[e->pos];
        if (c == ' ' || c == '\t') {
            saw_blank = 1;
            e->pos++;
            continue;
        }
        if (e_comment_at(e)) {
            e_skip_to_eol(e);
            break;
        }
        if (saw_blank || (c == '-' && chomp != 0) || (c == '+' && chomp != 0) ||
            (c >= '1' && c <= '9' && saw_digit)) {
            /* libyaml: the params ride the indicator DIRECTLY (a
             * spaced "| 2" is trailing content, not a param — "did
             * not find expected comment or line break"), one
             * indentation digit and one chomp sign at most (the
             * nightly's run 38119775008 family) */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (c == '-') {
            chomp = 1;
        } else if (c == '+') {
            chomp = 2;
        } else if (c >= '1' && c <= '9') {
            explicit_indent = c - '0';
            saw_digit = 1;
        } else {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        e->pos++;
    }
    e_line_done(e, e->pos);

    int content_indent =
        explicit_indent ? (parent_col < 0 ? explicit_indent : parent_col + explicit_indent) : -1;
    e->fold_n = 0;
    uint32_t pending_breaks = 0;
    uint32_t trailing_breaks = 0;
    int saw_content = 0;
    int blank_tab = 0;
    int max_blank_indent = -1;

    while (e->pos < e->len) {
        /* trailing spaces after "|-" leave pos mid-line: scan from pos
         * (the header tail), never the memo's full line */
        yep_line_info li =
            e->pos == e->line_start ? e_line_info_here(e) : yep_scan_line(e->p, e->len, e->pos);
        int blank = (li.flags & YEP_LF_BLANK) != 0;
        if (!blank) {
            if (saw_content && parent_col < 0 &&
                (li.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END | YEP_LF_DIRECTIVE))) {
                break; /* root block ends at ---/.../% even at the content column */
            }
            if (content_indent < 0) {
                content_indent = li.indent;
                if (content_indent <= parent_col) {
                    break; /* no block content */
                }
            } else if (li.indent <= parent_col) {
                break; /* dedent ends the block */
            } else if (li.indent < content_indent && li.indent <= parent_col) {
                break;
            }
            if (li.indent < content_indent) {
                break;
            }
        }
        if (blank) {
            if (li.indent > 0 && memchr(e->p + li.offset, '\t', li.end - li.offset) != NULL) {
                /* a tab after leading spaces is CONTENT (Y79Y#2) */
                if (content_indent < 0) {
                    content_indent = li.indent;
                    if (content_indent <= parent_col) {
                        break;
                    }
                } else if (li.indent < content_indent) {
                    break;
                }
                if (li.indent >= content_indent) {
                    uint32_t from2 = li.offset + (uint32_t)content_indent;
                    if (e->fold_n >= YEP_MAX_FOLD_LINES) {
                        return e_fail(e, YEP_ERR_MEMORY, e->pos);
                    }
                    e->fold[e->fold_n].content.p = e->p + from2;
                    e->fold[e->fold_n].content.len = li.end > from2 ? li.end - from2 : 0;
                    e->fold[e->fold_n].breaks_before =
                        saw_content ? pending_breaks + 1 : pending_breaks;
                    e->fold[e->fold_n].more_indented = 1;
                    e->fold_n++;
                    saw_content = 1;
                    pending_breaks = 0;
                    trailing_breaks = 0;
                    e_line_done(e, li.end);
                    continue;
                }
            }
            /* a blank line whose spaces extend past the content indent is
             * CONTENT (kept verbatim); otherwise it is a pure break */
            if (content_indent >= 0 && li.indent > content_indent) {
                if (e->fold_n >= YEP_MAX_FOLD_LINES) {
                    return e_fail(e, YEP_ERR_MEMORY, e->pos);
                }
                e->fold[e->fold_n].content.p = e->p + li.offset + (uint32_t)content_indent;
                e->fold[e->fold_n].content.len =
                    li.end > li.offset + content_indent ? li.end - li.offset - content_indent : 0;
                e->fold[e->fold_n].breaks_before =
                    saw_content ? pending_breaks + 1 : pending_breaks;
                e->fold[e->fold_n].more_indented = 1;
                e->fold_n++;
                saw_content = 1;
                pending_breaks = 0;
                trailing_breaks = 0;
                {
                    size_t at = li.end;
                    size_t br = yep_scan_break_len(e->p, e->len, at);
                    if (br > 0) {
                        trailing_breaks = 1;
                        e->pos = at + br;
                        e->line++;
                        e->line_start = e->pos;
                        e->li_cache_line = 0;
                        e->shape_cache_line = 0;
                    } else {
                        e->pos = at;
                    }
                }
                continue;
            }
            if (!saw_content) {
                if (memchr(e->p + li.offset, '\t', li.end - li.offset) != NULL) {
                    blank_tab = 1;
                }
                if ((int)li.indent > max_blank_indent) {
                    max_blank_indent = (int)li.indent;
                }
            }
            pending_breaks++;
            e_line_done(e, li.end);
            continue;
        }
        uint32_t from = li.offset + (uint32_t)content_indent;
        if (e->fold_n >= YEP_MAX_FOLD_LINES) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        e->fold[e->fold_n].content.p = e->p + from;
        e->fold[e->fold_n].content.len = li.end > from ? li.end - from : 0;
        /* leading blank lines belong to the block content */
        e->fold[e->fold_n].breaks_before = saw_content ? pending_breaks + 1 : pending_breaks;
        e->fold[e->fold_n].more_indented =
            li.indent > content_indent ||
            (from < li.end && (e->p[from] == ' ' || e->p[from] == '\t'));
        e->fold_n++;
        saw_content = 1;
        pending_breaks = 0;
        trailing_breaks = 0;
        /* the break after this content line counts toward trailing */
        size_t at = li.end;
        size_t br = yep_scan_break_len(e->p, e->len, at);
        if (br > 0) {
            trailing_breaks = 1;
            e->pos = at + br;
            e->line++;
            e->line_start = e->pos;
        } else {
            e->pos = at;
        }
    }
    trailing_breaks += pending_breaks;
    if (saw_content ? (max_blank_indent > content_indent) : blank_tab) {
        /* a leading blank line out-indents the first content line; a
         * tab-only line before any content is an error (5LLU/Y79Y) */
        return e_fail(e, YEP_ERR_BAD_INDENT, e->pos);
    }

    char* out = yep_finish_block(e->fold, e->fold_n, folded, chomp, trailing_breaks, e->pool,
                                 &ev->value.len);
    ev->value.p = out; /* may be NULL for an empty block; len 0 */
    ev->borrowed = 0;
    ev->style = folded ? YEP_STYLE_FOLDED : YEP_STYLE_LITERAL;
    e_pos_mark(e, e->pos, &ev->end_line, &ev->end_col);
    return 0;
}

/* Multi-line plain scalar continuation in block context. */
static int e_plain_multiline(yep_engine* e, yep_span s0, uint32_t block_floor, yep_event* ev,
                             int root_ctx) {
    /* the caller already scanned this exact span for its colon/term
     * decision — the rescan doubled every plain value's scan cost */
    e->fold_n = 0;
    {
        e->fold[0].content.p = e->p + s0.start;
        e->fold[0].content.len = s0.end - s0.start;
        e->fold[0].breaks_before = 0;
        e->fold_n = 1;
        if (s0.term == YEP_TERM_COMMENT) {
            e->pos = s0.end;
            e_skip_to_eol(e);
            return 0; /* the comment ends the scalar here */
        }
    }
    uint32_t breaks = 0;
    for (;;) {
        if (e->pos >= e->len) {
            break;
        }
        /* trailing spaces/tabs after a piece are stripped before the
         * break — they must not inflate the break count */
        while (e->pos < e->len && (e->p[e->pos] == ' ' || e->p[e->pos] == '\t')) {
            e->pos++;
        }
        if (e->pos >= e->len) {
            break;
        }
        if (e_comment_at(e)) {
            e_skip_to_eol(e);
            break; /* a comment terminates the plain scalar */
        }
        if (e->pos >= e->len) {
            break;
        }
        breaks++;
        e_line_done(e, e->pos);
        yep_line_info li = e_line_info_here(e);
        if (li.flags & YEP_LF_BLANK) {
            /* Park at the line end: the loop's next break consumption
             * counts this blank line's own break. */
            e->pos = li.end;
            continue;
        }
        /* At document root there is no parent block, so continuation
         * lines may sit at column 0. */
        int too_shallow = root_ctx ? 0 : (li.indent <= block_floor);
        if (too_shallow || (li.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END))) {
            break; /* a directive-LIKE line is content (XLQ9) */
        }
        size_t cs = e->pos + li.indent;
        while (cs < e->len && (e->p[cs] == ' ' || e->p[cs] == '\t')) {
            cs++; /* continuation leading white space is stripped */
        }
        yep_span s = yep_scan_plain(e->p, e->len, cs, 0);
        if (s.term == YEP_TERM_COLON) {
            return e_fail(e, YEP_ERR_UNEXPECTED, s.end);
        }
        if (e->fold_n >= YEP_MAX_FOLD_LINES) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        e->fold[e->fold_n].content.p = e->p + s.start;
        e->fold[e->fold_n].content.len = s.end - s.start;
        e->fold[e->fold_n].breaks_before = breaks;
        e->fold_n++;
        breaks = 0;
        e->pos = s.end;
        if (s.term == YEP_TERM_COMMENT) {
            e_skip_to_eol(e);
            break; /* the comment ends the scalar; later lines are new */
        }
    }
    { /* #179: the source end = the last piece's content end */
        const yep_view* last = &e->fold[e->fold_n - 1].content;
        size_t lend = (size_t)(last->p - e->p) + last->len;
        e_pos_mark(e, lend, &ev->end_line, &ev->end_col);
    }
    if (e->fold_n == 1) {
        return 0; /* single line: the borrowed view stands */
    }
    char* out = yep_fold_plain(e->fold, e->fold_n, e->pool, &ev->value.len);
    if (out == NULL) {
        return e_fail(e, YEP_ERR_MEMORY, e->pos);
    }
    ev->value.p = out;
    ev->borrowed = 0;
    return 0;
}

/* ---------------------------------------------------------------- props */

/* Resolves a scanned tag against the document's %TAG map. !! is the
 * YAML shorthand prefix; !<...> is verbatim; matched handles compose
 * prefix+suffix in the finish pool. */
static int e_hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Tags carry URI escapes (%XX) in every form (verbatim, !!, %TAG). */
static yep_view e_tag_uri_decode(yep_engine* e, yep_view v) {
    const char* p = v.p;
    /* every '%' must be followed by two hex digits — libyaml rejects
     * the family ("did not find URI escaped octet"); a stray '%' left
     * raw would decode differently on re-parse of the re-encoded form
     * (the nightly's tag-escape byte-instability) */
    for (uint32_t i = 0; i < v.len; i++) {
        if (p[i] == '%' && (i + 2 >= v.len || e_hex_val((unsigned char)p[i + 1]) < 0 ||
                            e_hex_val((unsigned char)p[i + 2]) < 0)) {
            e->tag_undef = 1; /* e_props reports it */
            return v;
        }
    }
    for (uint32_t i = 0; i + 2 < v.len; i++) {
        if (p[i] == '%' && e_hex_val((unsigned char)p[i + 1]) >= 0 &&
            e_hex_val((unsigned char)p[i + 2]) >= 0) {
            char* out = yep_pool_alloc(e->pool, v.len, 16);
            if (out == NULL) {
                return v;
            }
            uint32_t o = 0;
            for (uint32_t j = 0; j < v.len;) {
                if (j + 2 < v.len && p[j] == '%' && e_hex_val((unsigned char)p[j + 1]) >= 0 &&
                    e_hex_val((unsigned char)p[j + 2]) >= 0) {
                    out[o++] = (char)(e_hex_val((unsigned char)p[j + 1]) * 16 +
                                      e_hex_val((unsigned char)p[j + 2]));
                    j += 3;
                } else {
                    out[o++] = p[j++];
                }
            }
            yep_view d = {out, o};
            return d;
        }
    }
    return v;
}

static yep_view e_resolve_tag_raw(yep_engine* e, yep_view tag) {
    if (tag.len >= 3 && tag.p[0] == '!' && tag.p[1] == '<' && tag.p[tag.len - 1] == '>') {
        yep_view v = {tag.p + 2, tag.len - 3};
        return v;
    }
    /* longest matching handle wins */
    int best = -1;
    uint32_t best_len = 0;
    for (int i = 0; i < e->tagmap_n; i++) {
        if (tag.len >= e->tagmap[i].handle.len && e->tagmap[i].handle.len > best_len &&
            memcmp(tag.p, e->tagmap[i].handle.p, e->tagmap[i].handle.len) == 0) {
            best = i;
            best_len = e->tagmap[i].handle.len;
        }
    }
    if (tag.len >= 2 && tag.p[0] == '!' && tag.p[1] == '!' && best < 0) {
        static const char yns[] = "tag:yaml.org,2002:";
        size_t plen = sizeof(yns) - 1;
        char* out = yep_pool_alloc(e->pool, plen + tag.len - 2, 16);
        if (out == NULL) {
            return tag;
        }
        memcpy(out, yns, plen);
        memcpy(out + plen, tag.p + 2, tag.len - 2);
        yep_view v = {out, (uint32_t)(plen + tag.len - 2)};
        return v;
    }
    if (best >= 0) {
        const yep_view* h = &e->tagmap[best].handle;
        const yep_view* pfx = &e->tagmap[best].prefix;
        size_t n = pfx->len + tag.len - h->len;
        char* out = yep_pool_alloc(e->pool, n ? n : 1, 16);
        if (out == NULL) {
            return tag;
        }
        memcpy(out, pfx->p, pfx->len);
        memcpy(out + pfx->len, tag.p + h->len, tag.len - h->len);
        yep_view v = {out, (uint32_t)n};
        return v;
    }
    /* "!word!suffix" with no %TAG entry for the handle is undefined */
    for (uint32_t k = 1; k + 1 < tag.len; k++) {
        if (tag.p[k] == '!') {
            yep_view h = {tag.p, k + 1};
            char* m = yep_pool_alloc(e->pool, h.len, 16);
            if (m != NULL) {
                memcpy(m, h.p, h.len);
                yep_view hv = {m, h.len};
                yep_view res = e_resolve_tag_raw(e, hv);
                if (res.p == hv.p) {
                    e->tag_undef = 1; /* e_props reports it */
                }
            }
            break;
        }
    }
    return tag;
}

static yep_view e_resolve_tag(yep_engine* e, yep_view tag) {
    return e_tag_uri_decode(e, e_resolve_tag_raw(e, tag));
}

/* Parses &anchor / !tag runs at the cursor into *anchor / *tag. */
static void e_props(yep_engine* e, yep_view* anchor, yep_view* tag) {
    for (;;) {
        e_skip_inline_space(e);
        if (e->pos >= e->len) {
            return;
        }
        unsigned char c = (unsigned char)e->p[e->pos];
        if (c == '&') {
            size_t start = e->pos + 1;
            e->pos = yep_scan_prop_end(e->p, e->len, start);
            anchor->p = e->p + start;
            anchor->len = (uint32_t)(e->pos - start);
            if (anchor->len == 0) {
                /* libyaml requires one or more anchor characters
                 * ("did not find expected alphabetic or numeric
                 * character"); the nightly fuzz_roundtrip crasher,
                 * a lone '&' */
                e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                return;
            }
            if (e->pos < e->len && e->p[e->pos] == '#') {
                /* the name ends at a blank or flow indicator; a glued
                 * '#' is junk on the name (libyaml: "did not find
                 * expected alphabetic or numeric character") — the
                 * nightly's run 38121449804, "&f#o": this scan kept
                 * the anchor and DROPPED the bytes after '#',
                 * mangling the node while the runner kept a different
                 * shape */
                e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                return;
            }
        } else if (c == '!') {
            size_t start = e->pos;
            e->pos++;
            if (e->pos < e->len && e->p[e->pos] == '<') {
                /* a verbatim tag closes '>' on the same line, with no
                 * interior space and non-empty content — libyaml errors
                 * otherwise ("did not find expected '>'"; the nightly's
                 * fuzz_roundtrip byte-instability crasher, "!<") */
                size_t gt = e->pos + 1;
                while (gt < e->len && e->p[gt] != '>' && e->p[gt] != '\n' && e->p[gt] != '\r' &&
                       e->p[gt] != ' ' && e->p[gt] != '\t') {
                    gt++;
                }
                if (gt >= e->len || e->p[gt] != '>' || gt == e->pos + 1) {
                    e_fail(e, YEP_ERR_UNEXPECTED, start);
                    return;
                }
                e->pos = gt + 1;
            } else {
                /* libyaml scans URI chars only: bytes like '>' or '|'
                 * end the suffix and surface as the follower, which
                 * the check below rejects */
                while (e->pos < e->len && yep_scan_tag_char((unsigned char)e->p[e->pos])) {
                    e->pos++;
                }
            }
            if (e->pos < e->len) {
                unsigned char after = (unsigned char)e->p[e->pos];
                if (!(after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
                      after == ',' || after == '[' || after == ']' || after == '{' ||
                      after == '}')) {
                    e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                    return;
                }
            }
            yep_view raw = {e->p + start, (uint32_t)(e->pos - start)};
            *tag = e_resolve_tag(e, raw);
            if (e->tag_undef) {
                e->tag_undef = 0;
                e_fail(e, YEP_ERR_UNEXPECTED, start);
                return;
            }
        } else {
            return;
        }
    }
}

/* ------------------------------------------------------------- aliases */

static int e_alias(yep_engine* e, yep_event* ev) {
    size_t star = e->pos;
    size_t start = ++e->pos;
    while (e->pos < e->len && yep_scan_prop_char((unsigned char)e->p[e->pos])) {
        if (e->p[e->pos] == ':') {
            /* ':' separates only before blank or flow indicators; at
             * EOL it belongs to the name ("*a:" — 2SXE), and interior
             * colons always do ("*an:chor" — Y2GN) */
            size_t n2 = e->pos + 1;
            if (n2 < e->len && (e->p[n2] == ' ' || e->p[n2] == '\t' ||
                                yep_ct_is((unsigned char)e->p[n2], YEP_CT_FLOW_IND))) {
                break;
            }
        }
        e->pos++;
    }
    yep_view name = {e->p + start, (uint32_t)(e->pos - start)};
    if (name.len == 0) {
        return e_fail(e, YEP_ERR_UNEXPECTED, star);
    }
    uint32_t target = anchor_id_of(e, name);
    if (target == 0) {
        e->pos = star;
        return e_fail(e, YEP_ERR_UNDEFINED_ALIAS, star);
    }
    if (e->pos < e->len && e->p[e->pos] == '#') {
        /* the name ends at a blank or flow indicator; a glued '#' is
         * junk (the anchor-name follower law, catch 43's family) */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    e_skip_inline_space(e);
    if (e->pos < e->len && (e->p[e->pos] == '&' || e->p[e->pos] == '!' || e->p[e->pos] == '*')) {
        /* an alias node is COMPLETE — it carries no props and nothing
         * can follow it on the line but a separator: "<<: *base &bar"
         * (the nightly's run 38124486657 family) errored in libyaml
         * ("did not find expected key") while the DOM route absorbed
         * the stray props and the value drain rejected — the routes
         * split */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    ev->type = YEP_EV_ALIAS;
    ev->value = name;
    ev->borrowed = 1; /* the name is an input view (TODO.restructure/73):
                         the DOM borrows it like any plain scalar — no
                         arena copy per alias */
    ev->anchor_id = target;
    e_pos_mark(e, e->pos, &ev->end_line, &ev->end_col);
    return 0;
}

/* ---------------------------------------------------------------- flow */

static int e_flow_ws(yep_engine* e) {
    for (;;) {
        while (e->pos < e->len && (e->p[e->pos] == ' ' || e->p[e->pos] == '\t')) {
            e->pos++;
        }
        if (e->pos < e->len && e_comment_at(e)) {
            e_skip_to_eol(e);
            continue;
        }
        if (e->pos < e->len && (e->p[e->pos] == '\n' || e->p[e->pos] == '\r')) {
            e_line_done(e, e->pos);
            if (e->pos < e->len && e->pos == e->line_start) {
                yep_line_info fl = e_line_info_here(e);
                if ((fl.flags & YEP_LF_TAB) && !(fl.flags & YEP_LF_BLANK) && fl.indent == 0) {
                    /* a col-0 tab before flow content errors; tabs after
                     * spaces are separation (6HB6) */
                    size_t t = e->line_start + fl.indent;
                    while (t < fl.end && (e->p[t] == ' ' || e->p[t] == '\t')) {
                        t++;
                    }
                    if (t < fl.end && e->p[t] != '[' && e->p[t] != ']' && e->p[t] != '{' &&
                        e->p[t] != '}' && e->p[t] != ',' && e->p[t] != '#' && e->p[t] != ':') {
                        e_fail(e, YEP_ERR_TAB_IN_INDENT, t); /* Y79Y#4 */
                        return -1;
                    }
                }
            }
            continue;
        }
        return 0;
    }
}

/* Pre-scans a flow collection from its opening bracket, returning 0 with
 * e->pos just past the matching close (no events; for the flow-as-key
 * decision MAP_START must precede the key's events). */
static int e_skip_flow(yep_engine* e) {
    int stk[YEP_MAX_DEPTH];
    int n = 0;
    stk[n++] = (e->p[e->pos] == '[') ? 0 : 1;
    e->pos++;
    /* quotes open only at a TOKEN start: a quote byte inside a tag
     * suffix ("[!'str b]") or a plain ("[a'b]") is content — libyaml
     * accepts both; the pre-scan must not read them as a quoted
     * scalar (the nightly's run 38171171360) */
    int tok_start = 1;
    while (e->pos < e->len && n > 0) {
        unsigned char c = (unsigned char)e->p[e->pos];
        if (tok_start && (c == '!' || c == '&' || c == '*')) {
            /* props/aliases ride the span: skip them the way the real
             * scan does — a verbatim tag to its '>', names over the
             * tag-char run (no quote/bracket bytes in the run) */
            e->pos++;
            if (c == '!' && e->pos < e->len && e->p[e->pos] == '<') {
                while (e->pos < e->len && e->p[e->pos] != '>' && e->p[e->pos] != '\n' &&
                       e->p[e->pos] != '\r' && e->p[e->pos] != ' ' && e->p[e->pos] != '\t') {
                    e->pos++;
                }
                if (e->pos < e->len && e->p[e->pos] == '>') {
                    e->pos++;
                }
                tok_start = 0;
                continue;
            }
            while (e->pos < e->len && yep_scan_tag_char((unsigned char)e->p[e->pos])) {
                e->pos++;
            }
            tok_start = 0;
            continue;
        }
        if (tok_start && (c == '"' || c == '\'')) {
            int esc2 = 0;
            const yep_text_kernels* k = yep_text_active();
            ptrdiff_t r = k->quote_scan(e->p + e->pos + 1, e->len - e->pos - 1, (char)c, &esc2);
            if (r < 0) {
                return e_fail(e, YEP_ERR_UNTERMINATED_QUOTE, e->pos);
            }
            e->pos += 1 + (size_t)r + 1;
            tok_start = 0;
            continue;
        }
        if (c == '#' && e_comment_at(e)) {
            e_skip_to_eol(e);
            continue;
        }
        if (c == '\n' || c == '\r') {
            e_line_done(e, e->pos);
            tok_start = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '[' || c == '{' || c == ',') {
            tok_start = 1;
        } else if (c == ':' && e->pos + 1 < e->len &&
                   (e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                    e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r' ||
                    yep_ct_is((unsigned char)e->p[e->pos + 1], YEP_CT_FLOW_IND))) {
            tok_start = 1; /* a separating ':' only */
        } else {
            tok_start = 0;
        }
        if (c == '[') {
            if (n >= e->max_depth) {
                return e_fail(e, YEP_ERR_DEPTH, e->pos);
            }
            stk[n++] = 0;
        } else if (c == '{') {
            if (n >= e->max_depth) {
                return e_fail(e, YEP_ERR_DEPTH, e->pos);
            }
            stk[n++] = 1;
        } else if (c == ']') {
            if (stk[n - 1] != 0) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
            }
            n--;
        } else if (c == '}') {
            if (stk[n - 1] != 1) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
            }
            n--;
        }
        e->pos++;
    }
    if (n != 0) {
        if (!e->parsing_final) {
            return 0; /* incomplete: more bytes may close it; the real
                       * parse (e_flow) owns the deferral */
        }
        return e_fail(e, YEP_ERR_UNEXPECTED, e->len);
    }
    return 0;
}

/* Parses one flow node at the cursor into *ev. Return: 1 node parsed,
 * 0 no node (separator/close next), 2 nested opener, -1 error. */
static int e_flow_node(yep_engine* e, yep_event* ev, int keyish) {
    (void)keyish;
    if (e_flow_ws(e) != 0) {
        return -1;
    }
    if (e->pos >= e->len) {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    unsigned char c = (unsigned char)e->p[e->pos];
    if (c == '[' || c == '{') {
        return 2;
    }
    if (c == ']' || c == '}' || c == ',') {
        return 0; /* separator / close */
    }
    if (c == ':') {
        /* ':' separates when followed by blank/EOL/flow indicator, or
         * directly after a flow close ("{a: b}:c" — JSON-style); ":x" is
         * a plain scalar starting with a colon */
        size_t n2 = e->pos + 1;
        if (n2 >= e->len || e->p[n2] == ' ' || e->p[n2] == '\n' || e->p[n2] == '\r' ||
            yep_ct_is((unsigned char)e->p[n2], YEP_CT_FLOW_IND) ||
            (e->pos > 0 && (e->p[e->pos - 1] == ']' || e->p[e->pos - 1] == '}'))) {
            return 0;
        }
    }

    e_event_init(ev, YEP_EV_SCALAR);
    ev->line = e->line;
    ev->col = e_col(e, e->pos) + 1;
    yep_view anchor = {0}, tag = {0};
    e_props(e, &anchor, &tag);
    if (e->err.code != YEP_ERR_NONE) {
        return -1; /* e_props failed (empty anchor name, unresolved tag) */
    }
    if (!yep_view_is_empty(anchor) &&
        (memchr(anchor.p, '"', anchor.len) != NULL || memchr(anchor.p, '\'', anchor.len) != NULL)) {
        /* anchors have no quoting: a quote byte in a FLOW anchor name
         * has no safe spelling (Psych: "did not find expected
         * alphabetic or numeric character"). Block names ride quotes
         * (the suite's W5VH) — this law is flow-scoped. */
        return (int)e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    ev->anchor = anchor;
    ev->tag = tag;
    if (!yep_view_is_empty(anchor)) {
        ev->anchor_id = anchor_define(e, anchor);
    }
    if (e_flow_ws(e) != 0) {
        return -1;
    }
    if (e->pos >= e->len) {
        /* props at EOF: no node follows the properties (the entry
         * guard above cannot cover this — e_props consumed to the
         * end; found by fuzz_parse, input "[!<<\r\n!]" x8) */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    c = (unsigned char)e->p[e->pos];
    if ((c == ',' || c == ']' || c == '}' || c == ':') &&
        (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag))) {
        ev->implicit = 1; /* properties with no node: a tagged null */
        ev->style = YEP_STYLE_PLAIN;
        e_pos_mark(e, e->pos, &ev->end_line, &ev->end_col);
        return 1;
    }
    if (c == '"' || c == '\'') {
        ev->implicit = 0;
        return e_quoted_floor(e, ev, 0, 0) == 0 ? 1 : -1;
    }
    if (c == '*') {
        if (e_alias(e, ev) != 0) {
            return -1;
        }
        if (ev->value.len > 0 && (memchr(ev->value.p, '"', ev->value.len) != NULL ||
                                  memchr(ev->value.p, '\'', ev->value.len) != NULL)) {
            /* the alias twin of the flow anchor-name law above */
            return (int)e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        return 1;
    }
    if (c == '[' || c == '{') {
        return 2;
    }
    /* plain: may span lines (folded); a leading non-structural ':' is
     * part of the scalar ("::x" per the suite) — keep it in the first
     * piece by scanning from the colon itself */
    const char* piece_start_fix = e->p + e->pos;
    if ((unsigned char)*piece_start_fix == ':') {
        e->pos++;
        if (e_flow_ws(e) != 0) {
            return -1;
        }
        if (e->pos >= e->len) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        /* rescan from the colon so the piece includes it */
        e->pos = (size_t)(piece_start_fix - e->p);
    }
    e->fold_n = 0;
    int crossed_break = 0;
    for (;;) {
        yep_span s = yep_scan_plain(e->p, e->len, e->pos, 1);
        if (s.end == s.start && e->fold_n == 0) {
            return 0; /* empty token: separator/close comes next */
        }
        if (e->fold_n >= YEP_MAX_FOLD_LINES) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        if (s.end > s.start) {
            e->fold[e->fold_n].content.p = e->p + s.start;
            e->fold[e->fold_n].content.len = s.end - s.start;
            e->fold[e->fold_n].breaks_before = (e->fold_n == 0) ? 0 : 1;
            e->fold_n++;
        }
        if (s.term == YEP_TERM_EOL || s.term == YEP_TERM_COMMENT) {
            e->pos = s.end;
            if (s.term == YEP_TERM_COMMENT) {
                e_skip_to_eol(e);
                break; /* a comment ends the scalar: the next token is new */
            }
            e_line_done(e, e->pos);
            crossed_break = 1;
            {
                yep_line_info cl = e_line_info_here(e);
                if (cl.flags & YEP_LF_COMMENT) {
                    e_line_done(e, cl.end);
                    break; /* a comment line ends the scalar too */
                }
            }
            if (e_flow_ws(e) != 0) {
                return -1;
            }
            if (e->pos < e->len && e->p[e->pos] != ']' && e->p[e->pos] != '}' &&
                e->p[e->pos] != ',' && !e_comment_at(e) && e->p[e->pos] != '\n' &&
                e->p[e->pos] != '\r') {
                continue; /* continuation on a following line */
            }
            break; /* at a boundary; pos is on the separator/close */
        }
        e->pos = s.end;
        break;
    }
    if (e->fold_n == 1) {
        ev->value.p = e->fold[0].content.p;
        ev->value.len = e->fold[0].content.len;
        ev->borrowed = 1;
    } else {
        char* out = yep_fold_plain(e->fold, e->fold_n, e->pool, &ev->value.len);
        if (out == NULL) {
            return e_fail(e, YEP_ERR_MEMORY, e->pos);
        }
        ev->value.p = out;
        ev->borrowed = 0;
    }
    if (e->fold_n == 1 && e->fold[0].content.len == 1 && e->fold[0].content.p[0] == '-') {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "[-, -]": bare dash entry */
    }
    ev->style = YEP_STYLE_PLAIN;
    ev->implicit = 1;
    ev->multiline = (e->fold_n > 1) || crossed_break;
    { /* #179: the source end = the last fold piece's content end */
        const yep_view* lastp = &e->fold[e->fold_n - 1].content;
        e_pos_mark(e, (size_t)(lastp->p - e->p) + lastp->len, &ev->end_line, &ev->end_col);
    }
    return 1;
}

/* --------------------------------------------------- JSON fast path */

/* The JSON-class fast path (TODO.impl/08A): one validating scan of the
 * span, then bulk event emission — pure-JSON flow collections skip the
 * general kernel's per-node guards. STRICTLY conservative: anything not
 * byte-for-byte JSON (tabs, comments, YAML scalars, trailing commas,
 * indent anomalies under a block parent) returns 0 and the general
 * kernel handles it, so semantics can never diverge — only speed. */

#define YEP_JSON_MAX_DEPTH 256

/* Line/col across a gap: the walk is scan's SSOT (yep_scan_advance_
 * line) — the DOM direct builder shares it so line/col cannot drift
 * between the two paths. */
static void jx_advance_line(yep_engine* e, size_t* from, size_t to, uint32_t* line,
                            size_t* line_start) {
    yep_scan_advance_line(e->p, from, to, line, line_start);
}

/* A flow node followed by ':' on the same line is a KEY: the caller
 * must wrap it in a mapping first — refuse so the general path's
 * is_key logic runs. Shared by both fast paths (grammar fact). */
static int e_flow_key_after(const char* p, size_t len, size_t close) {
    size_t j = close + 1;
    while (j < len && p[j] == ' ') {
        j++;
    }
    return j < len && p[j] == ':' &&
           (j + 1 >= len || p[j + 1] == ' ' || p[j + 1] == '\n' || p[j + 1] == '\r' ||
            p[j + 1] == '\t');
}

/* Block-level flow lines must out-indent the parent (9C9N): any line
 * start inside the span at or left of the floor falls back — the
 * closer's own line included ("a: {\n  x: 1\n}\nb: 3" is a suite
 * error; only libyaml accepts it). The scan must cover the closer
 * line: its start sits AT the close offset, outside the loop below,
 * so it is checked explicitly — the general kernel's per-line check
 * rejects it, and the fast path must agree (the nightly's run
 * 38280372447: whole vs chunked split across the fast/slow paths and
 * produced two different verdicts for the same bytes). */
static int e_flow_floor_ok(const char* p, size_t open, size_t close, uint16_t floor) {
    size_t j = open;
    while (j < close) {
        if (p[j] == '\n') {
            size_t k = j + 1;
            uint32_t ind = 0;
            while (k < close && p[k] == ' ') {
                ind++;
                k++;
            }
            if (k < close && p[k] != '\n' && p[k] != '\r' && ind <= floor) {
                return 0;
            }
        }
        j++;
    }
    if (close < (size_t)-1 && close > open) {
        size_t ls = close;
        uint32_t ind = 0;
        while (ls > 0 && p[ls - 1] != '\n') {
            ls--;
        }
        while (ls < close && p[ls] == ' ') {
            ind++;
            ls++;
        }
        if (ind <= floor) {
            return 0;
        }
    }
    return 1;
}

/* Returns 1 = events emitted and consumed (e->pos past the close),
 * 0 = not JSON-class (fall back), -1 = error set, -2 = sink abort. */
static int e_flow_json(yep_engine* e, yep_view anchor, yep_view tag, uint32_t anchor_id) {
    const char* p = e->p;
    size_t len = e->len;
    size_t open_pos = e->pos;
    if (len - open_pos < 24) {
        return 0; /* tiny spans: the general kernel is cheaper */
    }

    /* ---- fused path (TODO.restructure/53): the sink validates AND
     * builds in one walk; the engine's grammar checks run on the
     * builder's close, then commit or roll back. ---- */
    if (e->ev_scopes == 0 && e->sink != NULL && e->sink->on_flow_build != NULL) {
        size_t bclose = 0;
        int rc = e->sink->on_flow_build(e->sink->ctx, p, open_pos, len, e->line, e->line_start,
                                        anchor, tag, anchor_id, e->max_depth, &bclose);
        if (rc < 0) {
            return -2;
        }
        if (rc == 0) {
            return 0; /* not JSON-class: the general kernel */
        }
        if (rc == 1) {
            int single = (memchr(p + open_pos, '\n', bclose - open_pos) == NULL);
            if (e_flow_key_after(p, len, bclose) ||
                (e->flow_enforce && !single &&
                 !e_flow_floor_ok(p, open_pos, bclose, e->flow_floor))) {
                e->sink->on_flow_rollback(e->sink->ctx);
                return 0;
            }
            if (e->sink->on_flow_commit(e->sink->ctx) != 1) {
                return -2;
            }
            size_t from = open_pos;
            uint32_t ln = e->line;
            size_t ls = e->line_start;
            jx_advance_line(e, &from, bclose + 1, &ln, &ls);
            e->line = ln;
            e->line_start = ls;
            e->pos = bclose + 1;
            e_skip_inline_space(e);
            return 1;
        }
        /* rc == 2: long key — fall to pass 1 + pass 2 for the error */
    }

    /* ---- pass 1: strict-JSON validation via the shared walker ----
     * (the grammar lives in scan/json.c; the engine drives it) */
    uint8_t kind0;
    size_t close;
    {
        yep_json_walk w;
        yep_json_tok t;
        yep_json_walk_init(&w, p, len, open_pos, e->max_depth);
        w.strict = 1; /* the YAML fast path is byte-for-byte JSON only:
                       * a lenient walk accepted number/literal keys
                       * ("{9:1}" -> {9->1}) that the general kernel
                       * reads as the plain scalar "9:1" — whole and
                       * chunked parses took different paths and
                       * diverged (the nightly's run 38513849602) */
        for (;;) {
            yep_jw_status st = yep_json_walk_next(&w, &t);
            if (st == YEP_JW_REJECT) {
                return 0;
            }
            if (st == YEP_JW_DONE) {
                break;
            }
        }
        close = w.close;
        kind0 = w.kind[0];
    }

    /* One memchr decides the dominant shape: a SINGLE-LINE span (one
     * flow collection per block line — the common config/API dump)
     * skips the flow_enforce rescan AND the per-event line bookkeeping
     * below; both are no-ops without a newline, and pass 1 already
     * walked these bytes (TODO.restructure/45, Phase B slice). */
    int single_line = (memchr(p + open_pos, '\n', close - open_pos) == NULL);

    if (e_flow_key_after(p, len, close)) {
        return 0;
    }
    if (e->flow_enforce && !single_line && !e_flow_floor_ok(p, open_pos, close, e->flow_floor)) {
        return 0;
    }

    /* ---- pass 2: emit events from the validated span ---- */
    uint32_t cur_line = e->line;
    size_t cur_ls = e->line_start;
    size_t cur_scan = e->pos; /* unscanned region starts at the span */
    {
        yep_event ev;
        e_event_init(&ev, kind0 ? YEP_EV_MAP_START : YEP_EV_SEQ_START);
        ev.flow = 1;
        ev.anchor = anchor;
        ev.anchor_id = anchor_id;
        ev.tag = tag;
        ev.line = cur_line;
        /* the general kernel's opener marks, mirrored exactly: col =
         * the indicator's 1-based column, end one past it. The old +1
         * made every fused open sit one column right of the same
         * span's general-kernel parse — a <24-byte span cut at a
         * chunk boundary routes general while the whole parse routes
         * fused (the nightly's run 38173499680) */
        ev.col = (uint32_t)(open_pos - cur_ls) + 1;
        ev.end_line = cur_line; /* flow opens end after the indicator */
        ev.end_col = (uint32_t)(open_pos - cur_ls) + 2;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
    }
    uint8_t stk[YEP_JSON_MAX_DEPTH]; /* 0 seq, 1 map; key-pending per frame */
    uint8_t kpending[YEP_JSON_MAX_DEPTH];
    int sd = 1;
    stk[0] = kind0;
    kpending[0] = 0;
    size_t i = open_pos + 1;
    for (;;) {
        while (i < close && (p[i] == ' ' || p[i] == '\n' || p[i] == '\r')) {
            i++;
        }
        char c = p[i];
        if (c == ']' || c == '}') {
            yep_event ev;
            e_event_init(&ev, stk[sd - 1] ? YEP_EV_MAP_END : YEP_EV_SEQ_END);
            /* flow=1 only on START events (kernel convention: the style
             * belongs to the opening bracket, END events stay plain) */
            if (!single_line)
                jx_advance_line(e, &cur_scan, i, &cur_line, &cur_ls);
            ev.line = cur_line;
            ev.col = (uint32_t)(i - cur_ls) + 1;
            ev.end_line = cur_line;
            ev.end_col = (uint32_t)(i + 1 - cur_ls) + 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            sd--;
            if (sd == 0) {
                break;
            }
            i++;
            continue;
        }
        if (c == '{' || c == '[') {
            if (stk[sd - 1] && kpending[sd - 1] == 2) {
                kpending[sd - 1] = 0; /* the value of the pending pair */
            }
            yep_event ev;
            e_event_init(&ev, c == '[' ? YEP_EV_SEQ_START : YEP_EV_MAP_START);
            ev.flow = 1;
            if (!single_line)
                jx_advance_line(e, &cur_scan, i, &cur_line, &cur_ls);
            ev.line = cur_line;
            ev.col = (uint32_t)(i + 1 - cur_ls) + 1;
            ev.end_line = cur_line; /* flow opens end after the indicator */
            ev.end_col = (uint32_t)(i + 2 - cur_ls) + 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            stk[sd] = (c == '[') ? 0 : 1;
            kpending[sd] = 0;
            sd++;
            i++;
            continue;
        }
        if (c == ',' || c == ':') {
            i++;
            continue;
        }
        /* scalar: quoted, number, or literal */
        yep_event ev;
        e_event_init(&ev, YEP_EV_SCALAR);
        if (!single_line)
            jx_advance_line(e, &cur_scan, i, &cur_line, &cur_ls);
        ev.line = cur_line;
        ev.col = (uint32_t)(i - cur_ls) + 1;
        size_t vstart = i;
        size_t raw_end = i; /* span end (past the closing quote for strings) */
        if (c == '"') {
            int he = 0;
            size_t vclose;
            if (!yep_json_string(p, close + 1, &i, &vclose, &he)) {
                return -1; /* validated: cannot happen */
            }
            size_t vend = vclose; /* the closing quote */
            raw_end = vclose + 1;
            ev.style = YEP_STYLE_DOUBLE_QUOTED;
            if (he) {
                ev.multiline = 0;
                char* out = yep_finish_double(p, (uint32_t)(vstart + 1), (uint32_t)vend, 0, e->pool,
                                              &ev.value.len);
                if (out == NULL) {
                    return e_fail(e, YEP_ERR_MEMORY, vstart);
                }
                ev.value.p = out;
                ev.borrowed = 0;
            } else {
                ev.value.p = p + vstart + 1;
                ev.value.len = (uint32_t)(vend - vstart - 1);
                ev.borrowed = 1;
            }
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            i++;
            while (i < close && ((p[i] >= '0' && p[i] <= '9') || p[i] == '.' || p[i] == 'e' ||
                                 p[i] == 'E' || p[i] == '+' || p[i] == '-')) {
                i++;
            }
            ev.value.p = p + vstart;
            ev.value.len = (uint32_t)(i - vstart);
            ev.borrowed = 1;
            ev.style = YEP_STYLE_PLAIN;
            ev.implicit = 1;
        } else {
            /* literals: validated in pass 1; walk their exact length */
            i += (c == 't') ? 4 : (c == 'f') ? 5 : 4;
            ev.value.p = p + vstart;
            ev.value.len = (uint32_t)(i - vstart);
            ev.borrowed = 1;
            ev.style = YEP_STYLE_PLAIN;
            ev.implicit = 1;
        }
        /* a map's first scalar of a pair is a KEY: the 1024-character
         * simple-key limit applies (raw span, quotes included) */
        if (stk[sd - 1] && kpending[sd - 1] == 0) {
            size_t kspan = raw_end - vstart;
            if (kspan > YEP_MAX_SIMPLE_KEY) {
                return e_fail(e, YEP_ERR_KEY_TOO_LONG, vstart);
            }
        }
        if (raw_end == vstart) {
            raw_end = i; /* numbers/literals advance i, not raw_end */
        }
        ev.end_line = cur_line;
        ev.end_col = (uint32_t)(raw_end - cur_ls) + 1;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
        if (stk[sd - 1] && p[i] == ':' && kpending[sd - 1] == 0) {
            kpending[sd - 1] = 2; /* key seen; the next scalar/collection is the value */
        }
    }

    /* engine position: past the close; line state reflects the span */
    jx_advance_line(e, &cur_scan, close + 1, &cur_line, &cur_ls);
    e->line = cur_line;
    e->line_start = cur_ls;
    e->pos = close + 1;
    e_skip_inline_space(e);
    return 1;
}

/* Iterative flow kernel; emits events for the whole collection. */
/* Initializes one flow frame (SSOT: every field, every push site). */
static void e_flow_frame_init(void* stv, int idx, int kind, int pending_key) {
    typedef struct {
        uint8_t kind;
        uint8_t pending_key;
        uint8_t sep;
        uint8_t just_closed;
        uint8_t pair_open;
        uint8_t pair_value;
        uint8_t pair_wrap;
        int32_t buf_from;
        uint32_t entry_line;
        uint32_t entries;
    } e_flow_frame;
    e_flow_frame* st = (e_flow_frame*)stv;
    st[idx].kind = (uint8_t)kind;
    st[idx].pending_key = (uint8_t)pending_key;
    st[idx].sep = 0;
    st[idx].just_closed = 0;
    st[idx].pair_open = 0;
    st[idx].pair_value = 0;
    st[idx].pair_wrap = 0;
    st[idx].buf_from = -1;
    st[idx].entry_line = 0;
    st[idx].entries = 0;
}

static int e_flow(yep_engine* e, yep_view anchor, yep_view tag, uint32_t anchor_id) {
    struct {
        uint8_t kind;        /* 0 seq, 1 map */
        uint8_t pending_key; /* map: 1 key seen awaiting ':' value; 2 key emitted w/o ':' */
        uint8_t sep;         /* a ',' was consumed and no entry followed yet */
        uint8_t just_closed; /* a nested collection closed here (key candidate) */
        uint8_t pair_open;   /* seq: a buffered single-pair awaits its MAP_END */
        uint8_t pair_value;  /* seq: the open pair already has its value */
        uint8_t pair_wrap;   /* map frame pushed as a seq single-pair */
        int32_t buf_from;    /* seq: entry events buffered from this index */
        uint32_t entry_line; /* seq: line where the buffered entry ended */
        uint32_t entries;    /* completed entries (for trailing-comma detection) */
    } st[YEP_MAX_DEPTH];
    int n = 0;

    unsigned char open = (unsigned char)e->p[e->pos];
    e->pos++;
    yep_event ev;
    e_event_init(&ev, open == '[' ? YEP_EV_SEQ_START : YEP_EV_MAP_START);
    ev.flow = 1;
    ev.anchor = anchor;
    ev.anchor_id = anchor_id;
    ev.tag = tag;
    ev.line = e->line;
    ev.col = e_col(e, e->pos);
    ev.end_line = e->line; /* flow opens end after the indicator */
    ev.end_col = e_col(e, e->pos) + 1;
    if (emit_now(e, &ev) != 0) {
        return -2;
    }
    e_flow_frame_init(st, n, (open == '[') ? 0 : 1, 0);
    n++;

    for (;;) {
        e_event_init(&ev, YEP_EV_NONE);
        if (e->pos == e->line_start && e->pos < e->len) {
            yep_line_info fl = e_line_info_here(e);
            if ((fl.flags & YEP_LF_TAB) && !(fl.flags & YEP_LF_BLANK)) {
                return e_fail(e, YEP_ERR_TAB_IN_INDENT, e->pos); /* Y79Y#4 */
            }
        }
        if (st[n - 1].kind == 0 && st[n - 1].buf_from < 0 && st[n - 1].pair_open == 0 &&
            e->pos < e->len && e->p[e->pos] != ',' && e->p[e->pos] != ']' && e->p[e->pos] != '}' &&
            e->p[e->pos] != ':') {
            /* a sequence entry may become a single-pair key: defer it */
            st[n - 1].buf_from = (int32_t)e->ev_buf_n;
            st[n - 1].entry_line = e->line;
            e->ev_scopes++;
        }
        int keyish = (st[n - 1].kind == 1 && st[n - 1].pending_key == 0);
        int q_key = 0;
        int rc;
        if (e_flow_ws(e) != 0) {
            return -1;
        }
        if (e->pos < e->len && e->pos == e->line_start) {
            yep_line_info fl = e_line_info_here(e);
            if (fl.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END | YEP_LF_DIRECTIVE)) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* marker inside flow */
            }
            if (fl.flags & YEP_LF_TAB) {
                return e_fail(e, YEP_ERR_TAB_IN_INDENT, e->pos); /* Y79Y#4 */
            }
            if (e->flow_enforce && !(fl.flags & (YEP_LF_BLANK | YEP_LF_COMMENT)) &&
                fl.indent <= e->flow_floor) {
                /* a block-level flow's lines out-indent its parent (9C9N) */
                return e_fail(e, YEP_ERR_BAD_INDENT, e->pos + fl.indent);
            }
        }
        if (e->pos < e->len && e->p[e->pos] != ',' && e->p[e->pos] != ']' && e->p[e->pos] != '}' &&
            !(e->p[e->pos] == ':' &&
              ((st[n - 1].kind == 1 && st[n - 1].pending_key == 1) || st[n - 1].just_closed)) &&
            st[n - 1].entries > 0 && st[n - 1].sep == 0 && !st[n - 1].pair_open &&
            !(st[n - 1].kind == 1 && st[n - 1].pending_key == 1)) {
            if (getenv("YEP_TRACE"))
                fprintf(stderr, "[mc] pos=%c kind=%d ent=%u sep=%u pk=%u jc=%u\n", e->p[e->pos],
                        st[n - 1].kind, st[n - 1].entries, st[n - 1].sep, st[n - 1].pending_key,
                        st[n - 1].just_closed);
            /* a node without a preceding ',' or ':' (CML9 missing comma);
             * in a sequence a ':'-led token is a new plain, so it errors */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (e->pos < e->len && e->p[e->pos] == '?' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r' ||
             yep_ct_is((unsigned char)e->p[e->pos + 1], YEP_CT_FLOW_IND))) {
            e->pos++;
            if (e_flow_ws(e) != 0) {
                return -1;
            }
            if (e->pos >= e->len ||
                (e->p[e->pos] == ':' &&
                 (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                  e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r' ||
                  yep_ct_is((unsigned char)e->p[e->pos + 1], YEP_CT_FLOW_IND) ||
                  e->p[e->pos + 1] == ',' || e->p[e->pos + 1] == ']' || e->p[e->pos + 1] == '}')) ||
                e->p[e->pos] == ',' || e->p[e->pos] == ']' || e->p[e->pos] == '}') {
                /* '? : v' / '?' at the boundary: an EMPTY explicit key */
                e_event_init(&ev, YEP_EV_SCALAR);
                ev.implicit = 1;
                ev.style = YEP_STYLE_PLAIN;
                ev.line = e->line;
                ev.col = e_col(e, e->pos) + 1;
                rc = 1;
                q_key = 1;
            } else {
                rc = e_flow_node(e, &ev, keyish);
                q_key = 1;
            }
        } else {
            rc = e_flow_node(e, &ev, keyish);
        }
        if (rc == -1) {
            return -1;
        }
        if (rc == 2) {
            if (n >= e->max_depth) {
                return e_fail(e, YEP_ERR_DEPTH, e->pos);
            }
            st[n - 1].pair_value = 1;
            unsigned char oc = (unsigned char)e->p[e->pos];
            e->pos++;
            yep_view pa = ev.anchor, pt = ev.tag; /* props before the opener */
            uint32_t paid = ev.anchor_id;
            e_event_init(&ev, oc == '[' ? YEP_EV_SEQ_START : YEP_EV_MAP_START);
            ev.flow = 1;
            ev.anchor = pa;
            ev.anchor_id = paid;
            ev.tag = pt;
            ev.line = e->line;
            ev.col = e_col(e, e->pos);
            ev.end_line = e->line; /* flow opens end after the indicator */
            ev.end_col = e_col(e, e->pos) + 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            st[n - 1].just_closed = 0;
            /* the nested collection is an entry of the parent frame; when
             * it appears in a map's value position it completes the pair */
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 1) {
                st[n - 1].pending_key = 0;
            }
            st[n - 1].sep = 0;
            st[n - 1].entries++;
            e_flow_frame_init(st, n, (oc == '[') ? 0 : 1, 0);
            n++;
            continue;
        }
        if (rc == 1) {
            st[n - 1].sep = 0;
            st[n - 1].just_closed = 0;
            st[n - 1].entries++;
            if (st[n - 1].buf_from >= 0) {
                st[n - 1].entry_line = e->line; /* node end (pre-colon ws) */
            }
            if (st[n - 1].pair_open) {
                st[n - 1].pair_value = 1; /* the pair's value arrived */
            }
            /* key detection: ':' after the node (spaces and line breaks
             * may precede it: "unquoted : value", "\"foo\"\n: bar").
             * After a QUOTED node the colon separates JSON-style. */
            size_t save_pos = e->pos;
            uint32_t save_line = e->line;
            size_t save_ls = e->line_start;
            if (e_flow_ws(e) != 0) {
                return -1;
            } /* crosses newlines; restored below when no colon */
            int quoted = (ev.type == YEP_EV_SCALAR && (ev.style == YEP_STYLE_SINGLE_QUOTED ||
                                                       ev.style == YEP_STYLE_DOUBLE_QUOTED));
            int key_pos = (st[n - 1].kind == 1 && st[n - 1].pending_key == 0);
            int cross_line = 0;
            for (size_t k = save_pos; k < e->pos; k++) {
                if (e->p[k] == '\n' || e->p[k] == '\r') {
                    cross_line = 1;
                    break;
                }
            }
            int colon = (e->pos < e->len && e->p[e->pos] == ':' &&
                         ((quoted && !ev.multiline && (!cross_line || st[n - 1].kind == 1)) ||
                          (key_pos && !ev.multiline && !cross_line) || e->pos + 1 >= e->len ||
                          e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                          e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r' ||
                          yep_ct_is((unsigned char)e->p[e->pos + 1], YEP_CT_FLOW_IND)));
            if (!colon) {
                /* not a key; the caller re-reads — the line state goes
                 * back with the cursor (the ws lookahead crossed the
                 * newline; a stale e->line marked everything after the
                 * flow node one line low) */
                e->pos = save_pos;
                e->line = save_line;
                e->line_start = save_ls;
            }
            if (colon && st[n - 1].kind == 1 && st[n - 1].pending_key == 0) {
                if (!q_key && quoted && ev.value.len + 2 > YEP_MAX_SIMPLE_KEY) {
                    return e_fail(e, YEP_ERR_KEY_TOO_LONG, e->pos);
                }
                if (!q_key && !quoted && ev.value.len > YEP_MAX_SIMPLE_KEY) {
                    return e_fail(e, YEP_ERR_KEY_TOO_LONG, e->pos);
                }
                e->pos++;
                st[n - 1].pending_key = 1;
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                /* value next; if none before , or } → null */
                continue;
            }
            if (colon && st[n - 1].kind == 0) {
                if (st[n - 1].pair_open && st[n - 1].pair_value) {
                    /* "[s: 2:]" — the open pair already has its value;
                     * a second single-pair would nest inside the value
                     * slot and unbalance the event stream (the nightly's
                     * run 38168927841). libyaml rejects the ':' mid-
                     * scalar; so does the suite's flow grammar. */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                if (!q_key && quoted && ev.value.len + 2 > YEP_MAX_SIMPLE_KEY) {
                    return e_fail(e, YEP_ERR_KEY_TOO_LONG, e->pos);
                }
                if (!q_key && !quoted && ev.value.len > YEP_MAX_SIMPLE_KEY) {
                    return e_fail(e, YEP_ERR_KEY_TOO_LONG, e->pos);
                }
                if (st[n - 1].buf_from >= 0 && !quoted && !q_key &&
                    (save_pos < e->line_start || ev.multiline)) {
                    /* "[a\n: b]": an IMPLICIT key cannot span lines ("? " may) */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                /* single-pair mapping inside a sequence: the entry's
                 * events are buffered; wrap them in MAP_START .. MAP_END */
                e->pos++;
                {
                    int32_t bf = st[n - 1].buf_from;
                    if (bf >= 0) {
                        st[n - 1].buf_from = -1;
                        e->ev_scopes--;
                        if (e_buf_wrap(e, (uint32_t)bf) != 0) {
                            return -2;
                        }
                        if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                            int brc = e_buf_send(e, 0);
                            if (brc != 0) {
                                return brc;
                            }
                        }
                        if (emit_now(e, &ev) != 0) {
                            return -2;
                        }
                        st[n - 1].pair_open = 1;
                        continue;
                    } else {
                        yep_event mk;
                        e_event_init(&mk, YEP_EV_MAP_START);
                        mk.flow = 1;
                        mk.line = ev.line;
                        mk.col = ev.col;
                        mk.end_line = ev.line; /* virtual opener: zero-span
                                                * at the implied position */
                        mk.end_col = ev.col;
                        if (emit_now(e, &mk) != 0) {
                            return -2;
                        }
                        if (emit_now(e, &ev) != 0) {
                            return -2;
                        }
                    }
                    st[n - 1].pair_open = 1;
                }
                continue; /* the value parses as the next regular node */
            }
            if (q_key && st[n - 1].kind == 0) {
                /* "[? x]": an explicit pair with no ':' — null value */
                int32_t bf = st[n - 1].buf_from;
                if (bf >= 0) {
                    st[n - 1].buf_from = -1;
                    e->ev_scopes--;
                    if (e_buf_wrap(e, (uint32_t)bf) != 0) {
                        return -2;
                    }
                    st[n - 1].pair_open = 1;
                    if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                        int brc = e_buf_send(e, 0);
                        if (brc != 0) {
                            return brc;
                        }
                    }
                    yep_event nv;
                    e_event_init(&nv, YEP_EV_SCALAR);
                    nv.implicit = 1;
                    nv.style = YEP_STYLE_PLAIN;
                    if (emit_now(e, &nv) != 0) {
                        return -2;
                    }
                    continue;
                }
            }
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 0) {
                /* bare entry in a flow map: key with null value */
                st[n - 1].pending_key = 2;
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                continue;
            }
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 1) {
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                st[n - 1].pending_key = 0;
                continue;
            }
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            continue;
        }

        /* rc == 0: separator or close */
        if (e_flow_ws(e) != 0) {
            return -1;
        }
        if (e->pos >= e->len) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        unsigned char c = (unsigned char)e->p[e->pos];
        if (c == ',') {
            e->pos++;
            if (st[n - 1].pair_open) {
                if (!st[n - 1].pair_value) {
                    yep_event nv;
                    e_event_init(&nv, YEP_EV_SCALAR);
                    nv.implicit = 1;
                    nv.style = YEP_STYLE_PLAIN;
                    if (emit_now(e, &nv) != 0) {
                        return -2;
                    }
                }
                yep_event mk;
                e_event_init(&mk, YEP_EV_MAP_END);
                if (emit_now(e, &mk) != 0) {
                    return -2;
                }
                st[n - 1].pair_open = 0;
                st[n - 1].pair_value = 0;
            }
            if (st[n - 1].buf_from >= 0) {
                st[n - 1].buf_from = -1;
                e->ev_scopes--;
                if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
            }
            if (st[n - 1].sep) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "{,x}" / "[1,,2]" */
            }
            if (st[n - 1].entries == 0 && !st[n - 1].pair_open) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "[, a]" */
            }
            if (st[n - 1].kind == 1 && st[n - 1].pending_key != 0) {
                /* key without ':' value, or ':' without value: null value */
                yep_event nv;
                e_event_init(&nv, YEP_EV_SCALAR);
                nv.implicit = 1;
                nv.style = YEP_STYLE_PLAIN;
                if (emit_now(e, &nv) != 0) {
                    return -2;
                }
                st[n - 1].pending_key = 0;
            }
            st[n - 1].sep = 1;
            st[n - 1].just_closed = 0;
            continue;
        }
        if (c == ']' || c == '}') {
            while (st[n - 1].pair_wrap) {
                /* a single-pair wrapper inside a sequence closes on any
                 * bracket ("[ : v ]", CFD4) */
                if (st[n - 1].pending_key != 0) {
                    yep_event nv;
                    e_event_init(&nv, YEP_EV_SCALAR);
                    nv.implicit = 1;
                    nv.style = YEP_STYLE_PLAIN;
                    if (emit_now(e, &nv) != 0) {
                        return -2;
                    }
                }
                yep_event mk;
                e_event_init(&mk, YEP_EV_MAP_END);
                if (emit_now(e, &mk) != 0) {
                    return -2;
                }
                st[n - 1].pair_wrap = 0;
                n--;
                if (n == 0) {
                    break; /* wrapper was the last frame: malformed */
                }
                st[n - 1].just_closed = 1;
                st[n - 1].entry_line = e->line;
            }
            unsigned char want = st[n - 1].kind ? '}' : ']';
            if (c != want) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
            }
            /* YAML 1.2 allows a trailing separator before the closer
             * ("[1, ]", "{a: 1, }"); the comma handler already rejected
             * doubled commas. */
            if (st[n - 1].kind == 1 && st[n - 1].pending_key != 0) {
                yep_event nv;
                e_event_init(&nv, YEP_EV_SCALAR);
                nv.implicit = 1;
                nv.style = YEP_STYLE_PLAIN;
                if (emit_now(e, &nv) != 0) {
                    return -2;
                }
                st[n - 1].pending_key = 0;
            }
            if (st[n - 1].pair_open) {
                if (!st[n - 1].pair_value) {
                    yep_event nv;
                    e_event_init(&nv, YEP_EV_SCALAR);
                    nv.implicit = 1;
                    nv.style = YEP_STYLE_PLAIN;
                    if (emit_now(e, &nv) != 0) {
                        return -2;
                    }
                }
                yep_event mk;
                e_event_init(&mk, YEP_EV_MAP_END);
                if (emit_now(e, &mk) != 0) {
                    return -2;
                }
                st[n - 1].pair_open = 0;
                st[n - 1].pair_value = 0;
            }
            if (st[n - 1].buf_from >= 0) {
                st[n - 1].buf_from = -1;
                e->ev_scopes--;
                if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
            }
            e->pos++;
            e_event_init(&ev, st[n - 1].kind ? YEP_EV_MAP_END : YEP_EV_SEQ_END);
            /* the close indicator's span: e->pos sits just past it */
            ev.line = e->line;
            ev.col = e_col(e, e->pos - 1) + 1;
            ev.end_line = e->line;
            ev.end_col = e_col(e, e->pos) + 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            n--;
            if (n == 0) {
                if (e->ev_scopes != 0) {
                    return e_fail(e, YEP_ERR_INTERNAL, e->pos);
                }
                if (e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
                e_skip_inline_space(e);
                return 0;
            }
            st[n - 1].just_closed = 1;
            if (st[n - 1].pair_open) {
                st[n - 1].pair_value = 1;
            }
            if (st[n - 1].buf_from >= 0) {
                st[n - 1].entry_line = e->line; /* the nested key ended here */
            }
            continue;
        }
        if (c == ':') {
            e->pos++;
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 2) {
                /* ':' after a bare key completes it (value next) */
                st[n - 1].pending_key = 1;
                continue;
            }
            if (st[n - 1].kind == 1 && st[n - 1].just_closed) {
                /* '[a, b]: v' inside a map — the closed collection is the key */
                st[n - 1].just_closed = 0;
                st[n - 1].pending_key = 1;
                continue;
            }
            if (st[n - 1].kind == 1 && st[n - 1].pending_key == 0 &&
                (st[n - 1].sep || st[n - 1].entries == 0)) {
                /* '{: x' or '{a, : x' — empty key at a fresh entry */
                yep_event kv;
                e_event_init(&kv, YEP_EV_SCALAR);
                kv.implicit = 1;
                if (emit_now(e, &kv) != 0) {
                    return -2;
                }
                st[n - 1].pending_key = 1;
                st[n - 1].entries = 1;
                continue;
            }
            if (st[n - 1].kind == 0 && st[n - 1].buf_from >= 0) {
                if (st[n - 1].pair_open && st[n - 1].pair_value) {
                    /* "[s: 2\n:]" — the loop-top twin of the inline
                     * colon guard: a buffered value cannot wrap as
                     * another pair once the open pair is complete */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                if (st[n - 1].entry_line != e->line) {
                    /* the key ended on an earlier line */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                /* '[ [a, b]: v' — the buffered collection is the pair key */
                int32_t bf0 = st[n - 1].buf_from;
                int was_empty = ((uint32_t)bf0 == e->ev_buf_n);
                st[n - 1].buf_from = -1;
                e->ev_scopes--;
                if (e_buf_wrap(e, (uint32_t)bf0) != 0) {
                    return -2;
                }
                if (e->ev_scopes == 0 && e->ev_buf_n > 0) {
                    int brc = e_buf_send(e, 0);
                    if (brc != 0) {
                        return brc;
                    }
                }
                if (was_empty) {
                    yep_event kv; /* "[ : v" — the empty key */
                    e_event_init(&kv, YEP_EV_SCALAR);
                    kv.implicit = 1;
                    if (emit_now(e, &kv) != 0) {
                        return -2;
                    }
                }
                st[n - 1].pair_open = 1;
                continue; /* value next */
            }
            if (st[n - 1].kind == 0 && n >= 1) {
                if (st[n - 1].pair_open) {
                    /* "[s: : v]" — ':' in the open pair's value slot
                     * is not node content (libyaml: "did not find
                     * expected node content") */
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                }
                /* '[ : v' — single-pair mapping with an empty key */
                if (n >= e->max_depth) {
                    return e_fail(e, YEP_ERR_DEPTH, e->pos);
                }
                yep_event mk;
                e_event_init(&mk, YEP_EV_MAP_START);
                mk.flow = 1;
                e_pos_mark(e, e->pos, &mk.line, &mk.col);
                mk.end_line = mk.line; /* virtual opener: zero-span */
                mk.end_col = mk.col;
                if (emit_now(e, &mk) != 0) {
                    return -2;
                }
                yep_event kv;
                e_event_init(&kv, YEP_EV_SCALAR);
                kv.implicit = 1;
                if (emit_now(e, &kv) != 0) {
                    return -2;
                }
                st[n - 1].sep = 0;
                st[n - 1].entries++;
                e_flow_frame_init(st, n, 1, 1);
                st[n].pair_wrap = 1; /* the seq single-pair wrapper */
                n++;
                continue;
            }
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        /* any other stray byte */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
}

/* -------------------------------------------------------- block machine */

/* -------------------------------------------- classified fast arms (49) */
/* Dispatch decided once per line from scan's shape facts. Each arm
 * mirrors the general chain's emission sequence exactly; returns
 * 1 = node complete, 0 = not applicable, <0 error to propagate. */

/* Plain (optionally anchored) value: emit, fold continuations, land at
 * the line end. root_ctx is 0 — every arm has pushed a frame. */
static int e_shape_plain_value(yep_engine* e, const yep_line_shape* sh, uint16_t floor,
                               yep_view anchor, uint32_t anchor_id) {
    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.anchor = anchor;
    ev.anchor_id = anchor_id;
    ev.value.p = e->p + sh->val_span.start;
    ev.value.len = sh->val_span.end - sh->val_span.start;
    ev.borrowed = 1;
    ev.style = YEP_STYLE_PLAIN;
    ev.implicit = 1;
    ev.line = e->line;
    ev.col = e_col(e, sh->val_start) + 1;
    e->pos = sh->val_span.end;
    if (e_plain_multiline(e, sh->val_span, floor, &ev, 0) != 0) {
        return -1;
    }
    if (e->fold_n == 1 && sh->val_span.term == YEP_TERM_COMMENT) {
        e_skip_to_eol(e); /* single line ending in a comment */
    }
    /* Dash-item fast path (TODO.restructure/78): the prepared value
     * is offered whole — the same contract as the pair batch; the
     * top frame is the sequence e_open_seq just opened/continued. */
    if (e->sink != NULL && e->sink->on_block_item != NULL) {
        yep_block_value v;
        memset(&v, 0, sizeof(v));
        v.cls = YEP_LVAL_PLAIN;
        v.borrowed = ev.borrowed;
        v.value = ev.value;
        int built = e->sink->on_block_item(e->sink->ctx, &v);
        if (built < 0) {
            return -2;
        }
        if (built == 1) {
            return 1;
        }
    }
    return emit_now(e, &ev) == 0 ? 1 : -2;
}

/* Flow value: the JSON fast path first; anything else restores the
 * cursor and runs the value chain (which re-derives the flow arms). */
static int e_shape_flow_value(yep_engine* e, const yep_line_shape* sh, uint16_t floor,
                              yep_ctx next) {
    yep_view none = {NULL, 0};
    e->pos = sh->val_start;
    e->flow_floor = floor;
    e->flow_enforce = (e->depth > 0) && !e->compat_grammar;
    int fast = e_flow_json(e, none, none, 0);
    e->flow_enforce = 0;
    if (fast == -2) {
        return -2;
    }
    if (fast == 1) {
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "k: [1] x" */
        }
        return 1;
    }
    e->pos = sh->val_start;
    int rc = e_parse_value(e, next, floor);
    return rc == 0 ? 1 : rc;
}

/* Alias value; a ':' after the name is a compact key under a dash and
 * the error the general path raises under a key. */
static int e_shape_alias_value(yep_engine* e, const yep_line_shape* sh, int under_key) {
    yep_view none = {NULL, 0};
    e->pos = sh->val_start;
    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.line = e->line;
    ev.col = e_col(e, e->pos) + 1;
    if (e_alias(e, &ev) != 0) {
        return -1;
    }
    if (e_colon_at(e, e->pos)) {
        if (under_key || ev.multiline) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (e_simple_key_ok(e, sh->val_start, e->pos) != 0) {
            return -1;
        }
        uint16_t key_col = (uint16_t)e_col(e, sh->val_start);
        int rc = e_open_map(e, key_col, e->line, key_col + 1, none, none, 0, 0);
        if (rc != 0) {
            return rc;
        }
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
        e->pos++; /* ':' */
        rc = e_parse_value(e, YEP_CTX_AFTER_COLON, key_col);
        return rc == 0 ? 1 : rc;
    }
    if (!e_at_eol(e)) {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    return emit_now(e, &ev) == 0 ? 1 : -2;
}

/* The classified line's key event — constructed ONLY on paths that
 * emit it: the pair path takes the whole line without events, and
 * building kv up front discarded ~48 stores per classified line
 * (TODO.restructure/63 phase 1). */
static void e_key_event(const yep_engine* e, const yep_line_shape* sh, uint16_t key_col,
                        uint32_t line, yep_event* kv) {
    e_event_init(kv, YEP_EV_SCALAR);
    kv->style = YEP_STYLE_PLAIN;
    kv->implicit = 1;
    kv->value.p = e->p + sh->key_start;
    kv->value.len = sh->key_end - sh->key_start;
    kv->borrowed = 1;
    kv->line = line;
    kv->col = key_col + 1;
    /* keys are single-line by the simple-key law; the cursor may have
     * advanced past the line, so the mark comes from e_pos_mark */
    e_pos_mark(e, sh->key_end, &kv->end_line, &kv->end_col);
}

static int e_classified(yep_engine* e, uint16_t floor_col) {
    yep_line_info li = e_line_info_here(e);
    if (e->pos != e->line_start + li.indent) {
        return 0; /* mid-line entry: the chain keeps its dispatch */
    }
    const yep_line_shape* sh = e_shape_here(e);
    if (sh->val == YEP_LVAL_NONE) {
        return 0; /* classified line, unclassified value: bail whole */
    }
    yep_view none = {NULL, 0};

    if (sh->kind == YEP_LSHAPE_DASH) {
        switch (sh->val) {
        case YEP_LVAL_EMPTY:
        case YEP_LVAL_FLOW:
        case YEP_LVAL_ALIAS:
        case YEP_LVAL_PLAIN:
            break; /* the anchored entry and anything else: general chain */
        default:
            return 0;
        }
        uint16_t col = (uint16_t)e_col(e, e->pos);
        int rc = e_open_seq(e, col, e->line, col + 1, none, none, 0);
        if (rc != 0) {
            return rc;
        }
        switch (sh->val) {
        case YEP_LVAL_EMPTY:
            e->pos = sh->dash + 1;
            rc = e_parse_value(e, YEP_CTX_AFTER_DASH, col);
            return rc == 0 ? 1 : rc;
        case YEP_LVAL_FLOW:
            return e_shape_flow_value(e, sh, col, YEP_CTX_AFTER_DASH);
        case YEP_LVAL_ALIAS:
            return e_shape_alias_value(e, sh, 0);
        default:
            return e_shape_plain_value(e, sh, col, none, 0);
        }
    }

    /* KEY: plain key + ':' — the mapping workhorse line */
    if (sh->kind != YEP_LSHAPE_KEY) {
        return 0;
    }
    if (e_simple_key_ok(e, sh->key_start, sh->key_end) != 0) {
        return -1;
    }
    uint16_t key_col = (uint16_t)e_col(e, e->pos);
    /* `key:` with the value on following lines, opening a FRESH map:
     * the sink may build map+key in one call (TODO.restructure/57) —
     * the engine then opens its frame silently. Sibling keys (the map
     * already continues at this column) keep the key-only event. */
    if (sh->val == YEP_LVAL_EMPTY && e->sink != NULL && e->sink->on_block_open != NULL &&
        !(e->depth > 0 && e->frames[e->depth - 1].kind == YEP_FRAME_MAP &&
          e->frames[e->depth - 1].col == key_col)) {
        yep_view okey = {e->p + sh->key_start, sh->key_end - sh->key_start};
        int built = e->sink->on_block_open(e->sink->ctx, &okey, e->line, key_col);
        if (built < 0) {
            return -2;
        }
        if (built == 1) {
            int orc = e_open_map(e, key_col, e->line, key_col + 1, none, none, 0, 1);
            if (orc != 0) {
                return orc;
            }
            e->pos = sh->colon + 1;
            orc = e_parse_value(e, YEP_CTX_AFTER_COLON, key_col);
            return orc == 0 ? 1 : orc;
        }
    }
    int rc = e_open_map(e, key_col, e->line, key_col + 1, none, none, 0, 0);
    if (rc != 0) {
        return rc;
    }
    /* Whole-line fast path (TODO.restructure/54): the sink may build
     * the key and its classified value as one unit — no events. The
     * value EVENT is assembled ONLY on the fallback path (the sinks
     * that take the pair read the yep_block_value and ignore it): the
     * semantic helpers run unchanged (alias resolution, anchor
     * definition, the fold), the fallback reuses the pair facts
     * captured before the fold — e_plain_multiline advances
     * line/pos across continuations, so post-fold e_col would lie. */
    /* the pair facts are captured BEFORE the fold: e_plain_multiline
     * advances line/pos across continuations */
    uint32_t pair_line = e->line;
    uint16_t pair_val_col = (uint16_t)e_col(e, sh->val_start);
    yep_block_value v;
    memset(&v, 0, sizeof(v));
    v.cls = (uint8_t)sh->val;
    yep_event vv; /* the fields the fallback reads are assigned HERE and
                   * overwritten by the arms/helpers — MSVC C4701 cannot
                   * see through the helpers' bypointer writes, so the
                   * zero-init lives in this function, not a memset */
    vv.value.p = NULL;
    vv.value.len = 0;
    vv.borrowed = 0;
    vv.anchor.p = NULL;
    vv.anchor.len = 0;
    vv.anchor_id = 0;
    vv.end_line = 0;
    vv.end_col = 0;
    int prepared = 0;
    if (sh->val == YEP_LVAL_ALIAS) {
        e->pos = sh->val_start;
        if (e_alias(e, &vv) != 0) {
            return -1; /* undefined alias: the chain's error */
        }
        if (e_colon_at(e, e->pos)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        v.value = vv.value;
        v.anchor_id = vv.anchor_id;
        prepared = 1;
    } else if (sh->val == YEP_LVAL_PLAIN || sh->val == YEP_LVAL_ANCHOR_PLAIN) {
        if (sh->val == YEP_LVAL_ANCHOR_PLAIN) {
            v.anchor.p = e->p + sh->val_start + 1;
            v.anchor.len = sh->anchor_end - sh->val_start - 1;
            v.anchor_id = anchor_define(e, v.anchor);
            vv.anchor = v.anchor;
            vv.anchor_id = v.anchor_id;
        }
        vv.value.p = e->p + sh->val_span.start;
        vv.value.len = sh->val_span.end - sh->val_span.start;
        vv.borrowed = 1;
        e->pos = sh->val_span.end;
        if (e_plain_multiline(e, sh->val_span, key_col, &vv, 0) != 0) {
            return -1;
        }
        if (e->fold_n == 1 && sh->val_span.term == YEP_TERM_COMMENT) {
            e_skip_to_eol(e); /* single line ending in a comment */
        }
        v.value = vv.value;
        v.borrowed = vv.borrowed;
        prepared = 1;
    }

    if (prepared) {
        if (e->sink != NULL && e->sink->on_block_pair != NULL) {
            yep_view key = {e->p + sh->key_start, sh->key_end - sh->key_start};
            int built =
                e->sink->on_block_pair(e->sink->ctx, &key, &v, pair_line, key_col, pair_val_col);
            if (built < 0) {
                return -2;
            }
            if (built == 1) {
                return 1;
            }
        }
        /* Fallback: assemble the value event with the helper-filled
         * fields preserved — e_alias stamps type=ALIAS + the end marks,
         * e_plain_multiline stamps the fold end marks; the pair facts
         * taken pre-fold (post-fold e_col would read the advanced
         * line/pos). Field-for-field what the pre-fill built. */
        yep_event ve;
        e_event_init(&ve, YEP_EV_SCALAR);
        ve.line = pair_line;
        ve.col = (uint32_t)pair_val_col + 1;
        ve.value = vv.value;
        ve.borrowed = vv.borrowed;
        ve.anchor = vv.anchor;
        ve.anchor_id = vv.anchor_id;
        ve.end_line = vv.end_line;
        ve.end_col = vv.end_col;
        if (sh->val == YEP_LVAL_ALIAS) {
            ve.type = YEP_EV_ALIAS;
        } else {
            ve.style = YEP_STYLE_PLAIN;
            ve.implicit = 1;
        }
        yep_event kv;
        e_key_event(e, sh, key_col, pair_line, &kv);
        if (emit_now(e, &kv) != 0 || emit_now(e, &ve) != 0) {
            return -2;
        }
        return 1;
    }

    /* EMPTY / FLOW: no prepared pair — the value is not on this line */
    switch (sh->val) {
    case YEP_LVAL_EMPTY: {
        yep_event kv;
        e_key_event(e, sh, key_col, pair_line, &kv);
        if (emit_now(e, &kv) != 0) {
            return -2;
        }
        e->pos = sh->colon + 1;
        rc = e_parse_value(e, YEP_CTX_AFTER_COLON, key_col);
        return rc == 0 ? 1 : rc;
    }
    case YEP_LVAL_FLOW: {
        yep_event kv;
        e_key_event(e, sh, key_col, pair_line, &kv);
        if (emit_now(e, &kv) != 0) {
            return -2;
        }
        return e_shape_flow_value(e, sh, key_col, YEP_CTX_AFTER_COLON);
    }
    default:
        return 0; /* PLAIN/ALIAS/ANCHOR_PLAIN handled above (unreachable) */
    }
}

/* Parses the node at the cursor (content position). Returns 0 ok,
 * -1 error, -2 sink abort. Collections may remain open (frames).
 *
 * Property semantics: same-line props belong to the node that follows
 * them (a key scalar when the line turns out to be a mapping entry);
 * pend props (parsed at the parent's value position on an earlier line)
 * belong to the VALUE node as a whole — a collection START event when
 * the value is a collection. */
static int e_node(yep_engine* e, yep_ctx ctx, uint16_t floor_col) {
    size_t node_at = e->pos;
    yep_view pend_a = e->pend_anchor, pend_t = e->pend_tag;
    uint32_t pend_aid = e->pend_anchor_id;
    e->pend_anchor.p = NULL;
    e->pend_anchor.len = 0;
    e->pend_anchor_id = 0;
    e->pend_tag.p = NULL;
    e->pend_tag.len = 0;
    yep_view anchor = {0}, tag = {0};
    e_props(e, &anchor, &tag);
    if (e->err.code != YEP_ERR_NONE) {
        return -1; /* e_props failed (unresolved tag handle) */
    }
    uint32_t anchor_ordinal = 0;
    if (!yep_view_is_empty(anchor)) {
        anchor_ordinal = anchor_define(e, anchor);
    }
    /* Node-level props for scalar/alias/flow VALUE events: same-line
     * wins over pend. */
    yep_view node_a = yep_view_is_empty(anchor) ? pend_a : anchor;
    yep_view node_t = yep_view_is_empty(tag) ? pend_t : tag;
    /* the id rides the SAME precedence as the view: same-line props win
     * over pend — a props-only line followed by its node ("&a\n- x",
     * "&a\n[b]") carried the pend VIEW with a zero id and the DOM never
     * bound the ordinal (found by the self-referencing-structures spec:
     * "--- &id001\n- *id001" failed with an EMPTY error message) */
    uint32_t node_aid = anchor_ordinal != 0 ? anchor_ordinal : pend_aid;

    /* One dispatch decision per line (TODO.restructure/49): a fresh
     * line with no pending props and a classified shape takes the fast
     * arms; every other entry keeps the chain below unchanged. */
    if ((ctx == YEP_CTX_FRESH || ctx == YEP_CTX_VALUE_LINE) && yep_view_is_empty(pend_a) &&
        yep_view_is_empty(pend_t) && pend_aid == 0) {
        int frc = e_classified(e, floor_col);
        if (frc != 0) {
            return frc > 0 ? 0 : frc;
        }
    }

    e_skip_inline_space(e);

    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.anchor = node_a;
    ev.anchor_id = node_aid;
    ev.tag = node_t;
    ev.line = e->line;
    ev.col = e_col(e, node_at) + 1;

    if (e_at_eol(e)) {
        /* properties with the value on following lines */
        if (e->depth > 0 && e->frames[e->depth - 1].kind == YEP_FRAME_MAP &&
            e->frames[e->depth - 1].col == e_col(e, node_at) &&
            (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag))) {
            /* a props-only line at an ESTABLISHED pair position: the
             * scanner never finds the simple key's ':' (libyaml:
             * "could not find expected ':' while scanning a simple
             * key"; Psych rejects "k: 1\n!!t: &b\n  n: m" and the
             * anchor-only twins). The old path consumed the line as
             * node props — the pair's key event vanished, the key's
             * tag leaked onto the value node, and a long-tag value
             * then lost its anchor at emit (the nightly's run
             * 38275208009). The FIRST pair of a fresh map ("a:\n
             * !!t: &c\n    n: m") has no frame at the column yet and
             * stays legal. */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (!yep_view_is_empty(pend_a) && !yep_view_is_empty(anchor)) {
            /* a second props line carrying its own ANCHOR displaces the
             * pend anchor (same-line-wins) — the displaced anchor stays
             * in the nametab, so a later alias emits a dangling
             * reference (the nightly's run 38271222289, "&a\n&a1\nF:
             * *a" and its '? &a' explicit-key twin). libyaml rejects
             * every nesting variant ("did not find expected <document
             * start>" / "did not find expected key"); a TAG-only
             * continuation ("&a\n!!str v") stays legal. */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        e->pend_anchor = node_a;
        e->pend_anchor_id = anchor_ordinal;
        e->pend_tag = node_t;
        e->doc_inline = 0; /* the node starts on a later line */
        if (++e->rec >= e->max_depth) {
            e->rec--;
            return e_fail(e, YEP_ERR_DEPTH, e->pos);
        }
        int rc = e_parse_value(e, ctx, floor_col);
        e->rec--;
        return rc;
        /* (ctx stays: AFTER_COLON converts to VALUE_LINE at the break) */
    }

    unsigned char c = (unsigned char)e->p[e->pos];

    if (c == ',' && (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag) ||
                     !yep_view_is_empty(pend_a) || !yep_view_is_empty(pend_t))) {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "!!str, xxx" (block) */
    }
    if (c == '-' && (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                     e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
        if (e->pos > 0 && e->p[e->pos - 1] == '\t') {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "-\t-" (Y79Y) */
        }
        if (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "&a - x" */
        }
        uint16_t col = (uint16_t)e_col(e, e->pos);
        int rc = e_open_seq(e, col, e->line, col + 1, node_a, node_t, node_aid);
        if (rc != 0) {
            return rc;
        }
        e->pos++; /* '-' */
        return e_parse_value(e, YEP_CTX_AFTER_DASH, col);
    }

    if (c == '?' && (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
                     e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
        if (ctx == YEP_CTX_AFTER_COLON) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "key: ? a" */
        }
        if (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag)) {
            /* node properties followed by a second key indicator on
             * the SAME line: libyaml "mapping keys are not allowed in
             * this context" (psych rejects "? &a ? b" and "k: &a ? b";
             * the cross-line twin "? &a\n  ? b" composes and stays
             * legal). The old path opened the nested key map with the
             * props dropped — a later alias then emitted a dangling
             * reference (the nightly's run 38355778230). */
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (e->pos > 0 && e->p[e->pos - 1] == '\t') {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "?\t-" (Y79Y) */
        }
        uint16_t col = (uint16_t)e_col(e, e->pos);
        int rc = e_open_map(e, col, e->line, col + 1, pend_a, pend_t, pend_aid, 0);
        if (rc != 0) {
            return rc;
        }
        if (e->qmd_n >= 16) {
            return e_fail(e, YEP_ERR_DEPTH, e->pos); /* the depth-guard law */
        }
        e->qmd_stack[e->qmd_n++] = e->depth;
        e->pos++;
        e_skip_inline_space(e);
        if (e->pos < e->len && e->p[e->pos] == ':' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
            size_t after = e->pos + 1;
            while (after < e->len && (e->p[after] == ' ' || e->p[after] == '\t')) {
                after++;
            }
            if (after >= e->len || e->p[after] == '\n' || e->p[after] == '\r' ||
                (e->p[after] == '#' && after > e->pos + 1)) {
                /* '? :' at end of line — empty explicit key */
                yep_event kv;
                e_event_init(&kv, YEP_EV_SCALAR);
                kv.style = YEP_STYLE_PLAIN;
                kv.implicit = 1;
                if (emit_now(e, &kv) != 0) {
                    return -2;
                }
                e->pos++;
                if (e->qmd_n > 0) {
                    e->qmd_n--; /* the ':' line supplied the innermost value */
                }
                return e_parse_value(e, YEP_CTX_AFTER_COLON, col);
            }
            /* '? : x' — the ':' starts a nested pair: the KEY is that map */
        }
        /* the explicit key is parsed as a value (may be empty → null);
         * its content may ALIGN with the '?' column */
        e->q_key_pending = 1;
        {
            int qrc = e_parse_value(e, YEP_CTX_AFTER_Q, col);
            if (qrc != 0) {
                return qrc;
            }
            e->q_value_pending = 1; /* until a ':' line supplies the value */
            return 0;
        }
    }

    if (c == '[' || c == '{') {
        /* pre-scan: is this flow collection a mapping KEY? (a key must
         * begin and end on one line — "[23\n]: 42" is not a key) */
        {
            /* JSON fast path first: its validating scan already finds the
             * close, replacing e_skip_flow's pre-scan when it applies. */
            e->flow_floor = floor_col;
            e->flow_enforce = (e->depth > 0) && !e->compat_grammar;
            int fast = e_flow_json(e, node_a, node_t, anchor_ordinal);
            e->flow_enforce = 0;
            if (fast == 1) {
                if (!e_at_eol(e)) {
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "[ a ] ]" */
                }
                if (e->depth == 0) {
                    e->last_root_flow = 1; /* "[23\n]: 42" is not a key */
                }
                return 0;
            }
            if (fast == -2) {
                return -2;
            }
        }
        size_t save = e->pos;
        uint32_t line0 = e->line;
        size_t ls0 = e->line_start;
        if (e_skip_flow(e) != 0) {
            return -1;
        }
        e_skip_inline_space(e);
        int is_key = e_colon_at(e, e->pos) && e->line == line0;
        /* the pre-scan tracks lines while it crosses them; the re-scan
         * below re-derives every mark from the open position, so the
         * line state must go back with the cursor (the nightly's run
         * 38280372447: a multi-line flow's open event marked at the
         * pre-scan's far line, diverging from the JSON fast path) */
        e->pos = save;
        e->line = line0;
        e->line_start = ls0;
        if (is_key && ctx == YEP_CTX_AFTER_COLON) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        if (is_key) {
            if (e_simple_key_ok(e, node_at, e->pos) != 0) {
                return -1;
            }
            uint16_t key_col = (uint16_t)e_col(e, node_at);
            int rc = e_open_map(e, key_col, e->line, key_col + 1, pend_a, pend_t, pend_aid, 0);
            if (rc != 0) {
                return rc;
            }
            /* the pend props went to the mapping; the key collection
             * keeps only same-line props (Psych: "!t\n[a]: v" tags
             * the map, "!t [a]: v" tags the key) */
            rc = e_flow(e, anchor, tag, anchor_ordinal);
            if (rc != 0) {
                return rc;
            }
            e->pos++; /* ':' */
            return e_pair_value_done(e, ctx, key_col);
        }
        {
            e->flow_floor = floor_col;
            e->flow_enforce = (e->depth > 0) && !e->compat_grammar;
            int rc = e_flow(e, node_a, node_t, node_aid);
            e->flow_enforce = 0;
            if (rc != 0) {
                return rc;
            }
            if (!e_at_eol(e)) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "[ a ] ]" */
            }
            if (e->depth == 0) {
                e->last_root_flow = 1; /* "[23\n]: 42" is not a key */
            }
            return 0;
        }
    }

    if (c == '"' || c == '\'') {
        if (e_quoted_floor(e, &ev, floor_col, e->depth > 0) != 0) {
            return -1;
        }
        if (e_colon_at(e, e->pos)) {
            if (ctx == YEP_CTX_AFTER_COLON || ev.multiline) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
            }
            if (e_simple_key_ok(e, node_at, e->pos) != 0) {
                return -1;
            }
            uint16_t key_col = (uint16_t)e_col(e, node_at);
            int rc = e_open_map(e, key_col, e->line, key_col + 1, pend_a, pend_t, pend_aid, 0);
            if (rc != 0) {
                return rc;
            }
            /* the pend props went to the mapping; the key keeps only
             * same-line props — ev carries the merged views and would
             * leak the map's tag onto the key (the nightly's
             * byte-instability: "!<!>\n\"\":\n" re-parsed with a
             * tagged first key and never re-stabilized) */
            ev.anchor = anchor;
            ev.anchor_id = anchor_ordinal;
            ev.tag = tag;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            e->pos++; /* ':' */
            return e_pair_value_done(e, ctx, key_col);
        }
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        return emit_now(e, &ev) == 0 ? 0 : -2;
    }

    if (c == '*') {
        if (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "&a *b" */
        }
        if (e_alias(e, &ev) != 0) {
            return -1;
        }
        if (e_colon_at(e, e->pos)) {
            if (ctx == YEP_CTX_AFTER_COLON || ev.multiline) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
            }
            if (e_simple_key_ok(e, node_at, e->pos) != 0) {
                return -1;
            }
            uint16_t key_col = (uint16_t)e_col(e, node_at);
            int rc = e_open_map(e, key_col, e->line, key_col + 1, pend_a, pend_t, pend_aid, 0);
            if (rc != 0) {
                return rc;
            }
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            e->pos++;
            return e_pair_value_done(e, ctx, key_col);
        }
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        return emit_now(e, &ev) == 0 ? 0 : -2;
    }

    if (c == '|' || c == '>') {
        int parent_col = (e->depth == 0) ? -1 : (int)floor_col;
        if (e_block_scalar(e, &ev, parent_col) != 0) {
            return -1;
        }
        return emit_now(e, &ev) == 0 ? 0 : -2;
    }

    /* plain scalar */
    if (!yep_plain_first_ok((unsigned char)e->p[e->pos])) {
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* lone "]" / "}" / "," */
    }
    yep_span s = yep_scan_plain(e->p, e->len, e->pos, 0);
    if (s.term != YEP_TERM_COLON && !yep_view_is_empty(pend_a) && !yep_view_is_empty(anchor)) {
        /* two anchors on ONE scalar ("&a
&b v"); a colon means the
         * inline anchor belongs to the key and the pend one elsewhere */
        return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
    }
    if (s.term != YEP_TERM_COLON && ctx == YEP_CTX_FRESH && e->depth > 0 &&
        e_col(e, node_at) == e->frames[e->depth - 1].col) {
        /* a plain scalar at an open container's column with no ':' —
         * neither a mapping key nor a sequence entry */
        return e_fail(e, YEP_ERR_UNEXPECTED, s.end);
    }
    if (s.term == YEP_TERM_COLON) {
        if (ctx == YEP_CTX_AFTER_COLON) {
            return e_fail(e, YEP_ERR_UNEXPECTED, s.end);
        }
        if (node_at > 0 && e->p[node_at - 1] == '\t') {
            return e_fail(e, YEP_ERR_UNEXPECTED, node_at); /* ":\tkey:" */
        }
        if (e_simple_key_ok(e, s.start, s.end) != 0) {
            return -1;
        }
        if (e->doc_inline && (!yep_view_is_empty(anchor) || !yep_view_is_empty(tag))) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "--- &a k: v" */
        }
        uint16_t key_col = (uint16_t)e_col(e, node_at);
        int rc = e_open_map(e, key_col, e->line, key_col + 1, pend_a, pend_t, pend_aid, 0);
        if (rc != 0) {
            return rc;
        }
        yep_event kv;
        e_event_init(&kv, YEP_EV_SCALAR);
        kv.anchor = anchor;
        kv.anchor_id =
            anchor_ordinal; /* the KEY binds its anchor; the
                             * DOM keys on the ordinal (a key-anchored scalar's aliases
                             * resolved to garbage without it — found by flow-direct-diff) */
        kv.tag = tag;
        kv.style = YEP_STYLE_PLAIN;
        kv.implicit = 1;
        kv.value.p = e->p + s.start;
        kv.value.len = s.end - s.start;
        kv.borrowed = 1;
        kv.line = e->line;
        kv.col = key_col + 1;
        e_pos_mark(e, s.end, &kv.end_line, &kv.end_col);
        if (emit_now(e, &kv) != 0) {
            return -2;
        }
        /* advance from the TRIMMED span end (not the stale pre-scan
         * position — the first ':' in the buffer may be INSIDE the key);
         * spaces may still precede the terminating colon */
        e->pos = s.end;
        while (e->pos < e->len && e->p[e->pos] != ':') {
            e->pos++;
        }
        e->pos++; /* past ':' */
        return e_pair_value_done(e, ctx, key_col);
    }

    ev.value.p = e->p + s.start;
    ev.value.len = s.end - s.start;
    ev.borrowed = 1;
    ev.style = YEP_STYLE_PLAIN;
    ev.implicit = 1;
    e->pos = s.end;
    if (e_plain_multiline(e, s, floor_col, &ev, e->depth == 0) != 0) {
        return -1;
    }
    if (e->fold_n == 1 && s.term == YEP_TERM_COMMENT) {
        e_skip_to_eol(e); /* single line ending in a comment */
    }
    return emit_now(e, &ev) == 0 ? 0 : -2;
}

/* Value after "key:" / "-" / "?": on this line, or on following lines. */
static int e_parse_value(yep_engine* e, yep_ctx ctx, uint16_t floor_col) {
    e_skip_inline_space(e);
    if (!e_at_eol(e)) {
        if (ctx == YEP_CTX_AFTER_COLON && e->p[e->pos] == '-' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos); /* "key: - x" */
        }
        if (ctx == YEP_CTX_AFTER_DASH && e->p[e->pos] == ':' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
            /* "- :" — a compact pair with an empty key */
            uint16_t col = (uint16_t)e_col(e, e->pos);
            int rc = e_open_map(e, col, e->line, col + 1, e->pend_anchor, e->pend_tag,
                                e->pend_anchor_id, 0);
            if (rc != 0) {
                return rc;
            }
            e->pend_anchor.p = NULL;
            e->pend_anchor.len = 0;
            e->pend_anchor_id = 0;
            e->pend_tag.p = NULL;
            e->pend_tag.len = 0;
            yep_event kv;
            e_event_init(&kv, YEP_EV_SCALAR);
            kv.implicit = 1;
            if (emit_now(e, &kv) != 0) {
                return -2;
            }
            e->pos++;
            return e_parse_value(e, YEP_CTX_AFTER_COLON, col);
        }
        int rc = e_node(e, ctx, floor_col);
        if (rc != 0) {
            return rc;
        }
        /* A plain value with continuations legitimately consumed through
         * the line break — pos sits at the next line's START; the main
         * loop must see the indentation, so do not skip it. */
        if (e->pos == e->line_start) {
            return 0;
        }
        e_skip_inline_space(e);
        if (!e_at_eol(e)) {
            return e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
        }
        return 0;
    }

    /* following lines */
    for (;;) {
        if (e->pos >= e->len) {
            goto empty_value;
        }
        yep_line_info li;
        if (e->pos == e->line_start) {
            /* seed the memo: the fast arms re-read this line's facts —
             * a direct scan made every following line double-scanned */
            li = e_line_info_here(e);
        } else {
            li = yep_scan_line(e->p, e->len, e->pos);
        }
        if (li.flags & YEP_LF_BLANK) {
            e_line_done(e, li.end);
            continue;
        }
        if (li.flags & YEP_LF_COMMENT) {
            e_line_done(e, li.end);
            continue;
        }
        if (li.flags & (YEP_LF_DOC_START | YEP_LF_DOC_END)) {
            /* a document boundary ends the pending node: the props-only
             * document ("---\n!\n---\n…") emits its empty scalar here,
             * never swallowing the next document as its value */
            goto empty_value;
        }
        if (li.indent == floor_col && e->depth > 0 &&
            (ctx == YEP_CTX_AFTER_COLON || ctx == YEP_CTX_VALUE_LINE || ctx == YEP_CTX_AFTER_Q) &&
            (li.first == '&' || li.first == '!')) {
            /* Only PROPS-ONLY lines are new nodes, not values
             * ("key: &x\n!!map\na: b"); "&anchor c: 3" is a keyed pair.
             * UNDER AN EXPLICIT '?' KEY a props-only line is worse: the
             * key would span lines waiting for a ':' that its own
             * props absorb — libyaml rejects ("could not find expected
             * ':' while scanning a simple key"), and absorbing the
             * props DROPPED the key's anchor so the emission wrote an
             * alias to an anchor that no longer existed ("? &a\n&F\n:
             * *a", the nightly's run 38207965687) */
            size_t t = li.offset + li.indent;
            int only_props = 1;
            char prev = ' '; /* '#' is a comment only after a blank —
                              * mid-token it is content ("&a ?#:" is a
                              * keyed pair whose plain carries a '#',
                              * the nightly's run 38204470906) */
            while (t < li.end) {
                char pc = e->p[t];
                if (pc == ' ' || pc == '\t') {
                    prev = pc;
                    t++;
                    continue;
                }
                int pc_comment = (pc == '#' && (prev == ' ' || prev == '\t'));
                if (pc == '&' || pc == '!' || pc == '<' || pc == ',' || pc == '>' ||
                    (pc != '\n' && pc != '\r' && !pc_comment &&
                     !yep_ct_is((unsigned char)pc, YEP_CT_FLOW_IND) && pc != ':' && pc != ' ')) {
                    prev = pc;
                    t++;
                    continue;
                }
                only_props = (pc == '\n' || pc == '\r' || pc_comment);
                break;
            }
            if (only_props) {
                return e_fail(e, YEP_ERR_UNEXPECTED, e->pos + li.indent);
            }
        }
        yep_ctx vctx = (ctx == YEP_CTX_AFTER_COLON) ? YEP_CTX_VALUE_LINE : ctx;
        /* At document root there is no parent block: a following line
         * at any indent is the value ("&a\n- x"). Explicit-key content
         * may align with the '?' column. Flow nodes and property runs
         * continue the value even at the parent column ("k:\n!!seq\n[a]"). */
        if (ctx == YEP_CTX_AFTER_Q && li.indent == floor_col && li.first != ':' &&
            li.first != '-' && li.first != '?' && yep_plain_first_ok(li.first)) {
            /* a same-column PLAIN-START pair line is a SIBLING, not
             * the key's content: the buffered "? &a" key would strand
             * inside it and an alias to the anchor emitted a dangling
             * reference ("? &a\nt: &b\n*a"; the nightly's run
             * 38261610465; libyaml: "could not find expected ':'
             * while scanning a simple key"). Dashes keep the old path
             * (a block seq is the key's content — the catch-38154662070
             * pin's shape). Stop: the null value flushes and the main
             * loop owns the pair. */
            size_t sc = li.offset + li.indent;
            int sibling = 0;
            while (sc < li.end) {
                if (e->p[sc] == ':' &&
                    (sc + 1 >= (size_t)li.end || e->p[sc + 1] == ' ' || e->p[sc + 1] == '\t' ||
                     e->p[sc + 1] == '\n' || e->p[sc + 1] == '\r')) {
                    sibling = 1;
                    break;
                }
                sc++;
            }
            if (sibling) {
                goto empty_value;
            }
        }
        if (li.indent > floor_col || e->depth == 0 ||
            (ctx == YEP_CTX_AFTER_Q && li.indent == floor_col && li.first != ':') ||
            ((li.first == '[' || li.first == '{') && li.indent == floor_col) ||
            /* libyaml never validates a block-scalar HEADER's column —
             * only the content must out-indent the parent (e_block_scalar
             * enforces that), so "|" / ">" at the key's own column is the
             * pending value (yeptris-ruby#258: the metanorma collection
             * fixture shape) */
            (vctx == YEP_CTX_VALUE_LINE && (li.first == '|' || li.first == '>') &&
             li.indent == floor_col)) {
            /* an empty-key ':' line cannot be a pending value's
             * content — libyaml errors ("did not find expected key";
             * the nightly's "- &a\n :" byte-instability crasher).
             * The same-line empty-key law lives in the shape arms. */
            if (li.first == ':') {
                size_t at2 = e->pos + li.indent + 1;
                if (at2 >= e->len || e->p[at2] == ' ' || e->p[at2] == '\t' || e->p[at2] == '\n' ||
                    e->p[at2] == '\r') {
                    return e_fail(e, YEP_ERR_UNEXPECTED, e->pos + li.indent);
                }
            }
            e->pos += li.indent;
            if (++e->rec >= e->max_depth) {
                e->rec--;
                return e_fail(e, YEP_ERR_DEPTH, e->pos);
            }
            {
                int rc = e_node(e, vctx, floor_col);
                e->rec--;
                return rc;
            }
        }
        /* indentless sequence as a mapping value; under a sequence frame
         * a dash at the same column is a SIBLING, not a continuation */
        if (li.indent == floor_col && li.first == '-' &&
            ((ctx == YEP_CTX_AFTER_DASH && e->depth > 0 &&
              e->frames[e->depth - 1].kind == YEP_FRAME_MAP &&
              e->frames[e->depth - 1].col == floor_col) ||
             vctx == YEP_CTX_VALUE_LINE) &&
            (li.offset + li.indent + 1 >= e->len || e->p[li.offset + li.indent + 1] == ' ' ||
             e->p[li.offset + li.indent + 1] == '\t' || e->p[li.offset + li.indent + 1] == '\n' ||
             e->p[li.offset + li.indent + 1] == '\r')) {
            e->pos += li.indent;
            return e_node(e, ctx == YEP_CTX_AFTER_DASH ? ctx : vctx, floor_col);
        }
        goto empty_value;
    }

empty_value: {
    yep_event ev;
    e_event_init(&ev, YEP_EV_SCALAR);
    ev.style = YEP_STYLE_PLAIN;
    ev.implicit = 1;
    ev.anchor = e->pend_anchor;
    ev.tag = e->pend_tag;
    ev.anchor_id = e->pend_anchor_id;
    e->pend_anchor.p = NULL;
    e->pend_anchor.len = 0;
    e->pend_anchor_id = 0;
    e->pend_tag.p = NULL;
    e->pend_tag.len = 0;
    { /* libyaml: the implicit null sits just past the key's colon
       * ("k:   " -> col 2, not the line end), zero-span */
        size_t at = e->pos;
        if (at > e->len) {
            at = e->len; /* the cursor may stand one past EOF after a
                          * flow close scan (the nightly's run
                          * 38147359781, '[&"[?:\n?(\nu\n?"]:...') —
                          * zero-copy means the mark reads in-bounds */
        }
        while (at > 0 && (e->p[at - 1] == ' ' || e->p[at - 1] == '\t' || e->p[at - 1] == '\n' ||
                          e->p[at - 1] == '\r')) {
            at--; /* the cursor may sit lines past the deferred key */
        }
        if (at == 0 || e->p[at - 1] != ':') {
            at = e->pos; /* not a plain "key:" defer: the cursor stands */
        }
        e_pos_mark(e, at, &ev.line, &ev.col);
        ev.end_line = ev.line;
        ev.end_col = ev.col;
    }
    return emit_now(e, &ev) == 0 ? 0 : -2;
}
}

/* ------------------------------------------------------------ lifecycle */

void yep_engine_set_resolver(yep_engine* e, const yep_resolver* r) {
    if (e != NULL) {
        e->resolver = r != NULL ? r : yep_resolver_core12();
    }
}

void yep_engine_set_compat_grammar(yep_engine* e, int on) {
    if (e != NULL) {
        e->compat_grammar = on != 0;
    }
}

void yep_engine_set_max_depth(yep_engine* e, int depth) {
    if (e != NULL && depth > 0) {
        e->max_depth = depth > YEP_MAX_DEPTH ? YEP_MAX_DEPTH : depth;
    }
}

yep_pool* yep_engine_detach_pool(yep_engine* e) {
    if (e == NULL || e->pool == NULL) {
        return NULL;
    }
    yep_pool* p = e->pool;
    e->pool = yep_pool_create(e->sys, 8192); /* fresh: engine stays usable */
    return p;
}

void yep_engine_prepare(yep_engine* e, const yep_text_stats* st) {
    if (e == NULL || st == NULL) {
        return;
    }
    if (st->amp > 0) {
        yep_nametab_reserve(&e->anchors, (uint32_t)st->amp);
    }
}

yep_engine* yep_engine_create(const yep_allocator* sys) {
    if (sys == NULL) {
        return NULL;
    }
    yep_pool* pool = yep_pool_create(sys, 8192);
    if (pool == NULL) {
        return NULL;
    }
    yep_engine* e = yep_alloc(sys, sizeof(yep_engine));
    if (e == NULL) {
        yep_pool_destroy(pool);
        return NULL;
    }
    memset(e, 0, sizeof(*e));
    e->sys = sys;
    e->pool = pool;
    e->max_depth = YEP_MAX_DEPTH;
    if (!yep_nametab_init(&e->anchors, sys)) {
        yep_free(e->sys, e);
        yep_pool_destroy(pool);
        return NULL;
    }
    yep_error_clear(&e->err);
    return e;
}

/* YAML 1.2 line breaks beyond \n/\r: NEL, LS, PS. When present, the
 * input is copied with each normalized to a single '\n' (libyaml reader
 * behavior); inputs without them keep the zero-copy path. The copied
 * length is not strlen()'s: the INPUT may carry NUL bytes of its own,
 * and a strlen() of the copy truncates the run at the first one (the
 * nightly's run 38524928004: "\0\n---\r<U+2028>" whole-parsed as an
 * empty stream while the chunked cut skipped the copy). */
static const char* e_normalize_breaks(yep_engine* e, const char* p, size_t len, size_t* out_len) {
    char* out = yep_engine_normalize_breaks(e->sys, p, len, out_len);
    if (out == NULL) {
        return p;
    }
    e->norm_buf = out;
    return out;
}

/* The NEL/LS/PS -> '\n' copy (the one-shot path rides this BEFORE the
 * engine so the normalized bytes own the document's `transcoded` slot;
 * the engine's per-feed copy stays for the streaming feeds). Returns
 * NULL when the input has no exotic break. */
/* The two possible LEAD bytes of a Unicode break (NEL = C2 85; LS/PS =
 * E2 80 A8/A9). The whole-document probe rides the SIMD stopset kernel
 * and only candidates get the byte check — the per-byte walk was 11%
 * of every parse (exclusive profile, block-heavy DOM, 2026-09-27).
 * Pinned against yep_stopset_init by Parse.NormBreakLeadsMatchRuntimeBuild. */
const yep_stopset yep_norm_break_leads = {
    .bitmap = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00},
    .groups = 1,
    .lo = {{0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00}},
    .hi = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
            0x02, 0x00}},
};

char* yep_engine_normalize_breaks(const yep_allocator* sys, const char* p, size_t len,
                                  size_t* out_len) {
    const unsigned char* q = (const unsigned char*)p;
    const yep_text_kernels* k = yep_text_active();
    size_t i = 0;
    int found = 0;
    while (i < len) {
        ptrdiff_t hit = k->stopset_find(&yep_norm_break_leads, (const char*)q + i, len - i);
        if (hit < 0) {
            break;
        }
        i += (size_t)hit;
        if (q[i] == 0xC2 && i + 1 < len && q[i + 1] == 0x85) {
            found = 1;
            break;
        }
        if (q[i] == 0xE2 && i + 2 < len && q[i + 1] == 0x80 &&
            (q[i + 2] == 0xA8 || q[i + 2] == 0xA9)) {
            found = 1;
            break;
        }
        i++;
    }
    if (!found) {
        return NULL;
    }
    char* out = yep_alloc(sys, len + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t j = 0; j < len;) {
        if (q[j] == 0xC2 && j + 1 < len && q[j + 1] == 0x85) {
            out[o++] = '\n';
            j += 2;
        } else if (q[j] == 0xE2 && j + 2 < len && q[j + 1] == 0x80 &&
                   (q[j + 2] == 0xA8 || q[j + 2] == 0xA9)) {
            out[o++] = '\n';
            j += 3;
        } else {
            out[o++] = p[j++];
        }
    }
    out[o] = '\0';
    if (out_len != NULL) {
        *out_len = o;
    }
    return out;
}

typedef struct yep_stepstate {
    char* buf;
    size_t len, cap;
    size_t scanned;     /* bytes folded into quote/flow/comment state */
    int in_flow;        /* open [ { depth */
    int quote;          /* 0 none, else the opening quote char */
    int in_verbatim;    /* inside a "!<...>" verbatim tag: cuts would
                         * split the tag across the chunk boundary */
    int in_comment;     /* inside a # comment (until the line break) */
    int saw_directive;  /* a % line awaits its --- document */
    int started;        /* STREAM_START emitted */
    uint32_t line_base; /* whole-stream lines consumed by earlier runs */
    int bs_open;        /* a block scalar's content lines are pending: a
                         * '>' / '|' header was seen and its terminator
                         * has not */
    uint32_t bs_indent; /* the header line's indent: the content floor */
    int tok_start;      /* the next word may begin here: quotes are
                         * tokens only at a word start (the "A!'- %YAML"
                         * plain's apostrophe is content, not a quote) */
    int word_led;       /* the current word began with an indicator */
} yep_stepstate;

static int engine_run_impl(yep_engine* e, const char* buf, size_t len, const yep_sink* sink,
                           int emit_start, int emit_end);

int yep_engine_run(yep_engine* e, const char* buf, size_t len, const yep_sink* sink) {
    return engine_run_impl(e, buf, len, sink, 1, 1);
}

void yep_engine_destroy(yep_engine* e) {
    if (e == NULL) {
        return;
    }
    if (e->step != NULL) {
        yep_stepstate* st = e->step;
        free(st->buf);
        free(st);
        e->step = NULL;
    }
    yep_nametab_free(&e->anchors);
    yep_free(e->sys, e->norm_buf);
    yep_pool_destroy(e->pool);
    yep_free(e->sys, e);
}

/* ---- resumable stepping (TODO.impl/07) --------------------------------
 *
 * The pending buffer holds raw bytes; each step parses the complete-
 * document prefix and keeps the tail. A cut is safe only at the start
 * of a column-0 document marker (--- / ...) while no quoted scalar,
 * comment, or flow collection spans it — tracked incrementally as
 * chunks arrive. Anything ambiguous (lone-\r line breaks, an
 * unterminated marker at the buffer edge, quote pairs split by the
 * chunk boundary) merely delays the cut: buffering, never a wrong
 * split. Cuts land only at --- lines (never ...: that would orphan
 * an explicit DOCUMENT_END) and only when no % directive is pending:
 * directives travel with their document. Line numbers stay
 * stream-absolute: each run starts at the base derived from the
 * engine's own line counter, so stepped events are byte-identical to
 * whole-buffer ones. */

/* A quote opens a scalar only where a value can begin; anywhere else
 * it is literal plain text (the apostrophe in "don't"). This is exact
 * on valid YAML: a quote at a value position always opens. */
/* Advances quote/flow/comment state over buf[scanned, len) and returns
 * the offset of the LAST safe cut (0 = none). When the final bytes'
 * meaning depends on bytes not yet fed (a quote that may be doubled,
 * a marker missing its terminator), scanned is held back so the next
 * scan re-reads them with context. */
static size_t step_scan(yep_stepstate* st) {
    const char* p = st->buf;
    size_t last_cut = 0;
    size_t hold = st->len;
    size_t i = st->scanned;
    for (; i < st->len; i++) {
        char c = p[i];
        if (st->in_comment) {
            if (c == '\n') {
                st->in_comment = 0;
            }
            continue;
        }
        if (st->quote != 0) {
            if (st->quote == '\'') {
                if (c == '\'') {
                    if (i + 1 >= st->len) {
                        hold = i; /* '' or close? wait for the next byte */
                        break;
                    }
                    if (p[i + 1] == '\'') {
                        i++; /* escaped quote: still inside */
                        continue;
                    }
                    st->quote = 0;
                }
                continue;
            }
            if (c == '\\') {
                if (i + 1 >= st->len) {
                    hold = i; /* the escaped byte is unknown */
                    break;
                }
                i++;
                continue;
            }
            if (c == '"') {
                st->quote = 0;
            }
            continue;
        }
        if (st->in_verbatim) {
            if (c == '>') {
                st->in_verbatim = 0;
            }
            continue; /* verbatim tag bytes never cut (a truncated tag
                       * at the buffer end holds the tail whole) */
        }
        if (c == '#') {
            char prev = i > 0 ? p[i - 1] : '\n';
            if (prev == '\n' || prev == ' ' || prev == '\t') {
                st->in_comment = 1;
                continue;
            }
        }
        if (c == '\n') {
            st->tok_start = 1;
            st->word_led = 0;
            continue;
        }
        if (c == ' ' || c == '\t') {
            /* in a flow a space always ends the token: the next byte
             * may begin the VALUE ("{f 'x'}" — the quote after the
             * plain key is the value's opener, and missing it let the
             * cut land inside the quoted span, splitting the scalar
             * across the chunk boundary: whole saw the far close and
             * ate the '---' marker, chunked errored on the unterminated
             * prefix — the nightly's run 38534965021) */
            st->tok_start = (st->in_flow > 0) ? 1 : st->word_led;
            continue;
        }
        if (st->tok_start) {
            if (c == '!') {
                if (i + 1 >= st->len) {
                    hold = i; /* a possibly-truncated '!<': the next
                               * byte decides verbatim vs named */
                    break;
                }
                if (p[i + 1] == '<') {
                    st->in_verbatim = 1;
                    i++; /* the '<' rides */
                    continue;
                }
            }
            st->word_led = (c == '&' || c == '*' || c == '!' || c == '-' || c == '?' || c == ':' ||
                            c == ',' || c == '[' || c == '{' || c == ']' || c == '}');
            if (c == '\'' || c == '"') {
                st->quote = c; /* a quote at a word start IS a quoted token */
                st->tok_start = 0;
                continue;
            }
            st->tok_start = 0;
        }
        if (c == ',' || c == ':' || c == '[' || c == '{' || c == ']' || c == '}') {
            st->tok_start = 1; /* flow separators: the next byte may begin a
                                * token with no space ("{\"a\":...") */
        }
        if (c == '[' || c == '{') {
            st->in_flow++;
            continue;
        }
        if (c == ']' || c == '}') {
            if (st->in_flow > 0) {
                st->in_flow--;
            }
            continue;
        }
        if (st->in_flow == 0) {
            char prev = i > 0 ? p[i - 1] : '\n';
            if (prev != '\n') {
                continue;
            }
            /* Block scalars: once a '>' / '|' header is seen, its
             * content is literal and the fold reader's extent depends
             * on bytes this run may not hold (markers, comments and
             * blank lines all ride along to EOF in the lenient
             * grammar) — so consume every line without updating the
             * quote/flow/comment state and WITHOUT cutting: a split
             * inside the fold changes the scalar's value (the
             * nightly's run 38291177166, ">\n---\n" and friends).
             * The fold ends only at the reader's own rule: a dedented
             * line that is not blank, not a comment, not a marker. */
            if (st->bs_open) {
                size_t k = i;
                uint32_t ind = 0;
                while (k < st->len && p[k] == ' ') {
                    ind++;
                    k++;
                }
                if (k >= st->len) {
                    hold = i; /* an incomplete line */
                    break;
                }
                if (p[k] == '\n' || p[k] == '#') {
                    size_t e = k;
                    while (e < st->len && p[e] != '\n') {
                        e++;
                    }
                    if (e >= st->len) {
                        hold = i;
                        break;
                    }
                    i = e; /* blank or comment content: the loop's i++ crosses the break */
                    continue;
                }
                int marker =
                    (p[k] == '-' && k + 2 < st->len && p[k + 1] == '-' && p[k + 2] == '-') ||
                    (p[k] == '.' && k + 2 < st->len && p[k + 1] == '.' && p[k + 2] == '.');
                if ((p[k] == '-' || p[k] == '.') && k + 2 >= st->len) {
                    hold = i; /* a marker possibly truncated by the chunk */
                    break;
                }
                if (!marker && st->bs_indent != (uint32_t)-1 && ind <= st->bs_indent) {
                    st->bs_open = 0; /* the fold ends here: normal processing resumes */
                } else {
                    size_t e = k;
                    while (e < st->len && p[e] != '\n') {
                        e++;
                    }
                    if (e >= st->len) {
                        hold = i;
                        break;
                    }
                    i = e;
                    continue;
                }
            }
            if ((c == '>' || c == '|') &&
                (i + 1 >= st->len || p[i + 1] == ' ' || p[i + 1] == '\t' || p[i + 1] == '\n' ||
                 p[i + 1] == '\r' || (p[i + 1] >= '0' && p[i + 1] <= '9') || p[i + 1] == '+' ||
                 p[i + 1] == '-')) {
                uint32_t ind = 0;
                size_t k = i;
                while (k > 0 && p[k - 1] == ' ') {
                    k--;
                    ind++;
                }
                st->bs_open = 1;
                st->bs_indent = ind;
                size_t e = i;
                while (e < st->len && p[e] != '\n') {
                    e++;
                }
                if (e >= st->len) {
                    hold = i; /* an incomplete header line */
                    break;
                }
                i = e;
                continue;
            }
            if (c == '%') {
                st->saw_directive = 1; /* must stay with its --- line */
                continue;
            }
            if (c == '-') {
                if (i + 3 >= st->len) {
                    hold = i; /* --- possibly truncated by the chunk */
                    break;
                }
                if (p[i + 1] == '-' && p[i + 2] == '-') {
                    char after = p[i + 3];
                    if (after == '\n' || after == ' ' || after == '\t' || after == '\r') {
                        /* a doc-inline block scalar ("--- |"): the node
                         * hangs off the DOCUMENT, so its fold follows
                         * the root rule — only a later marker ends it
                         * (col-0 lines are content). The line-start
                         * header check below cannot see it (the '|'
                         * rides mid-line after the marker), and
                         * without the state the fold's content
                         * poisons the quote/flow tracking (the
                         * nightly's run 38352718743). */
                        if (!st->bs_open) {
                            size_t k2 = i + 3;
                            while (k2 < st->len && (p[k2] == ' ' || p[k2] == '\t')) {
                                k2++;
                            }
                            if (k2 < st->len && (p[k2] == '>' || p[k2] == '|')) {
                                st->bs_open = 1;
                                st->bs_indent = (uint32_t)-1; /* root rule: markers only */
                            }
                        }
                        if (st->saw_directive == 0 && i > 0 && !st->bs_open) {
                            last_cut = i; /* interior document boundary */
                        }
                        st->saw_directive = 0; /* this --- owns the directives */
                    }
                }
            }
        }
    }
    st->scanned = hold < st->len ? hold : st->len;
    return last_cut;
}

int yep_engine_step(yep_engine* e, const char* chunk, size_t len, int final, const yep_sink* sink) {
    if (e == NULL || (chunk == NULL && len != 0)) {
        return -1;
    }
    if (e->step == NULL) {
        e->step = calloc(1, sizeof(yep_stepstate));
        if (e->step == NULL) {
            return -1;
        }
    }
    yep_stepstate* st = e->step;
    st->tok_start = 1; /* the buffer starts at a line start */
    if (len > 0) {
        if (st->len + len > st->cap) {
            size_t cap = st->cap ? st->cap : 4096;
            while (cap < st->len + len) {
                cap *= 2;
            }
            char* nb = realloc(st->buf, cap);
            if (nb == NULL) {
                return -1;
            }
            st->buf = nb;
            st->cap = cap;
        }
        memcpy(st->buf + st->len, chunk, len);
        st->len += len;
    }
    if (!final) {
        size_t cut = step_scan(st);
        if (cut == 0 || cut >= st->len) {
            return 0; /* no complete document yet */
        }
        e->line_base = st->line_base;
        int rc = engine_run_impl(e, st->buf, cut, sink, !st->started, 0);
        if (rc != 0) {
            return rc;
        }
        st->started = 1;
        st->line_base = e->line - 1; /* the engine consumed to here */
        memmove(st->buf, st->buf + cut, st->len - cut);
        st->len -= cut;
        st->scanned = 0;
        /* the tail began at a --- line in a clean state */
        st->in_flow = 0;
        st->quote = 0;
        st->in_verbatim = 0;
        st->in_comment = 0;
        st->saw_directive = 0;
        return 0;
    }
    /* final: parse everything that remains and close the stream */
    e->line_base = st->line_base;
    int rc = engine_run_impl(e, st->buf, st->len, sink, !st->started, 1);
    free(st->buf);
    free(st);
    e->step = NULL;
    e->line_base = 0;
    return rc;
}

static int engine_run_impl(yep_engine* e, const char* buf, size_t len, const yep_sink* sink,
                           int emit_start, int emit_end) {
    if (e == NULL || (buf == NULL && len != 0)) {
        return -1;
    }
    yep_free(e->sys, e->norm_buf);
    e->norm_buf = NULL;
    e->parsing_final = emit_end;
    size_t norm_len = len;
    e->p = e_normalize_breaks(e, buf, len, &norm_len);
    e->len = norm_len;
    e->pos = 0;
    e->line = 1 + e->line_base;
    e->line_base = 0;
    e->line_start = 0;
    e->li_cache_line = 0;
    e->shape_cache_line = 0;
    e->facts_cache_line = 0;
    e->depth = 0;
    yep_nametab_clear(&e->anchors);
    e->alias_memo_len = 0;
    e->tagmap_n = 0;
    e->saw_yaml = 0;
    e->doc_begin = (size_t)-1;
    e->doc_content = 0;
    e->q_key_pending = 0;
    e->q_value_pending = 0;
    e->qmd_n = 0;
    e->sink = sink;
    yep_error_clear(&e->err);
    if (e->resolver == NULL) {
        e->resolver = yep_resolver_core12();
    }

    yep_event ev;
    if (emit_start) {
        e_event_init(&ev, YEP_EV_STREAM_START);
        ev.line = 1;
        ev.col = 1;
        ev.end_line = 1;
        ev.end_col = 1;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
    }

    int doc_open = 0;

    while (e->pos < e->len) {
        /* pos may sit MID-line here (e.g. at a comment left by the
         * "--- # c" inline path, whose e_line_done was a no-op): the
         * memo scans line_start and would rewind — scan from pos, the
         * pre-memo semantics */
        yep_line_info li =
            e->pos == e->line_start ? e_line_info_here(e) : yep_scan_line(e->p, e->len, e->pos);
        if ((li.flags & YEP_LF_TAB) && !(li.flags & YEP_LF_BLANK)) {
            size_t t = li.offset + li.indent;
            while (t < li.end && (e->p[t] == ' ' || e->p[t] == '\t')) {
                t++;
            }
            if (t >= li.end || (e->p[t] != '[' && e->p[t] != '{')) {
                e_fail(e, YEP_ERR_TAB_IN_INDENT, e->pos + li.indent);
                goto fail;
            }
            li.indent = (uint16_t)(t - li.offset);
        }
        if (li.flags & YEP_LF_DIRECTIVE) {
            if (doc_open) {
                e_fail(e, YEP_ERR_BAD_DIRECTIVE, e->pos);
                goto fail;
            }
            if (e->saw_yaml) {
                e_fail(e, YEP_ERR_BAD_DIRECTIVE, e->pos); /* repeated %YAML */
                goto fail;
            }
            {
                /* %YAML takes exactly one version argument */
                size_t chk = li.offset + li.indent + 1;
                if (li.end - chk >= 4 && memcmp(e->p + chk, "YAML", 4) == 0) {
                    e->saw_yaml = 1;
                    size_t a0 = chk + 4;
                    while (a0 < li.end && e->p[a0] == ' ') {
                        a0++;
                    }
                    size_t a1 = a0;
                    int dots = 0;
                    while (a1 < li.end &&
                           ((e->p[a1] >= '0' && e->p[a1] <= '9') || e->p[a1] == '.')) {
                        dots += (e->p[a1] == '.');
                        a1++;
                    }
                    size_t rest = a1;
                    while (rest < li.end && e->p[rest] == ' ') {
                        rest++;
                    }
                    if (rest < li.end && e->p[rest] == '#' && rest > a1) {
                        rest = li.end; /* trailing comment (blank before #) */
                    }
                    if (a1 == a0 || dots != 1 || rest < li.end || e->p[a0] == '.' ||
                        e->p[a1 - 1] == '.') {
                        e_fail(e, YEP_ERR_BAD_DIRECTIVE, chk);
                        goto fail;
                    }
                }
            }
            /* %TAG <handle> <prefix> — registered for the next document */
            size_t d = li.offset + li.indent + 1;
            while (d < li.end && e->p[d] == ' ') {
                d++;
            }
            if (li.end - d >= 3 && memcmp(e->p + d, "TAG", 3) == 0 && e->tagmap_n < 8) {
                size_t h0 = d + 3;
                while (h0 < li.end && e->p[h0] == ' ') {
                    h0++;
                }
                size_t h1 = h0;
                while (h1 < li.end && e->p[h1] != ' ' && e->p[h1] != '\t') {
                    h1++;
                }
                size_t p0 = h1;
                while (p0 < li.end && (e->p[p0] == ' ' || e->p[p0] == '\t')) {
                    p0++;
                }
                size_t p1 = p0;
                while (p1 < li.end && e->p[p1] != ' ' && e->p[p1] != '\t' && e->p[p1] != '#') {
                    p1++;
                }
                {
                    /* libyaml: only blanks and an optional comment may
                     * follow the prefix ("did not find expected comment
                     * or line break") — trailing content riding the
                     * prefix made parseable tags that never re-parsed
                     * (the nightly's "%TAG !m! !mymy 3 ..." trap) */
                    size_t r = p1;
                    while (r < li.end && (e->p[r] == ' ' || e->p[r] == '\t')) {
                        r++;
                    }
                    if (r < li.end && e->p[r] != '#') {
                        e_fail(e, YEP_ERR_BAD_DIRECTIVE, r);
                        return -1;
                    }
                }
                if (h1 > h0 && p1 > p0 && e->p[h0] == '!') {
                    e->tagmap[e->tagmap_n].handle.p = e->p + h0;
                    e->tagmap[e->tagmap_n].handle.len = (uint32_t)(h1 - h0);
                    e->tagmap[e->tagmap_n].prefix.p = e->p + p0;
                    e->tagmap[e->tagmap_n].prefix.len = (uint32_t)(p1 - p0);
                    e->tagmap_n++;
                }
            }
            if (e->doc_begin == (size_t)-1) {
                e->doc_begin = li.offset; /* libyaml: DOCUMENT_START's
                                           * start_mark includes the
                                           * leading directive lines */
            }
            e_line_done(e, li.end);
            continue;
        }
        if (li.flags & (YEP_LF_BLANK | YEP_LF_COMMENT)) {
            e_line_done(e, li.end);
            continue;
        }
        if (li.flags & YEP_LF_DOC_START) {
            int rc = e_close_to(e, 0);
            if (rc != 0) {
                return rc;
            }
            if (doc_open) {
                if (!e->doc_content) {
                    e_event_init(&ev, YEP_EV_SCALAR);
                    ev.implicit = 1;
                    if (emit_now(e, &ev) != 0) {
                        return -2;
                    }
                }
                e->doc_content = 0;
                e_event_init(&ev, YEP_EV_DOCUMENT_END);
                e_pos_mark(e, li.offset + li.indent, &ev.line, &ev.col);
                ev.end_line = ev.line; /* zero-span at the new marker —
                                        * libyaml's implicit close mark */
                ev.end_col = ev.col;
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                doc_open = 0;
                e->tagmap_n = 0; /* directives are per-document */
                yep_nametab_clear(&e->anchors);
                e->alias_memo_len = 0; /* so are anchors */
                e->alias_memo_len = 0; /* and the repeat-alias memo: doc 1's
                                        * "*ORIGIN" memoized the ordinal and doc 2's
                                        * identical name answered from it ACROSS the
                                        * boundary (the nightly's run 38266487549) */
            }
            e_event_init(&ev, YEP_EV_DOCUMENT_START);
            ev.style = 1; /* explicit marker */
            e_pos_mark(e, e->doc_begin != (size_t)-1 ? e->doc_begin : li.offset + li.indent,
                       &ev.line, &ev.col);
            e_pos_mark(e, li.offset + li.indent + 3, &ev.end_line, &ev.end_col);
            e->doc_begin = (size_t)-1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            doc_open = 1;
            e->saw_yaml = 0;
            e->last_root_flow = 0; /* a new document may carry its own directives */
            /* the '?' / ':' pair state is per-document too: doc 4's flow
             * set q_key_pending and the boundary left it set, so doc 5's
             * bare ':' skipped its empty-key event — whole-buffer lost a
             * record the chunked feed (per-run reset) emitted (the
             * nightly's run 38287390219) */
            e->q_key_pending = 0;
            e->q_value_pending = 0;
            e->qmd_n = 0;
            e->pos = li.offset + li.indent + 3;
            e_skip_inline_space(e);
            if (!e_at_eol(e)) {
                e->doc_inline = 1;
                rc = e_node(e, YEP_CTX_FRESH, li.indent);
                e->doc_inline = 0;
                if (rc != 0) {
                    if (rc == -2) {
                        return -2;
                    }
                    goto fail;
                }
                e->doc_content = 1;
            }
            e_line_done(e, e->pos);
            continue;
        }
        if (li.flags & YEP_LF_DOC_END) {
            {
                size_t rest = li.offset + li.indent + 3;
                while (rest < e->len &&
                       (e->p[rest] == ' ' || e->p[rest] == '\t' || e->p[rest] == '\r')) {
                    rest++;
                }
                if (rest < e->len && e->p[rest] != '\n' && e->p[rest] != '#') {
                    e_fail(e, YEP_ERR_UNEXPECTED, rest);
                    goto fail;
                }
            }
            int rc = e_close_to(e, 0);
            if (rc != 0) {
                return rc;
            }
            if (doc_open) {
                if (!e->doc_content) {
                    e_event_init(&ev, YEP_EV_SCALAR);
                    ev.implicit = 1;
                    if (emit_now(e, &ev) != 0) {
                        return -2;
                    }
                }
                e_event_init(&ev, YEP_EV_DOCUMENT_END);
                ev.style = 1; /* explicit "..." marker */
                e_pos_mark(e, li.offset + li.indent, &ev.line, &ev.col);
                e_pos_mark(e, li.offset + li.indent + 3, &ev.end_line, &ev.end_col);
                e->tagmap_n = 0; /* directives are per-document */
                yep_nametab_clear(&e->anchors);
                e->alias_memo_len = 0;
                if (emit_now(e, &ev) != 0) {
                    return -2;
                }
                doc_open = 0;
                e->doc_content = 0;
                e->saw_yaml = 0;
                e->doc_begin = (size_t)-1;
            }
            e_line_done(e, li.end);
            continue;
        }

        if (!doc_open) {
            e_event_init(&ev, YEP_EV_DOCUMENT_START);
            if (e->doc_begin != (size_t)-1) {
                e_pos_mark(e, e->doc_begin, &ev.line, &ev.col);
            } else {
                e_pos_mark(e, li.offset + li.indent, &ev.line, &ev.col);
            }
            ev.end_line = ev.line;
            ev.end_col = ev.col;
            e->doc_begin = (size_t)-1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
            doc_open = 1;
        }

        /* unwind frames that cannot continue at this column. The
         * dash-blank test is computed once per line (TODO.restructure/75
         * — it was spelled out twice below); the top frame loads once
         * per iteration. */
        uint16_t c = li.indent;
        const int dash_blank =
            li.first == '-' &&
            (li.offset + li.indent + 1 >= e->len || e->p[li.offset + li.indent + 1] == ' ' ||
             e->p[li.offset + li.indent + 1] == '\t' || e->p[li.offset + li.indent + 1] == '\n' ||
             e->p[li.offset + li.indent + 1] == '\r');
        while (e->depth > 0) {
            const yep_frame* fr = &e->frames[e->depth - 1];
            if (fr->col > c) {
                int rc = e_close_to(e, e->depth - 1);
                if (rc != 0) {
                    return rc;
                }
                continue;
            }
            if (fr->col == c) {
                int continues;
                if (fr->kind == YEP_FRAME_SEQ) {
                    continues = dash_blank;
                } else if (fr->inline_doc) {
                    continues = 0; /* "--- k: v" maps take deeper keys only */
                } else {
                    continues = yep_scan_is_key_start(li.first) || li.first == ':' ||
                                li.first == '?' || (li.first == '-' && !dash_blank);
                }
                if (!continues) {
                    int rc = e_close_to(e, e->depth - 1);
                    if (rc != 0) {
                        return rc;
                    }
                    continue;
                }
            }
            break;
        }

        if (e->depth == 0 && e->doc_content) {
            /* a completed root scalar followed by more content: a second
             * root node in the same document ("word1  # c\nword2") */
            e_fail(e, YEP_ERR_UNEXPECTED, e->pos + li.indent);
            goto fail;
        }
        if (e->depth > 0 && e->frames[e->depth - 1].col < c) {
            /* deeper than any open container at main dispatch: nested
             * values are consumed inside e_parse_value, so this line is
             * an orphan continuation ("key: word1\n# c\n  word2") */
            e_fail(e, YEP_ERR_BAD_INDENT, e->pos + li.indent);
            goto fail;
        }
        e->pos += li.indent;
        unsigned char first = (unsigned char)e->p[e->pos];
        if (first == ':' &&
            (e->pos + 1 >= e->len || e->p[e->pos + 1] == ' ' || e->p[e->pos + 1] == '\t' ||
             e->p[e->pos + 1] == '\n' || e->p[e->pos + 1] == '\r')) {
            /* explicit-key value line, or a bare ':' pair (empty key) */
            int rc;
            if (e->last_root_flow) {
                e_fail(e, YEP_ERR_UNEXPECTED, e->pos);
                goto fail; /* a root flow node cannot become a key */
            }
            if (e->depth == 0 || e->frames[e->depth - 1].kind != YEP_FRAME_MAP) {
                rc = e_open_map(e, c, e->line, c + 1, e->pend_anchor, e->pend_tag,
                                e->pend_anchor_id, 0);
                if (rc != 0) {
                    if (rc == -2) {
                        return -2;
                    }
                    goto fail;
                }
            }
            if (!e->q_key_pending) {
                yep_event kv;
                e_event_init(&kv, YEP_EV_SCALAR);
                kv.style = YEP_STYLE_PLAIN;
                kv.implicit = 1;
                kv.line = e->line;
                kv.col = (uint32_t)e_col(e, e->pos) + 1;
                if (emit_now(e, &kv) != 0) {
                    return -2;
                }
            }
            e->q_key_pending = 0;
            e->q_value_pending = 0;
            if (e->qmd_n > 0) {
                e->qmd_n--; /* the ':' line supplied the innermost value */
            }
            e->pos++;
            rc = e_parse_value(e, YEP_CTX_AFTER_DASH, e->frames[e->depth - 1].col);
            if (rc != 0) {
                if (rc == -2) {
                    return -2;
                }
                goto fail;
            }
            e->doc_content = 1;
            e_line_done(e, e->pos);
            continue;
        }

        /* The cycle fast path (TODO.restructure/79 carve 1): a line
         * that CONTINUES the top frame (a map key at its column, a
         * seq dash) with no pending state is exactly e_classified's
         * contract — call it straight from the line loop and skip
         * e_node's prologue (pend save/clear, e_props, the gate) and
         * the dispatch tail. Non-classified shapes return 0 and take
         * the chain below unchanged. */
        if (e->depth > 0 && e->pend_anchor.p == NULL && e->pend_tag.p == NULL &&
            e->pend_anchor_id == 0 && !e->q_key_pending && !e->q_value_pending &&
            ((e->frames[e->depth - 1].kind == YEP_FRAME_MAP && e->frames[e->depth - 1].col == c &&
              !e->frames[e->depth - 1].inline_doc) ||
             (e->frames[e->depth - 1].kind == YEP_FRAME_SEQ && e->frames[e->depth - 1].col == c &&
              dash_blank))) {
            int frc = e_classified(e, e->frames[e->depth - 1].col);
            if (frc != 0) {
                if (frc < 0) {
                    if (frc == -2) {
                        return -2;
                    }
                    goto fail;
                }
                e->doc_content = 1;
                e_line_done(e, e->pos);
                continue;
            }
        }

        {
            e->last_root_flow = 0;
            int rc = e_flush_q_value(e); /* a key without its ':' line */
            if (rc != 0) {
                return -2;
            }
            rc = e_node(e, YEP_CTX_FRESH, e->depth > 0 ? e->frames[e->depth - 1].col : 0);
            if (rc != 0) {
                if (rc == -2) {
                    return -2;
                }
                goto fail;
            }
            e->doc_content = 1;
        }
        e_line_done(e, e->pos);
    }

    if ((e->tagmap_n > 0 || e->saw_yaml) && !doc_open) {
        /* a directive was never followed by "---" */
        e_fail(e, YEP_ERR_BAD_DIRECTIVE, e->len);
        goto fail;
    }
    {
        int rc = e_close_to(e, 0);
        if (rc != 0) {
            return rc;
        }
    }
    if (doc_open) {
        if (!e->doc_content) {
            e_event_init(&ev, YEP_EV_SCALAR);
            ev.implicit = 1;
            if (emit_now(e, &ev) != 0) {
                return -2;
            }
        }
        e_event_init(&ev, YEP_EV_DOCUMENT_END);
        e_pos_mark(e, e->len, &ev.line, &ev.col);
        ev.end_line = ev.line;
        ev.end_col = ev.col;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
        e->tagmap_n = 0;
        yep_nametab_clear(&e->anchors);
        e->alias_memo_len = 0;
    }
    if (emit_end) {
        e_event_init(&ev, YEP_EV_STREAM_END);
        e_pos_mark(e, e->len, &ev.line, &ev.col);
        ev.end_line = ev.line;
        ev.end_col = ev.col;
        if (emit_now(e, &ev) != 0) {
            return -2;
        }
    }
    return 0;

fail:
    *yep_error_tls() = e->err;
    return -1;
}

size_t yep_engine_pos(const yep_engine* e) {
    return e ? e->pos : 0;
}

const yep_error* yep_engine_error(const yep_engine* e) {
    return e ? &e->err : NULL;
}
