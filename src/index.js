// The public surface: YAML <-> JSON at libyeptris speed.
// Engine FFI through ../dist/yeptris.mjs (built by build.sh).

function cstr(mod, s) {
  const bytes = mod.lengthBytesUTF8(s) + 1;
  const ptr = mod._malloc(bytes);
  mod.stringToUTF8(s, ptr, bytes);
  return [ptr, bytes - 1]; // bytes - 1: the NUL is ours, not the input's
}

function freeCstr(mod, ptr) {
  mod._yeptris_wasm_free(ptr);
}

class YamlError extends Error {
  constructor(message, line, column) {
    super(line ? `${message} (line ${line}, column ${column})` : message);
    this.name = "YamlError";
    this.line = line;
    this.column = column;
  }
}

function run(mod, fn, input, ...args) {
  const [ptr, len] = cstr(mod, input);
  try {
    // len is the UTF-8 byte count: input.length counts UTF-16 code
    // units, which is strictly shorter than the wire bytes for any
    // non-ASCII text (é is 1 unit / 2 bytes) — every such document
    // parsed truncated
    const st = fn(ptr, len, ...args);
    if (st !== 0) {
      const msg = mod.UTF8ToString(mod._yeptris_wasm_last_error());
      throw new YamlError(
        msg,
        mod._yeptris_wasm_last_error_line(),
        mod._yeptris_wasm_last_error_column());
    }
    const out = mod.UTF8ToString(mod._yeptris_wasm_out(), mod._yeptris_wasm_out_len());
    return out;
  } finally {
    freeCstr(mod, ptr);
    // the C side mallocs the output per call and nothing else freed
    // it — without this a long-running process grows the wasm heap by
    // one output per call
    mod._yeptris_wasm_out_free();
  }
}

let cached = null;

async function mod() {
  if (cached) return cached;
  const factory = (await import("../dist/yeptris.mjs")).default;
  const m = await factory();
  // Fail loudly on a stale or truncated dist: a module missing the
  // engine's exports would otherwise answer every call with empty
  // strings and consumed bytes at microsecond speed (yeptris-ts#4 —
  // the published 0.1.0 measured 121k loads/s on 205 KB, i.e. no
  // parse at all).
  for (const fn of [
    "_yeptris_wasm_load",
    "_yeptris_wasm_load_all",
    "_yeptris_wasm_dump",
    "_yeptris_wasm_dump_ex",
    "_yeptris_wasm_events",
    "_yeptris_wasm_event_records",
    "_yeptris_wasm_event_count",
    "_yeptris_wasm_event_arena",
    "_yeptris_wasm_event_arena_len",
    "_yeptris_wasm_events_free",
    "_yeptris_wasm_out",
    "_yeptris_wasm_out_len",
    "_yeptris_wasm_out_free",
    "_yeptris_wasm_last_error",
    "_yeptris_wasm_free",
    "_malloc",
  ]) {
    if (typeof m[fn] !== "function") {
      throw new Error(
        `yeptris: dist/yeptris.mjs is missing the wasm export ${fn} — ` +
          "rebuild with build.sh (stale or mismatched artifact)",
      );
    }
  }
  cached = m;
  return cached;
}

/** Parse YAML text and return the document as a JavaScript value
 * (the engine's canonical JSON, JSON.parse'd). Throws YamlError with
 * line/column on malformed input. A multi-document stream answers
 * with the FIRST document (the engine's json contract); every
 * document of a stream comes from loadAll. */
export async function load(yaml) {
  const m = await mod();
  return JSON.parse(run(m, m._yeptris_wasm_load, yaml));
}

/** Parse a multi-document YAML stream and return EVERY document as a
 * JavaScript value, in stream order (js-yaml's loadAll shape). An
 * empty or comment-only stream answers []; an empty document
 * ("---\n---") answers null at its position. options is accepted for
 * call-site symmetry and reserved (load takes none today). Throws
 * YamlError with line/column on malformed input. */
export async function loadAll(yaml, options) {
  const m = await mod();
  return JSON.parse(run(m, m._yeptris_wasm_load_all, yaml));
}

/** Parse YAML text and return the engine's canonical JSON string. */
export async function loadJSON(yaml) {
  const m = await mod();
  return run(m, m._yeptris_wasm_load, yaml);
}

// ---- the event face (the recorder's bulk drain) -------------------------
//
// The engine ships three event models over the one parse: push (a
// callback per event), pull (a StAX cursor) and the recorder (fixed
// 44-byte records plus one copied string arena). Across the wasm
// boundary the recorder wins: the whole stream drains as TWO spans
// read straight from the heap — O(1) crossings, no per-event callback
// marshalling. The record layout is ABI-pinned (events.h, 44B) and
// this decoder is the JS mirror of the ctypes/FFI readers the
// ruby/py/rs faces use over the same bytes.

// YeptrisEventType (events.h, ABI-pinned order).
const EV_KINDS = {
  1: "stream-start",
  2: "stream-end",
  3: "document-start",
  4: "document-end",
  5: "sequence-start",
  6: "sequence-end",
  7: "mapping-start",
  8: "mapping-end",
  9: "scalar",
  10: "alias",
};

// YeptrisScalarStyle (dom.h). 0 = ANY ("no recorded style" — the empty
// document's null scalar) and spells as an ABSENT style, not a lie.
const EV_STYLES = {
  1: "plain",
  2: "single",
  3: "double",
  4: "literal",
  5: "folded",
};

// YeptrisTagId (resolve.h) — the resolver's typing verdict, spelled as
// the kind name (the same vocabulary ruby/py/rs consume).
const EV_TAG_IDS = {
  0: "str",
  1: "int",
  2: "float",
  3: "bool",
  4: "null",
  5: "timestamp",
  6: "seq",
  7: "map",
  8: "binary",
  9: "merge",
  10: "value",
  11: "custom",
};

// YEPTRIS_EF_* flag bits (events.h).
const EF_FLOW = 1; // collection in flow style
const EF_EXPLICIT = 2; // document with a --- / ... marker
const EF_IMPLICIT = 4; // plain (unquoted, un-block) scalar style

const EV_RECORD_SIZE = 44; // sizeof(YeptrisEventRecord), ABI-pinned

let utf8Decoder = null;

function decodeArena(u8, off, len) {
  if (utf8Decoder === null) {
    utf8Decoder = new TextDecoder("utf-8");
  }
  return utf8Decoder.decode(u8.subarray(off, off + len));
}

/** Parse YAML text and return EVERY event of the stream as an array
 * of plain objects, in stream order (leptris/yeptris#886). One event
 * is:
 *
 *   { type: "stream-start" | "stream-end" | "document-start" |
 *           "document-end" | "sequence-start" | "sequence-end" |
 *           "mapping-start" | "mapping-end" | "scalar" | "alias",
 *     line, column,        // 1-based start mark (0 = no position)
 *     endLine, endColumn,  // libyaml end_mark: col one past the end
 *     value?,              // scalar content; the alias NAME for alias
 *     anchor?, tag?,       // node properties, present when written
 *     style?,              // scalars: plain|single|double|literal|folded
 *     tagId?,              // scalars: the resolved kind (str|int|...)
 *     implicit?,           // scalars: plain style (the typing trigger)
 *     flow?,               // collection starts: flow context
 *     explicit? }          // document start/end: --- / ... marker seen
 *
 * Absent properties are omitted, never null. The faces of
 * ruby/py/rs consume the same normalized vocabulary (see
 * test/fixtures/events.json, whose header documents it as data).
 * options is accepted for call-site symmetry and reserved. Throws
 * YamlError on malformed input (position from the recorder's status;
 * its message is the generic "parse failed" — the recorder carries
 * status + position, not the error text). */
export async function events(yaml, options) {
  const m = await mod();
  const [ptr, len] = cstr(m, yaml);
  try {
    const st = m._yeptris_wasm_events(ptr, len);
    if (st !== 0) {
      throw new YamlError(
        m.UTF8ToString(m._yeptris_wasm_last_error()),
        m._yeptris_wasm_last_error_line(),
        m._yeptris_wasm_last_error_column());
    }
    const n = m._yeptris_wasm_event_count();
    const recs = m._yeptris_wasm_event_records();
    const arena = m._yeptris_wasm_event_arena();
    // Heap views fetched AFTER the call: the feed may have grown the
    // memory, and the exported views are re-bound on growth.
    const u8 = m.HEAPU8;
    const u32 = m.HEAPU32;
    const out = new Array(n);
    for (let i = 0; i < n; i++) {
      const base = recs + i * EV_RECORD_SIZE;
      const p = base >> 2;
      const type = u8[base];
      const flags = u8[base + 2];
      const ev = {
        type: EV_KINDS[type],
        line: u32[p + 1],
        column: u32[p + 2],
        endLine: u32[p + 3],
        endColumn: u32[p + 4],
      };
      const valueLen = u32[p + 6];
      if (valueLen !== 0) {
        ev.value = decodeArena(u8, arena + u32[p + 5], valueLen);
      }
      const anchorLen = u32[p + 8];
      if (anchorLen !== 0) {
        ev.anchor = decodeArena(u8, arena + u32[p + 7], anchorLen);
      }
      const tagLen = u32[p + 10];
      if (tagLen !== 0) {
        ev.tag = decodeArena(u8, arena + u32[p + 9], tagLen);
      }
      if (type === 9) { // scalar
        const style = u8[base + 1];
        if (style !== 0) {
          ev.style = EV_STYLES[style];
        }
        if ((flags & EF_IMPLICIT) !== 0) {
          ev.implicit = true;
        }
        ev.tagId = EV_TAG_IDS[u8[base + 3]];
      }
      if ((flags & EF_FLOW) !== 0) {
        ev.flow = true;
      }
      if ((type === 3 || type === 4) && (flags & EF_EXPLICIT) !== 0) {
        ev.explicit = true;
      }
      out[i] = ev;
    }
    return out;
  } finally {
    // the drain is decoded: release the recorder (nothing retained
    // past this call — the answer left the heap as JS objects)
    m._yeptris_wasm_events_free();
  }
}

/** Convert JSON text (or any JSON-serializable value) to the engine's
 * byte-stable YAML. JSON is a subset of YAML 1.2 flow, so the one
 * parse engine takes it.
 *
 * options (js-yaml parity names over emit.h's yeptris_emit_options):
 *   lineWidth — fold flow collections once the current line exceeds
 *     this column (the engine's best_width). Default: the engine
 *     default (80); -1 disables folding.
 *   canonical — the engine's fixed form: flow collections, quoted
 *     strings, typed words, shortest floats — a parse of the output
 *     re-serializes byte-identically. Default: false.
 *
 * `indent` and `flowLevel` have no engine option (the block indent is
 * fixed at 2; there is no flow-level override) — they are not accepted
 * here rather than silently ignored. Throws TypeError on a mistyped
 * known option; throws YamlError on malformed JSON input. */
export async function dump(json, options) {
  const m = await mod();
  const text = typeof json === "string" ? json : JSON.stringify(json);
  let canonical = 0;
  let lineWidth = 0; // <= 0 rides the library default (emit.h)
  if (options !== undefined && options !== null) {
    if (typeof options !== "object") {
      throw new TypeError("yeptris: dump options must be an object");
    }
    for (const key of Object.keys(options)) {
      if (key !== "lineWidth" && key !== "canonical") {
        throw new TypeError(
          `yeptris: dump option "${key}" is not supported ` +
            `(the engine surface is lineWidth + canonical)`);
      }
    }
    if (options.lineWidth !== undefined) {
      const w = options.lineWidth;
      if (typeof w !== "number" || !Number.isInteger(w) || w === 0 || w < -1) {
        throw new TypeError(
          "yeptris: dump option lineWidth must be an integer > 0 (or -1 for no folding)");
      }
      // -1 is js-yaml's "unlimited": the engine folds only past the
      // width, so the largest width IS "never fold"
      lineWidth = w === -1 ? 0x7fffffff : w;
    }
    if (options.canonical !== undefined) {
      if (typeof options.canonical !== "boolean") {
        throw new TypeError("yeptris: dump option canonical must be a boolean");
      }
      canonical = options.canonical ? 1 : 0;
    }
  }
  return run(m, m._yeptris_wasm_dump_ex, text, canonical, lineWidth);
}

/** The libyeptris version string. */
export async function version() {
  const m = await mod();
  return m.UTF8ToString(m._yeptris_wasm_version());
}

/** The wasm module instance this package caches (diagnostics:
 * heap/memory inspection, the engine's raw exports). The load/dump
 * calls all share this one instance. */
export async function wasmModule() {
  return mod();
}
