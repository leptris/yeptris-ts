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
    "_yeptris_wasm_plan_compile",
    "_yeptris_wasm_plan_column_count",
    "_yeptris_wasm_plan_free",
    "_yeptris_wasm_plan_walk",
    "_yeptris_wasm_plan_seq",
    "_yeptris_wasm_plan_rows",
    "_yeptris_wasm_plan_kind",
    "_yeptris_wasm_plan_ints",
    "_yeptris_wasm_plan_floats",
    "_yeptris_wasm_plan_strs",
    "_yeptris_wasm_plan_nulls",
    "_yeptris_wasm_plan_walk_free",
    "_yeptris_wasm_tape_parse",
    "_yeptris_wasm_tape_seq",
    "_yeptris_wasm_tape_count",
    "_yeptris_wasm_tape_columns",
    "_yeptris_wasm_tape_kinds",
    "_yeptris_wasm_tape_offs",
    "_yeptris_wasm_tape_lens",
    "_yeptris_wasm_tape_convert",
    "_yeptris_wasm_tape_free",
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
// a failed one. The engine default (schema 1.2 core) rides as-is;
// the options door (schema/max_depth — ruby/py parity) fills the
// engine's parse-options struct through the shim's open params.

const ST_MEMORY = 2; // YEPTRIS_ERROR_MEMORY — stream-fatal
const ST_ENCODING = 4; // YEPTRIS_ERROR_ENCODING — stream-fatal

// eachDocument's options door (the engine's YeptrisParseOptions, the
// ruby `each_document(yaml, schema:, max_depth:)` / py
// `each_document(yaml, schema=, max_depth=)` parity): `schema` is the
// resolver family every frame types its plain scalars through (the
// parse resolver's kind, spelled the js-yaml parity way this face
// already uses for validate/load — "core" the 1.2 core default,
// "json" the spec 10.2 JSON resolver), `max_depth` caps per-frame
// nesting (0 = the library default; a frame past the cap fails as
// that frame's error item and the walk continues at the next marker).
// The compiled/JSON-Schema doors have no seat on the walk — the
// iterator carries a resolver KIND, not a schema document; those
// doors ride validate/load. Strictness law (the dump precedent):
// unknown or mistyped keys are rejected, never silently ignored.
// With no options the shim keeps its NULL ride — the engine defaults,
// the face's shipped v1 contract — byte-identical to the pre-door era.
function parseEachOptions(options, who) {
  if (options === undefined || options === null) {
    return { kind: -1, maxDepth: 0 }; // -1: the shim's NULL-opts ride
  }
  if (typeof options !== "object") {
    throw new TypeError(`yeptris: ${who} options must be an object`);
  }
  const KEYS = new Set(["schema", "max_depth"]);
  for (const key of Object.keys(options)) {
    if (!KEYS.has(key)) {
      throw new TypeError(
        `yeptris: ${who} option "${key}" is not supported ` +
          "(the surface is schema + max_depth)");
    }
  }
  let kind = 0; // an options object arms the door: the 1.2 core family
  if (options.schema !== undefined && options.schema !== null) {
    if (options.schema instanceof Schema) {
      throw new TypeError(
        `yeptris: ${who}: a compiled Schema rides validate()/load(), ` +
          "not the walk — the iterator takes the resolver kind " +
          '(schema: "core" | "json")');
    }
    if (typeof options.schema !== "string" ||
        (options.schema !== "core" && options.schema !== "json")) {
      throw new TypeError(
        `yeptris: ${who} option schema must be "core" or "json" (got ` +
          JSON.stringify(options.schema) + ")");
    }
    kind = options.schema === "json" ? 2 : 0; // YEPTRIS_SCHEMA_12_JSON / _12_CORE
  }
  let maxDepth = 0;
  if (options.max_depth !== undefined && options.max_depth !== null) {
    const d = options.max_depth;
    if (typeof d !== "number" || !Number.isInteger(d) || d < 0) {
      throw new TypeError(
        `yeptris: ${who} option max_depth must be a non-negative integer ` +
          "(0 rides the library default)");
    }
    maxDepth = d;
  }
  return { kind, maxDepth };
}

function eachDocumentCore(mod, yaml, vo) {
  return (function* () {
    const [ptr, len] = cstr(mod, yaml);
    try {
      // The capability probe: the door rides a NEW export (the v1
      // open's 2-param wasm ABI stays frozen — this surface's ABI is
      // append-only), so a dist built before the door simply lacks
      // it. The probe is the missing-export check's own idiom, and it
      // gates the DOOR ONLY: the no-options walk is the v1 contract
      // and rides either era unchanged. Without the probe wasm would
      // never even see the call — the export isn't there to call —
      // and the options would have no seat at all; with it, a stale
      // dist is refused loudly instead of pretending.
      let openSt;
      if (vo.kind >= 0) {
        const openEx = mod._yeptris_wasm_each_open_ex;
        if (typeof openEx !== "function") {
          throw new Error(
            "yeptris: the eachDocument options door needs a dist rebuilt " +
              "with build.sh — the loaded wasm predates the options era " +
              "(stale artifact)",
          );
        }
        openSt = openEx(ptr, len, vo.kind, vo.maxDepth);
      } else {
        openSt = mod._yeptris_wasm_each_open(ptr, len);
      }
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
 * door, so each frame's value is load's value by construction.
 *
 * options (the engine's parse options — ruby/py parity): schema —
 * the resolver family every frame types through, "core" (default) or
 * "json"; max_depth — the per-frame nesting cap, 0 = the library
 * default (a frame past the cap fails as that frame's item and the
 * walk continues). Unknown or mistyped options throw TypeError. */
export async function* eachDocument(yaml, options) {
  const m = await mod();
  yield* eachDocumentCore(m, yaml, parseEachOptions(options, "eachDocument"));
}

/** eachDocument's Sync twin: a sync generator over an initialized
 * module (await init() first). Takes the same options door. */
export function eachDocumentSync(yaml, options) {
  return eachDocumentCore(
    requireMod("eachDocumentSync"),
    yaml,
    parseEachOptions(options, "eachDocumentSync"),
  );
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

// ---- the plan face (plan.h; the compiled columnar walk) ----------------
//
// The query surface, the rs face's shape (yeptris-rs 0.6.78, `pub mod
// plan`): a plan compiles ONCE from a strict-JSON spec
//
//   {"kind":"seq","path":"items","children":[
//      {"name":"id","kind":"int"}, {"name":"ok","kind":"bool"}, ...]}
//
// (kind: "seq" | "map" — the rows container's shape at the root; path:
// the root mapping's key holding the rows, a string or an ARRAY of
// strings for a segmented path walked mapping by mapping; leaf kinds:
// int, float, str, bool) and applies to a parsed document in ONE C
// pass, answering typed columns. Across the wasm boundary the walk is
// RETAINED state, the events-face lifecycle: the result BORROWS the
// document (its string columns are (ptr,len) views into the
// document's regions) and the document borrows its input — walk()
// retains all three until the host has copied its answers out, then
// free() releases them (a newer walk releases an undrained one
// defensively). Every accessor COPIES from the heap; nothing
// engine-owned survives free().

// YEP_PLAN_* (plan.h) -> the leaf-kind name.
const PLAN_KINDS = {
  0: "int",
  1: "float",
  2: "str",
  3: "bool",
};

/** The columnar result of one plan walk. Produced by Plan.walk(); a
 * shim-retained answer the accessors copy from — free() releases the
 * engine state. After free() reuse throws; so does a STALE result
 * (a newer walk() replaced the retained state) — never a silent wrong
 * answer. */
export class PlanResult {
  /** @private — produced by Plan.walk() only */
  constructor() {
    throw new TypeError(
      "yeptris: walk results come from plan.walk(), not new PlanResult()");
  }

  static _take(mod, seq) {
    const r = Object.create(PlanResult.prototype);
    r._mod = mod;
    r._seq = seq; // the shim's retained-walk generation this answer is
    r._freed = false;
    return r;
  }

  /** True once free() ran. */
  get freed() {
    return this._freed;
  }

  _requireLive(who) {
    if (this._freed) {
      throw new Error(
        `yeptris: the walk result is freed — ${who} after free() is ` +
          "refused (walk again for a fresh result)");
    }
    if (this._mod._yeptris_wasm_plan_seq() !== this._seq) {
      throw new Error(
        "yeptris: stale walk result — a newer walk() replaced the " +
          "retained state; read this result's columns before walking again");
    }
  }

  /** The number of rows (one per row of the rows container). */
  get rows() {
    this._requireLive("rows");
    return this._mod._yeptris_wasm_plan_rows();
  }

  /** The leaf kind of column `col`: "int" | "float" | "str" | "bool". */
  kind(col) {
    this._requireLive("kind");
    const k = this._mod._yeptris_wasm_plan_kind(col);
    return PLAN_KINDS[k] ?? `unknown-${k}`;
  }

  /** INT column values as JS numbers. BOOL columns read through here
   * too, as 0/1 — the engine's columnar contract (the rs face's
   * `ints()`). Exact within +/-2^53 (the load()'s JSON round-trip
   * law); the engine's int64 wider than that rounds, per JS number. */
  ints(col) {
    this._requireLive("ints");
    const k = this._mod._yeptris_wasm_plan_kind(col);
    if (k !== 0 && k !== 3) {
      throw new TypeError(
        `yeptris: ints() reads int and bool columns — column ${col} is ` +
          `${PLAN_KINDS[k] ?? `unknown-${k}`}`);
    }
    const rows = this._mod._yeptris_wasm_plan_rows();
    const p = this._mod._yeptris_wasm_plan_ints(col);
    if (!p || rows === 0) {
      return [];
    }
    // views fetched AFTER the calls (the memory may have grown); the
    // int64 lane composes from the u32 pair — wasm64 views are not a
    // given across the emcc eras this surface builds on
    const u32 = this._mod.HEAPU32;
    const base = p >> 2;
    const out = new Array(rows);
    for (let i = 0; i < rows; i++) {
      const hi = u32[base + i * 2 + 1];
      let v = hi * 4294967296 + u32[base + i * 2];
      if (hi >= 2147483648) {
        v -= 18446744073709551616;
      }
      out[i] = v;
    }
    return out;
  }

  /** FLOAT column values as JS numbers (the f64 lane, copied). */
  floats(col) {
    this._requireLive("floats");
    const k = this._mod._yeptris_wasm_plan_kind(col);
    if (k !== 1) {
      throw new TypeError(
        `yeptris: floats() reads float columns — column ${col} is ` +
          `${PLAN_KINDS[k] ?? `unknown-${k}`}`);
    }
    const rows = this._mod._yeptris_wasm_plan_rows();
    const p = this._mod._yeptris_wasm_plan_floats(col);
    if (!p || rows === 0) {
      return [];
    }
    const f64 = this._mod.HEAPF64;
    const base = p >> 3;
    return Array.from(f64.subarray(base, base + rows));
  }

  /** STR column values as JS strings (the DOM leg's (ptr,len) views,
   * decoded and copied out of the heap). */
  strs(col) {
    this._requireLive("strs");
    const k = this._mod._yeptris_wasm_plan_kind(col);
    if (k !== 2) {
      throw new TypeError(
        `yeptris: strs() reads str columns — column ${col} is ` +
          `${PLAN_KINDS[k] ?? `unknown-${k}`}`);
    }
    const rows = this._mod._yeptris_wasm_plan_rows();
    const p = this._mod._yeptris_wasm_plan_strs(col);
    if (!p || rows === 0) {
      return [];
    }
    // records of yeptris_plan_str { const char* p; size_t len; } —
    // 8 bytes on wasm32: one u32 pair per row
    const u8 = this._mod.HEAPU8;
    const u32 = this._mod.HEAPU32;
    const base = p >> 2;
    const out = new Array(rows);
    for (let i = 0; i < rows; i++) {
      const sp = u32[base + i * 2];
      const sl = u32[base + i * 2 + 1];
      out[i] = sl !== 0 ? decodeArena(u8, sp, sl) : "";
    }
    return out;
  }

  /** The column's null markers: 1 = the row's leaf was null or
   * missing. A Uint8Array copy of `rows` entries. */
  nulls(col) {
    this._requireLive("nulls");
    const rows = this._mod._yeptris_wasm_plan_rows();
    const p = this._mod._yeptris_wasm_plan_nulls(col);
    if (!p || rows === 0) {
      return new Uint8Array(0);
    }
    // slice COPIES: a subarray would view the heap that free() releases
    return this._mod.HEAPU8.slice(p, p + rows);
  }

  /** Releases the retained walk (result + document + input copy).
   * Idempotent. A STALE result only dies locally — the retained state
   * belongs to the walk that replaced it. */
  free() {
    if (this._freed) return;
    this._freed = true;
    if (this._mod._yeptris_wasm_plan_seq() === this._seq) {
      this._mod._yeptris_wasm_plan_walk_free();
    }
  }
}

/** A COMPILED plan (plan.h): the query spec parses once — its columns
 * and segmented path retained behind one opaque handle — and walks
 * MANY documents. Build with compilePlan(); direct construction is
 * refused. After free() the handle is dead: reuse throws. */
export class Plan {
  /** @private — produced by compilePlan() only */
  constructor() {
    throw new TypeError(
      "yeptris: build plans with compilePlan(), not new Plan()");
  }

  static _take(mod, handle) {
    const s = Object.create(Plan.prototype);
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
        `yeptris: the plan handle is freed — ${who} on a freed plan is ` +
          "refused (compile a fresh one with compilePlan())");
    }
  }

  /** The number of leaf columns the spec declared. */
  get columnCount() {
    this._requireAlive("columnCount");
    return this._mod._yeptris_wasm_plan_column_count(this._handle);
  }

  /** Apply the plan to a YAML document (the DOM leg): typed columns,
   * one C pass, no per-node host dispatch. Synchronous — the handle
   * only exists once the module is initialized. Throws YamlError when
   * the document does not parse or its shape disagrees with the plan
   * (the rows container the path names is not there). */
  walk(yaml) {
    this._requireAlive("walk");
    return planWalkCore(this._mod, this, yaml);
  }

  /** Frees the handle. Idempotent; afterwards reuse throws. */
  free() {
    if (this._freed) return;
    this._freed = true;
    this._mod._yeptris_wasm_plan_free(this._handle);
    this._handle = 0;
  }
}

function planWalkCore(mod, plan, yaml) {
  const [ptr, len] = cstr(mod, yaml);
  try {
    const st = mod._yeptris_wasm_plan_walk(plan._handle, ptr, len);
    if (st !== 0) {
      throw new YamlError(
        mod.UTF8ToString(mod._yeptris_wasm_last_error()),
        mod._yeptris_wasm_last_error_line(),
        mod._yeptris_wasm_last_error_column());
    }
    return PlanResult._take(mod, mod._yeptris_wasm_plan_seq());
  } finally {
    freeCstr(mod, ptr);
  }
}

function compilePlanCore(mod, spec) {
  if (spec === undefined || spec === null) {
    throw new TypeError("yeptris: plan spec bytes are required");
  }
  const bytes = typeof spec === "string" ? spec : JSON.stringify(spec);
  if (bytes.length === 0) {
    throw new TypeError("yeptris: plan spec holds no document");
  }
  const [ptr, len] = cstr(mod, bytes);
  let handle;
  try {
    handle = mod._yeptris_wasm_plan_compile(ptr, len);
  } finally {
    freeCstr(mod, ptr);
  }
  if (handle === 0) {
    throw failFrom(mod); // the engine's why rides last_error
  }
  return Plan._take(mod, handle);
}

/** Compile a plan from a strict-JSON spec document (object or string;
 * the shape plan.h spells: kind "seq"|"map", path string|string[],
 * children of { name, kind: int|float|str|bool }). Throws TypeError
 * for empty/absent spec bytes and YamlError (with the engine's
 * message) for bytes that do not parse or name no engine kind. */
export async function compilePlan(spec) {
  const m = await mod();
  return compilePlanCore(m, spec);
}

// ---- the tape face (tape.h; the columnar JSON parse) -------------------
//
// The simdjson-class materialization: one fused strict walk appends
// ONE record per token — no DOM nodes, no links, no views. Records
// are three parallel columns (kind byte, off, len); bools encode in
// the kind, container links ride the off column (matching
// open/close), numbers record spans only, and strings stay zero-copy
// spans into the caller's buffer — which is why the tape is RETAINED
// state across the wasm boundary (the events-face lifecycle again):
// parse() copies the input and retains tape + copy, the accessors
// copy their answers out, free() releases both. The record kinds are
// ABI-pinned (tape.h, appending-only) and exported below as data.

/** The tape record kinds (tape.h's YEP_T_*, ABI-pinned). `kinds` in
 * columns() answers these raw bytes; the names spell the vocabulary. */
export const TAPE_KINDS = Object.freeze({
  doc: 0, // document boundary (records start after it)
  null: 1,
  true: 2, // the kind IS the value
  false: 3,
  int: 4, // off/len: signed digit span; convert materializes
  float: 5, // off/len: float text span
  str: 6, // off/len: raw span into the source (escapes untouched)
  "seq-open": 7, // off: index of the matching close
  "map-open": 8, // off: index of the matching close
  close: 9, // off: index of the matching open
  num: 10, // lenient route only: a number span the grammar has NOT
  // validated at parse — convert is the authority
});

/** A parsed JSON tape (tape.h): the record stream of one strict
 * parse, retained behind the module. Build with parseJsonTape();
 * direct construction is refused. After free() reuse throws; a newer
 * parse() makes older tape objects stale — they refuse instead of
 * answering the newer records. */
export class JsonTape {
  /** @private — produced by parseJsonTape() only */
  constructor() {
    throw new TypeError(
      "yeptris: build tapes with parseJsonTape(), not new JsonTape()");
  }

  static _take(mod, seq) {
    const t = Object.create(JsonTape.prototype);
    t._mod = mod;
    t._seq = seq; // the shim's retained-tape generation this answer is
    t._freed = false;
    return t;
  }

  /** True once free() ran. */
  get freed() {
    return this._freed;
  }

  _requireLive(who) {
    if (this._freed) {
      throw new Error(
        `yeptris: the tape is freed — ${who} after free() is refused ` +
          "(parse again for a fresh tape)");
    }
    if (this._mod._yeptris_wasm_tape_seq() !== this._seq) {
      throw new Error(
        "yeptris: stale tape — a newer parseJsonTape() replaced the " +
          "retained state; read this tape's records before parsing again");
    }
  }

  /** The number of records (one per token, the document boundary
   * record included). */
  get count() {
    this._requireLive("count");
    return this._mod._yeptris_wasm_tape_count();
  }

  /** The three parallel columns, materialized (the shim runs
   * yeptris_tape_columns first — a no-op when already materialized)
   * and COPIED out of the heap: `{ kinds: Uint8Array, offs:
   * Uint32Array, lens: Uint32Array }`, one entry per record. `kinds`
   * answers TAPE_KINDS bytes; `offs`/`lens` are span starts and
   * lengths into the SOURCE for str/int/float records and container
   * links for open/close records. */
  columns() {
    this._requireLive("columns");
    const st = this._mod._yeptris_wasm_tape_columns();
    if (st !== 0) {
      throw new Error(
        `yeptris: tape column materialization failed (status ${st})`);
    }
    const count = this._mod._yeptris_wasm_tape_count();
    // heap views fetched AFTER the calls (the memory may have grown);
    // slice COPIES — the tape frees under these
    const u8 = this._mod.HEAPU8;
    const u32 = this._mod.HEAPU32;
    const kinds = this._mod._yeptris_wasm_tape_kinds();
    const offs = this._mod._yeptris_wasm_tape_offs();
    const lens = this._mod._yeptris_wasm_tape_lens();
    return {
      kinds: kinds !== 0 ? u8.slice(kinds, kinds + count) : new Uint8Array(0),
      offs: offs !== 0
        ? u32.slice(offs >> 2, (offs >> 2) + count)
        : new Uint32Array(0),
      lens: lens !== 0
        ? u32.slice(lens >> 2, (lens >> 2) + count)
        : new Uint32Array(0),
    };
  }

  /** Convert the number records in `[from, to)` (record indices; the
   * defaults convert every number record): answers
   * `{ ints: number[], floats: number[] }` — DENSE, in tape order:
   * the INT records' converted values and the FLOAT records' (a
   * lenient route's `num` record converts as a float — convert is the
   * grammar's authority there). Exact within +/-2^53 (the load()'s
   * JSON round-trip law). */
  convert(from = 0, to) {
    this._requireLive("convert");
    const count = this._mod._yeptris_wasm_tape_count();
    const end = to === undefined || to === null ? count : to;
    const reqIndex = (v, who) => {
      if (typeof v !== "number" || !Number.isInteger(v) || v < 0) {
        throw new TypeError(
          `yeptris: convert ${who} must be a non-negative integer ` +
            "(a record index)");
      }
    };
    reqIndex(from, "from");
    if (to !== undefined && to !== null) {
      reqIndex(to, "to");
    }
    if (from > end) {
      throw new TypeError("yeptris: convert from exceeds to");
    }
    if (end > count) {
      throw new TypeError(
        `yeptris: convert to ${end} exceeds the tape's ${count} records`);
    }
    const cols = this.columns(); // the kinds view drives the dense answer
    const bufN = count > 0 ? count : 1;
    const ivals = this._mod._malloc(bufN * 8);
    const dvals = this._mod._malloc(bufN * 8);
    try {
      // record-index-parallel slots, zeroed: the engine writes only
      // the number records' slots, the untouched ones read 0
      new Uint8Array(this._mod.HEAPU8.buffer, ivals, bufN * 8).fill(0);
      new Uint8Array(this._mod.HEAPU8.buffer, dvals, bufN * 8).fill(0);
      const n = this._mod._yeptris_wasm_tape_convert(from, end, ivals, dvals);
      if (n === -1) {
        throw new Error(
          "yeptris: a number span refused to convert (not a tape this " +
            "library produced)");
      }
      // views fetched AFTER the call: the mallocs may have grown the
      // memory, and the exported views re-bind on growth
      const u32 = this._mod.HEAPU32;
      const f64 = this._mod.HEAPF64;
      const ibase = ivals >> 2;
      const dbase = dvals >> 3;
      const ints = [];
      const floats = [];
      for (let i = from; i < end; i++) {
        const kind = cols.kinds[i];
        if (kind === TAPE_KINDS.int) {
          const hi = u32[ibase + i * 2 + 1];
          let v = hi * 4294967296 + u32[ibase + i * 2];
          if (hi >= 2147483648) {
            v -= 18446744073709551616;
          }
          ints.push(v);
        } else if (kind === TAPE_KINDS.float || kind === TAPE_KINDS.num) {
          floats.push(f64[dbase + i]);
        }
      }
      return { ints, floats };
    } finally {
      this._mod._yeptris_wasm_free(ivals);
      this._mod._yeptris_wasm_free(dvals);
    }
  }

  /** Releases the retained tape and its input copy. Idempotent;
   * afterwards reuse throws. */
  free() {
    if (this._freed) return;
    this._freed = true;
    this._mod._yeptris_wasm_tape_free();
  }
}

function parseJsonTapeCore(mod, json) {
  const [ptr, len] = cstr(mod, json);
  try {
    const st = mod._yeptris_wasm_tape_parse(ptr, len);
    if (st !== 0) {
      throw new YamlError(
        mod.UTF8ToString(mod._yeptris_wasm_last_error()),
        mod._yeptris_wasm_last_error_line(),
        mod._yeptris_wasm_last_error_column());
    }
    return JsonTape._take(mod, mod._yeptris_wasm_tape_seq());
  } finally {
    freeCstr(mod, ptr);
  }
}

/** Parse strict JSON into the columnar tape (tape.h): one record per
 * token, no DOM — the simdjson-class materialization. Scalar roots
 * ride (a single record after the boundary). Throws YamlError with
 * line/column on malformed input — the strict route validates the
 * number grammar at parse, so a malformed span never reaches the
 * tape (the lenient route's deferred numbers are an engine door this
 * face does not open; convert remains the authority for `num`
 * records). */
export async function parseJsonTape(json) {
  const m = await mod();
  return parseJsonTapeCore(m, json);
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

/** compilePlan's Sync twin. Throws unless the module is initialized. */
export function compilePlanSync(spec) {
  return compilePlanCore(requireMod("compilePlanSync"), spec);
}

/** parseJsonTape's Sync twin. Throws unless the module is initialized. */
export function parseJsonTapeSync(json) {
  return parseJsonTapeCore(requireMod("parseJsonTapeSync"), json);
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
