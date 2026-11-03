// The public surface: YAML <-> JSON at libyeptris speed.
// Engine FFI through ../dist/yeptris.mjs (built by build.sh).
//
// Two call postures over one initialized module: the async forms
// (load/dump/validate/...) initialize the wasm on demand and stay the
// default entry; the *Sync forms (loadSync/dumpSync/validateSync/...)
// run synchronously against the module an earlier await already
// initialized (the wasm executes synchronously — init-then-work is
// sync by construction). Every exported function shares its body
// with its Sync twin through the *Core helpers below, so the two
// postures cannot drift.

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

/** Raised by load/loadAll when the input PARSES but the schema walk
 * (schema: / jsonSchema:) produced diagnostics. Carries the whole
 * report as `diagnostics` — never first-fail, never a raw pointer.
 * Not a YamlError: the input is well-formed (the same split the
 * ruby/py faces draw between their parse errors and
 * Validation::Error / ValidationError). */
export class ValidationError extends Error {
  constructor(diagnostics) {
    const first = diagnostics[0];
    const detail = first
      ? `${first.keyword} at ${first.pointer || "(root)"}: ${first.message}`
      : "no diagnostics";
    super(`validation failed (${diagnostics.length} diagnostics): ${detail}`);
    this.name = "ValidationError";
    this.diagnostics = diagnostics;
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
    "_yeptris_wasm_each_open",
    "_yeptris_wasm_each_next",
    "_yeptris_wasm_each_frame",
    "_yeptris_wasm_each_index",
    "_yeptris_wasm_each_close",
    "_yeptris_wasm_validate",
    "_yeptris_wasm_json_schema_validate",
    "_yeptris_wasm_schema_compile",
    "_yeptris_wasm_schema_validate",
    "_yeptris_wasm_schema_free",
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

/** Initialize the wasm module (idempotent). The async forms call this
 * on demand; calling it first is what unlocks the Sync forms
 * (loadSync/dumpSync/validateSync/...), which run synchronously
 * against the same module instance. */
export async function init() {
  await mod();
}

/** The module this package caches, when an async call (or init())
 * has already initialized it; undefined before that. The Sync forms
 * exist for the initialized posture only — they refuse to lazily
 * import (that door is async by nature). */
function requireMod(who) {
  if (!cached) {
    throw new Error(
      `yeptris: ${who} needs an initialized module — await init() first ` +
        "(the async forms initialize on demand)",
    );
  }
  return cached;
}

// ---- the validate face (validate.h) ------------------------------------
//
// The engine's three doors, all fused parse+validate over the same
// front door load uses (encodings, BOM, every document of a stream):
// the SCHEMA-FAMILY walk (yeptris_validate — the YeptrisSchema kind
// rides as a parameter), the one-shot JSON SCHEMA walk (draft 2020-12
// subset), and the COMPILED economy (yeptris_schema_compile: parse
// the schema once, validate many streams). The engine carries NO kind
// on the compile door — a compiled handle is a JSON-Schema document
// by validate.h's contract; the family-kind selection lives on the
// validate door, spelled exactly as the C surface spells it.
//
// Diagnostics cross as data: the whole report drains as one JSON
// array of raw records (code/path/message/line/col/offset) and is
// mapped here into plain objects. The code -> keyword table is
// binding-owned (the same law the ruby/py faces follow); `path` is
// the JSON-pointer split into ~0/~1-unescaped segments, `pointer` its
// raw string form.

// YeptrisDiagCode -> the spec-spelled keyword (validate.h). The
// 2020-12 walk's codes name their JSON Schema keyword; the
// schema-family walk's codes name the violated YAML rule. Values are
// ABI-pinned (validate.h, appending-only).
const DIAG_KEYWORDS = {
  0: "none",
  1: "tag", // explicit tag contradicts content
  2: "keyType", // json12: a mapping key is not a string
  3: "jsonType", // json12: plain scalar matches no JSON production
  4: "duplicateKey",
  5: "type",
  6: "required",
  7: "enum",
  8: "const",
  9: "pattern",
  10: "minimum",
  11: "maximum",
  12: "exclusiveMinimum",
  13: "exclusiveMaximum",
  14: "minLength",
  15: "maxLength",
  16: "minItems",
  17: "maxItems",
  18: "uniqueItems",
  19: "additionalProperties",
  20: "anyOf",
  21: "oneOf",
  22: "not",
  23: "$ref",
  24: "false",
  25: "depth",
  26: "limit",
  27: "multipleOf",
  28: "minProperties",
  29: "maxProperties",
  30: "contains",
  31: "minContains",
  32: "maxContains",
  33: "dependentRequired",
};

// YeptrisStatus values that carry a report in the wasm out-buffer:
// 0 = valid (an empty report is a valid answer), 9 =
// YEPTRIS_ERROR_SCHEMA (invalid: drain the diagnostics). Anything
// else is a call failure (the parse itself failed).
const ST_OK = 0;
const ST_SCHEMA_ERROR = 9;

/** '/a/b/0' -> ['a', 'b', '0'], with ~1 and ~0 unescaped; '' (the
 * root) -> []. */
function pointerSegments(raw) {
  if (typeof raw !== "string" || raw.length === 0) return [];
  return raw
    .split("/")
    .slice(1)
    .map((s) => s.replaceAll("~1", "/").replaceAll("~0", "~"));
}

function toDiagnostic(raw) {
  return {
    code: raw.code,
    keyword: DIAG_KEYWORDS[raw.code] ?? `unknown-${raw.code}`,
    path: pointerSegments(raw.path),
    pointer: raw.path,
    message: raw.message,
    line: raw.line, // 1-based; 0 = unknown (unpositioned scalars)
    col: raw.col,
    offset: raw.offset,
  };
}

function drainReport(mod, st) {
  const raw = JSON.parse(
    mod.UTF8ToString(mod._yeptris_wasm_out(), mod._yeptris_wasm_out_len()));
  mod._yeptris_wasm_out_free();
  return raw.map(toDiagnostic);
}

function failFrom(mod) {
  return new YamlError(
    mod.UTF8ToString(mod._yeptris_wasm_last_error()),
    mod._yeptris_wasm_last_error_line(),
    mod._yeptris_wasm_last_error_column());
}

function runFamilyValidate(mod, input, kind) {
  const [ptr, len] = cstr(mod, input);
  try {
    const st = mod._yeptris_wasm_validate(ptr, len, kind);
    if (st === ST_OK || st === ST_SCHEMA_ERROR) return drainReport(mod, st);
    throw failFrom(mod);
  } finally {
    freeCstr(mod, ptr);
    mod._yeptris_wasm_out_free();
  }
}

function runJsonSchemaValidate(mod, schemaBytes, input) {
  const [sptr, slen] = cstr(mod, schemaBytes);
  const [ptr, len] = cstr(mod, input);
  try {
    const st = mod._yeptris_wasm_json_schema_validate(sptr, slen, ptr, len);
    if (st === ST_OK || st === ST_SCHEMA_ERROR) return drainReport(mod, st);
    throw failFrom(mod);
  } finally {
    freeCstr(mod, sptr);
    freeCstr(mod, ptr);
    mod._yeptris_wasm_out_free();
  }
}

function runCompiledValidate(mod, handle, input) {
  const [ptr, len] = cstr(mod, input);
  try {
    const st = mod._yeptris_wasm_schema_validate(handle, ptr, len);
    if (st === ST_OK || st === ST_SCHEMA_ERROR) return drainReport(mod, st);
    throw failFrom(mod);
  } finally {
    freeCstr(mod, ptr);
    mod._yeptris_wasm_out_free();
  }
}

/** A COMPILED JSON Schema (the compiled economy): the schema parses
 * once — its DOM and pattern programs retained behind one handle —
 * and validates MANY instance streams. Build with compileSchema();
 * direct construction is refused. After free() the handle is dead:
 * reuse throws, double-free is a no-op (the engine frees NULL as a
 * documented no-op; the flag makes the JS door idempotent). */
export class Schema {
  /** @private — produced by compileSchema() only */
  constructor() {
    throw new TypeError(
      "yeptris: build schemas with compileSchema(), not new Schema()");
  }

  static _take(mod, handle) {
    const s = Object.create(Schema.prototype);
    s._mod = mod; // the module the handle lives in (compile ran => initialized)
    s._handle = handle;
    s._freed = false;
    return s;
  }

  /** True once free() ran. */
  get freed() {
    return this._freed;
  }

  _requireAlive(who) {
    if (this._freed) {
      throw new Error(
        `yeptris: the schema handle is freed — ${who} on a freed schema ` +
          "is refused (compile a fresh one with compileSchema())");
    }
  }

  /** Every diagnostic across every document of `input` ([] when
   * valid). Same contract as validate(). Synchronous: the handle only
   * exists once the module is initialized. */
  validate(input) {
    this._requireAlive("validate");
    return runCompiledValidate(this._mod, this._handle, input);
  }

  /** True when validate() answers no diagnostics. */
  isValid(input) {
    return this.validate(input).length === 0;
  }

  /** Frees the handle and everything it owns in one call. Idempotent;
   * afterwards the handle is dead (reuse throws, never segfaults). */
  free() {
    if (this._freed) return;
    this._freed = true;
    this._mod._yeptris_wasm_schema_free(this._handle);
    this._handle = 0;
  }
}

// The options door: schema: "core" | "json" (the YAML 1.2 family
// walk — js-yaml's load(str, { schema }) analog) | a compiled Schema;
// jsonSchema: object | string (the JSON Schema 2020-12 walk); collect:
// load-only (load answers { document, diagnostics } instead of
// raising). Strictness law (the dump precedent): unknown options are
// rejected, never silently ignored.
function parseValidateOptions(options, who, allowCollect) {
  if (options === undefined || options === null) {
    return { door: "family", kind: 0, collect: false }; // core_12, the engine default
  }
  if (typeof options !== "object") {
    throw new TypeError(`yeptris: ${who} options must be an object`);
  }
  const KEYS = new Set(["schema", "jsonSchema", "collect"]);
  for (const key of Object.keys(options)) {
    if (!KEYS.has(key)) {
      throw new TypeError(
        `yeptris: ${who} option "${key}" is not supported ` +
          `(the surface is schema + jsonSchema${allowCollect ? " + collect" : ""})`);
    }
  }
  let collect = false;
  if (options.collect !== undefined) {
    if (typeof options.collect !== "boolean") {
      throw new TypeError(`yeptris: ${who} option collect must be a boolean`);
    }
    if (!allowCollect) {
      throw new TypeError(
        `yeptris: ${who} takes no collect option — collect rides load() ` +
          "(loadAll validates the whole stream and returns documents)");
    }
    collect = options.collect;
  }
  let schema = options.schema;
  if (schema !== undefined && schema !== null) {
    if (typeof schema === "string") {
      if (schema !== "core" && schema !== "json") {
        throw new TypeError(
          `yeptris: ${who} option schema must be "core", "json", or a ` +
            `compiled Schema (got ${JSON.stringify(schema)})`);
      }
    } else if (!(schema instanceof Schema)) {
      throw new TypeError(
        `yeptris: ${who} option schema must be "core", "json", or a ` +
          "compiled Schema (got " + typeof schema + ")");
    }
  }
  let jsonSchema = options.jsonSchema;
  if (jsonSchema !== undefined && jsonSchema !== null) {
    if (typeof jsonSchema !== "string" && typeof jsonSchema !== "object") {
      throw new TypeError(
        `yeptris: ${who} option jsonSchema must be an object or a string ` +
          "(the schema document)");
    }
    if (jsonSchema instanceof Schema) {
      throw new TypeError(
        `yeptris: ${who}: a compiled Schema rides schema:, not jsonSchema: ` +
          "(jsonSchema takes the schema document itself)");
    }
  }
  if (jsonSchema !== undefined && jsonSchema !== null && schema !== undefined && schema !== null) {
    // the two doors are alternatives, not layers: a caller passing both
    // is one edit away from the door they meant, so refuse instead of
    // silently picking one (the dump precedent)
    throw new TypeError(
      `yeptris: ${who}: pass the compiled schema as schema: OR the ` +
        "schema document as jsonSchema: — not both");
  }
  if (jsonSchema !== undefined && jsonSchema !== null) {
    const bytes = typeof jsonSchema === "string" ? jsonSchema : JSON.stringify(jsonSchema);
    return { door: "json", schemaBytes: bytes, collect, armed: true };
  }
  if (schema instanceof Schema) {
    return { door: "handle", handle: schema, collect, armed: true };
  }
  if (schema !== undefined && schema !== null) {
    const kind = schema === "json" ? 2 : 0; // YEPTRIS_SCHEMA_12_JSON / _12_CORE
    return { door: "family", kind, collect, armed: true };
  }
  if (collect) {
    throw new TypeError(
      `yeptris: ${who}: collect requires a schema — pass schema: or jsonSchema: ` +
        "(collect:true without a gate has nothing to collect)");
  }
  return { door: "family", kind: 0, collect: false, armed: false };
}

function validateWithOptions(mod, input, vo) {
  if (vo.door === "handle") return vo.handle.validate(input);
  if (vo.door === "json") {
    return runJsonSchemaValidate(mod, vo.schemaBytes, input);
  }
  return runFamilyValidate(mod, input, vo.kind);
}

/** Validate a YAML stream, answering the WHOLE report: every
 * diagnostic, in engine order ([] when valid). Each diagnostic:
 * `{ code, keyword, path, pointer, message, line, col, offset }` —
 * `path` is the JSON-pointer instance path split into ~0/~1-unescaped
 * segments ([] at the root), `pointer` its raw string form, `line`/
 * `col` the offending node's 1-based mark (0 = unknown: scalars whose
 * bytes the DOM owns have no input position; the path still
 * identifies them).
 *
 * options picks the walk (js-yaml parity names):
 *   schema: "core" (default) — explicit-tag consistency + canonical-
 *     form key uniqueness; "json" — the spec 10.2 JSON productions
 *     (every plain scalar a JSON type, string mapping keys);
 *   schema: a compiled Schema (compileSchema()) — validate against
 *     the retained model;
 *   jsonSchema: object | string — the JSON Schema draft 2020-12 walk
 *     (the engine's keyword subset; unknown keywords are ignored).
 * The doors are alternatives: passing schema AND jsonSchema throws
 * TypeError (pick the door you meant — nothing is silently dropped).
 * A stream that does not parse throws YamlError; a $ref that resolves
 * nowhere is a diagnostic, not an exception. */
export async function validate(input, options) {
  const m = await mod();
  return validateWithOptions(m, input, parseValidateOptions(options, "validate", false));
}

/** True when validate() answers no diagnostics. */
export async function isValid(input, options) {
  return (await validate(input, options)).length === 0;
}

/** Compile a JSON Schema document (object or string; JSON is YAML
 * flow, so a YAML-shaped mapping schema compiles too) into a
 * reusable Schema handle — parse ONCE, validate many. Throws
 * TypeError for empty/absent schema bytes and YamlError (with the
 * engine's message) for bytes that do not parse. */
export async function compileSchema(jsonSchema) {
  const m = await mod();
  return compileSchemaCore(m, jsonSchema);
}

function compileSchemaCore(mod, jsonSchema) {
  if (jsonSchema === undefined || jsonSchema === null) {
    throw new TypeError("yeptris: schema bytes are required");
  }
  const bytes = typeof jsonSchema === "string"
    ? jsonSchema
    : JSON.stringify(jsonSchema);
  if (bytes.length === 0) {
    throw new TypeError("yeptris: schema bytes hold no document");
  }
  const [ptr, len] = cstr(mod, bytes);
  let handle;
  try {
    handle = mod._yeptris_wasm_schema_compile(ptr, len);
  } finally {
    freeCstr(mod, ptr);
  }
  if (handle === 0) {
    throw failFrom(mod); // the engine's why rides last_error
  }
  return Schema._take(mod, handle);
}

// ---- the tolerant stream walk (parse.h's stream iterator) ---------------
//
// loadAll is all-or-nothing: one bad document fails the whole stream.
// The engine's stream iterator (leptris/yeptris#921) walks document by
// document: a parse-class failure is ONE failed item carrying the
// failed document's OWN line/column, and the walk RESUMES at the next
// line-start marker — the valid documents after a bad one are kept.
// The stream-fatal classes (memory/encoding) end the walk after their
// item; an unrecoverable tail ends it.
//
// Across the wasm boundary the walk is RETAINED state (like the
// recorder above): open copies the feed into the heap (the engine
// iterator only borrows its buffer until free), every next() advances
// one step, close frees both. The step answer crosses through the
// g_out discipline: frame() says whether the last successful step
// published a document JSON, index() says which stream position it
// consumed (failures consume an index too — the yield order is the
// stream's). Each document serializes through the same serialize_json
// door load uses, so a frame's value is load's value by construction.
//
// The JS shape is the rs face's: an iterator of result objects —
// `{ document, index }` for a valid document, `{ error, index }` for
// a failed one. The engine default (schema 1.2 core) rides as-is; the
// options door (schema/max_depth) is the engine's parse-options struct
// and joins when the face needs it.

const ST_MEMORY = 2; // YEPTRIS_ERROR_MEMORY — stream-fatal
const ST_ENCODING = 4; // YEPTRIS_ERROR_ENCODING — stream-fatal

function eachDocumentCore(mod, yaml) {
  return (function* () {
    const [ptr, len] = cstr(mod, yaml);
    try {
      const openSt = mod._yeptris_wasm_each_open(ptr, len);
      if (openSt !== 0) {
        throw failFrom(mod);
      }
      for (;;) {
        const st = mod._yeptris_wasm_each_next();
        const index = mod._yeptris_wasm_each_index();
        if (st === 0) {
          if (mod._yeptris_wasm_each_frame() === 0) {
            return; // the stream is over
          }
          const document = JSON.parse(
            mod.UTF8ToString(mod._yeptris_wasm_out(), mod._yeptris_wasm_out_len()));
          mod._yeptris_wasm_out_free();
          yield { document, index, error: undefined };
        } else {
          yield {
            document: undefined,
            index,
            error: new YamlError(
              mod.UTF8ToString(mod._yeptris_wasm_last_error()),
              mod._yeptris_wasm_last_error_line(),
              mod._yeptris_wasm_last_error_column()),
          };
          if (st === ST_MEMORY || st === ST_ENCODING) {
            return; // stream-fatal: the iterator is done
          }
        }
      }
    } finally {
      mod._yeptris_wasm_out_free(); // idempotent (frees NULL = no-op)
      mod._yeptris_wasm_each_close();
    }
  })();
}

/** Walk a multi-document stream one document at a time, TOLERANTLY
 * (the engine's stream iterator): yields one result per document in
 * stream order —
 *
 *   { document: value, index: 0, error: undefined } // a valid document
 *   { document: undefined, index: 1, error: YamlError } // a failed one
 *
 * A parse-class failure carries the failed document's OWN line/column
 * and its 0-based stream index, and the walk CONTINUES at the next
 * `---` line-start marker — the valid documents after a bad one are
 * kept (parse()/loadAll would fail the whole stream instead). The
 * stream-fatal classes (memory/encoding) yield their item and end the
 * walk; an unrecoverable tail ends it. Lazy: nothing parses until
 * iteration starts. Documents materialize through load's serialize
 * door, so each frame's value is load's value by construction. The
 * engine default (schema 1.2 core) rides as-is. */
export async function* eachDocument(yaml) {
  const m = await mod();
  yield* eachDocumentCore(m, yaml);
}

/** eachDocument's Sync twin: a sync generator over an initialized
 * module (await init() first). */
export function eachDocumentSync(yaml) {
  return eachDocumentCore(requireMod("eachDocumentSync"), yaml);
}

// ---- load/dump cores (shared by the async and Sync forms) --------------

function loadCore(mod, yaml, options) {
  const vo = parseValidateOptions(options, "load", true);
  if (!vo.armed) {
    return JSON.parse(run(mod, mod._yeptris_wasm_load, yaml));
  }
  // validate BEFORE loading (the ruby/py law): the document only
  // materializes when the walk is clean — or when collect asked for
  // both, in which case the pair answers even for a dirty walk
  const diags = validateWithOptions(mod, yaml, vo);
  if (vo.collect) {
    return { document: JSON.parse(run(mod, mod._yeptris_wasm_load, yaml)), diagnostics: diags };
  }
  if (diags.length > 0) {
    throw new ValidationError(diags);
  }
  return JSON.parse(run(mod, mod._yeptris_wasm_load, yaml));
}

function loadAllCore(mod, yaml, options) {
  const vo = parseValidateOptions(options, "loadAll", false);
  if (vo.armed) {
    // whole-stream validation BEFORE the parse (the ruby/py law): one
    // walk over every document, then the materialization
    const diags = validateWithOptions(mod, yaml, vo);
    if (diags.length > 0) {
      throw new ValidationError(diags);
    }
  }
  return JSON.parse(run(mod, mod._yeptris_wasm_load_all, yaml));
}

function dumpCore(mod, json, options) {
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
  return run(mod, mod._yeptris_wasm_dump_ex, text, canonical, lineWidth);
}

// ---- the event face (the recorder's bulk drain) ------------------------
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

function eventsCore(mod, yaml, options) {
  const [ptr, len] = cstr(mod, yaml);
  try {
    const st = mod._yeptris_wasm_events(ptr, len);
    if (st !== 0) {
      throw new YamlError(
        mod.UTF8ToString(mod._yeptris_wasm_last_error()),
        mod._yeptris_wasm_last_error_line(),
        mod._yeptris_wasm_last_error_column());
    }
    const n = mod._yeptris_wasm_event_count();
    const recs = mod._yeptris_wasm_event_records();
    const arena = mod._yeptris_wasm_event_arena();
    // Heap views fetched AFTER the call: the feed may have grown the
    // memory, and the exported views are re-bound on growth.
    const u8 = mod.HEAPU8;
    const u32 = mod.HEAPU32;
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
    mod._yeptris_wasm_events_free();
  }
}

/** Parse YAML text and return the document as a JavaScript value
 * (the engine's canonical JSON, JSON.parse'd). Throws YamlError with
 * line/column on malformed input. A multi-document stream answers
 * with the FIRST document (the engine's json contract); every
 * document of a stream comes from loadAll.
 *
 * options arms a validation gate (the ruby/py load law: validate
 * BEFORE loading) — schema: "core" | "json" | a compiled Schema,
 * jsonSchema: object | string. With diagnostics present the load
 * raises ValidationError carrying the whole report; with `collect:
 * true` it answers `{ document, diagnostics }` instead (collect
 * requires an armed gate). */
export async function load(yaml, options) {
  const m = await mod();
  return loadCore(m, yaml, options);
}

/** Parse a multi-document YAML stream and return EVERY document as a
 * JavaScript value, in stream order (js-yaml's loadAll shape). An
 * empty or comment-only stream answers []; an empty document
 * ("---\n---") answers null at its position. options arms a
 * whole-stream validation gate (schema:/jsonSchema:) that runs
 * BEFORE the parse and raises ValidationError carrying the whole
 * report (collect rides load() only). Throws YamlError with
 * line/column on malformed input. */
export async function loadAll(yaml, options) {
  const m = await mod();
  return loadAllCore(m, yaml, options);
}

/** Parse YAML text and return the engine's canonical JSON string. */
export async function loadJSON(yaml) {
  const m = await mod();
  return run(m, m._yeptris_wasm_load, yaml);
}

// ---- the async entries (initialize on demand) ---------------------------

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
  return eventsCore(m, yaml, options);
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
  return dumpCore(m, json, options);
}

/** The libyeptris version string. */
export async function version() {
  const m = await mod();
  return m.UTF8ToString(m._yeptris_wasm_version());
}

// ---- the Sync entries (usable once the module is initialized) ----------
//
// The wasm executes synchronously; every async form is async only
// because the FIRST call may initialize the module. After one await
// (init(), or any async call) the same bodies run synchronously. Each
// Sync twin refuses to run uninitialized — the lazy import is an
// async door and half-initializing it would be a lie — and names the
// way out in the error.

/** Parse YAML text (load's Sync twin). Throws unless the module is
 * initialized (await init() first). */
export function loadSync(yaml, options) {
  return loadCore(requireMod("loadSync"), yaml, options);
}

/** loadAll's Sync twin. Throws unless the module is initialized. */
export function loadAllSync(yaml, options) {
  return loadAllCore(requireMod("loadAllSync"), yaml, options);
}

/** loadJSON's Sync twin. Throws unless the module is initialized. */
export function loadJSONSync(yaml) {
  return run(requireMod("loadJSONSync"), cached._yeptris_wasm_load, yaml);
}

/** events' Sync twin. Throws unless the module is initialized. */
export function eventsSync(yaml, options) {
  return eventsCore(requireMod("eventsSync"), yaml, options);
}

/** dump's Sync twin. Throws unless the module is initialized. */
export function dumpSync(json, options) {
  return dumpCore(requireMod("dumpSync"), json, options);
}

/** validate's Sync twin. Throws unless the module is initialized. */
export function validateSync(input, options) {
  return validateWithOptions(
    requireMod("validateSync"), input, parseValidateOptions(options, "validate", false));
}

/** isValid's Sync twin. Throws unless the module is initialized. */
export function isValidSync(input, options) {
  return validateSync(input, options).length === 0;
}

/** compileSchema's Sync twin. Throws unless the module is initialized. */
export function compileSchemaSync(jsonSchema) {
  return compileSchemaCore(requireMod("compileSchemaSync"), jsonSchema);
}

/** version's Sync twin. Throws unless the module is initialized. */
export function versionSync() {
  const m = requireMod("versionSync");
  return m.UTF8ToString(m._yeptris_wasm_version());
}

/** The wasm module instance this package caches (diagnostics:
 * heap/memory inspection, the engine's raw exports). The load/dump
 * calls all share this one instance. */
export async function wasmModule() {
  return mod();
}
