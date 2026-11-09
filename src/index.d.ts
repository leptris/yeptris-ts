// Type declarations for yeptris (the wasm binding).
export declare class YamlError extends Error {
  line: number;
  column: number;
}

/** Raised by load/loadAll when the input PARSES but the armed schema
 * walk produced diagnostics (validate BEFORE loading). Carries the
 * whole report as `diagnostics` — never first-fail. Not a YamlError:
 * the input is well-formed. */
export declare class ValidationError extends Error {
  diagnostics: Diagnostic[];
}

/** One structured validation miss. `path` is the JSON-pointer
 * instance path split into ~0/~1-unescaped segments ([] at the
 * document root); `pointer` is its raw string form; `message` is the
 * engine's bounded detail; `line`/`col` are the offending node's
 * 1-based mark (0 = unknown: scalars whose bytes the DOM owns have no
 * input position — the path still identifies them). */
export interface Diagnostic {
  /** The engine's YeptrisDiagCode (validate.h, ABI-pinned,
   * appending-only). */
  code: number;
  /** The spec-spelled keyword: the violated JSON Schema keyword for
   * the 2020-12 walk ("type", "minimum", "required", "$ref", ...), the
   * violated YAML rule for the family walk ("tag", "keyType",
   * "jsonType", "duplicateKey"). */
  keyword: string;
  /** The instance path as segments, ~0/~1 unescaped; [] at the root. */
  path: string[];
  /** The raw JSON-pointer form of `path` ("" at the root). */
  pointer: string;
  /** The engine's bounded explanation ("3 is less than the minimum
   * 10"). */
  message: string;
  /** 1-based line of the offending node (0 = unknown). */
  line: number;
  /** 1-based column of the offending node (0 = unknown). */
  col: number;
  /** The offending node's byte offset in the input. */
  offset: number;
}

/** Options that arm a validation gate. The doors are alternatives —
 * passing schema AND jsonSchema throws TypeError. */
export interface ValidateOptions {
  /** The schema-FAMILY walk: "core" (default) — explicit-tag
   *  consistency + canonical-form key uniqueness (spec 3.2.1.3);
   *  "json" — the spec 10.2 JSON productions (every plain scalar a
   *  JSON type, string mapping keys). A compiled Schema (from
   *  compileSchema()) selects the compiled door instead. */
  schema?: "core" | "json" | Schema;
  /** The JSON Schema draft 2020-12 walk: the schema document itself
   *  (object — serialized for the wire — or string). The engine's
   *  keyword subset applies; unknown keywords are ignored, per
   *  2020-12. */
  jsonSchema?: object | string;
}

/** load's collect posture: the document plus the whole report
 * (requires an armed gate — schema: or jsonSchema:). */
export interface LoadCollectResult {
  document: unknown;
  diagnostics: Diagnostic[];
}

/** A COMPILED JSON Schema: the schema parses once (its DOM and
 * pattern programs retained behind one opaque handle) and validates
 * many instance streams. Build with compileSchema(); after free()
 * reuse throws, double-free is a no-op. */
export declare class Schema {
  private constructor();
  /** Every diagnostic across every document of `input` ([] when
   *  valid). Synchronous — the handle only exists once the module is
   *  initialized. Throws after free(). */
  validate(input: string): Diagnostic[];
  /** True when validate() answers no diagnostics. */
  isValid(input: string): boolean;
  /** True once free() ran. */
  get freed(): boolean;
  /** Frees the handle and everything it owns in one call. Idempotent;
   *  afterwards the handle is dead (reuse throws, never crashes). */
  free(): void;
}

/** Compile a JSON Schema document (object or string; JSON is YAML
 * flow, so a YAML-shaped mapping schema compiles too) into a reusable
 * Schema handle — parse ONCE, validate many. Throws TypeError for
 * empty/absent schema bytes and YamlError (with the engine's message)
 * for bytes that do not parse. */
export declare function compileSchema(jsonSchema: object | string): Promise<Schema>;

/** Validate a YAML stream, answering the WHOLE report: every
 * diagnostic, in engine order ([] when valid). Without options the
 * core-family walk runs. Throws YamlError when the stream does not
 * parse. */
export declare function validate(
  input: string,
  options?: ValidateOptions,
): Promise<Diagnostic[]>;

/** True when validate() answers no diagnostics. */
export declare function isValid(
  input: string,
  options?: ValidateOptions,
): Promise<boolean>;

/** Parse YAML text and return the document as a JavaScript value
 * (the engine's canonical JSON, JSON.parse'd). Throws YamlError with
 * line/column on malformed input. A multi-document stream answers
 * with the FIRST document; every document comes from loadAll.
 * options arms a validation gate (validate BEFORE loading): with
 * diagnostics the load raises ValidationError; with `collect: true`
 * it answers a LoadCollectResult instead. */
export declare function load(
  yaml: string,
  options?: ValidateOptions & { collect?: boolean },
): Promise<unknown>;

/** Parse a multi-document YAML stream and return EVERY document as a
 * JavaScript value, in stream order. An empty or comment-only stream
 * answers []. options arms a whole-stream validation gate that runs
 * BEFORE the parse and raises ValidationError carrying the whole
 * report (collect rides load() only). Throws YamlError with
 * line/column on malformed input. */
export declare function loadAll(
  yaml: string,
  options?: ValidateOptions,
): Promise<unknown[]>;

/** Parse YAML text and return the engine's canonical JSON string. */
export declare function loadJSON(yaml: string): Promise<string>;

/** One normalized parse event (the recorder's bulk drain). Absent
 * properties are omitted, never null. */
export interface YamlEvent {
  /** stream-start | stream-end | document-start | document-end |
   *  sequence-start | sequence-end | mapping-start | mapping-end |
   *  scalar | alias */
  type: string;
  /** 1-based start mark (0 = the event carries no position, the empty
   *  document's null scalar) */
  line: number;
  column: number;
  /** libyaml end_mark semantics: 1-based, column one past the end */
  endLine: number;
  endColumn: number;
  /** scalar content; the alias NAME for type "alias" */
  value?: string;
  /** the node's &anchor, present when written */
  anchor?: string;
  /** the node's tag: the resolved core URI (!!str) or the local form
   *  (!custom), present when written */
  tag?: string;
  /** scalars: plain | single | double | literal | folded (absent when
   *  the parser recorded no style — the empty document's null scalar) */
  style?: string;
  /** scalars: the resolved kind (str|int|float|bool|null|timestamp|
   *  seq|map|binary|merge|value|custom) — the typing SSOT, so hosts
   *  never re-scan */
  tagId?: string;
  /** scalars: plain style (the resolver's implicit-typing trigger) */
  implicit?: boolean;
  /** collection starts: flow context */
  flow?: boolean;
  /** document start/end: a --- / ... marker was seen */
  explicit?: boolean;
}

/** Parse YAML text and return EVERY event of the stream, in stream
 * order (the recorder's bulk drain: one wasm crossing per stream, no
 * per-event marshalling). Document boundaries ride the same stream —
 * the document-start/-end events are the loadAll boundaries. options
 * is accepted for call-site symmetry and reserved. Throws YamlError
 * on malformed input. */
export declare function events(
  yaml: string,
  options?: Record<string, unknown>,
): Promise<YamlEvent[]>;

/** One step of the tolerant stream walk (eachDocument): a valid
 * document or a failed one, in stream order. The two shapes are
 * exclusive — `document` is present (and `error` undefined) exactly
 * when the frame parsed. */
export type EachDocumentResult =
  | { document: unknown; index: number; error: undefined }
  | { document: undefined; index: number; error: YamlError };

/** eachDocument's options door (the engine's YeptrisParseOptions —
 * the ruby `each_document(yaml, schema:, max_depth:)` / py
 * `each_document(yaml, schema=, max_depth=)` parity). Unknown or
 * mistyped options throw TypeError. The compiled/JSON-Schema walks
 * have no seat on the walk — the iterator carries a resolver kind,
 * not a schema document; those doors ride validate()/load(). */
export interface EachDocumentOptions {
  /** The resolver family every frame types its plain scalars
   *  through (the parse resolver's kind, the js-yaml parity names
   *  this face uses for validate/load): "core" (default) — the
   *  YAML 1.2 core resolver; "json" — the spec 10.2 JSON resolver. */
  schema?: "core" | "json";
  /** Maximum nesting depth per frame (0 = the library default). A
   *  frame past the cap fails as that frame's error item and the
   *  walk CONTINUES at the next `---` marker. */
  max_depth?: number;
}

/** Walk a multi-document stream one document at a time, TOLERANTLY
 * (the engine's stream iterator): an async generator of
 * EachDocumentResult in stream order. A parse-class failure carries
 * the failed document's OWN line/column and its 0-based stream index,
 * and the walk CONTINUES at the next `---` line-start marker — the
 * valid documents after a bad one are kept (loadAll would fail the
 * whole stream instead). The stream-fatal classes (memory/encoding)
 * yield their item and end the walk; an unrecoverable tail ends it.
 * Lazy: nothing parses until iteration starts. Documents materialize
 * through load's serialize door (the engine's default, schema 1.2
 * core). options selects the resolver family and the per-frame depth
 * cap (ruby/py parity). */
export declare function eachDocument(
  yaml: string,
  options?: EachDocumentOptions,
): AsyncGenerator<EachDocumentResult, void, undefined>;

/** Options for dump (js-yaml parity names over emit.h's
 * yeptris_emit_options). `indent` and `flowLevel` have no engine
 * option — the block indent is fixed at 2 and there is no flow-level
 * override — so they are not in the surface rather than silently
 * ignored. */
export interface DumpOptions {
  /** Fold flow collections once the current line exceeds this column
   *  (the engine's best_width; the emitter wraps after a value).
   *  Default: the engine default (80). -1 disables folding. */
  lineWidth?: number;
  /** The engine's fixed form: flow collections, quoted strings, typed
   *  words, shortest floats — a parse of the output re-serializes
   *  byte-identically. Default: false (the recorded-style fidelity
   *  mode). */
  canonical?: boolean;
}

/** Convert JSON text (or any JSON-serializable value) to the engine's
 * byte-stable YAML. Throws TypeError on an unsupported or mistyped
 * option. */
export declare function dump(
  json: string | unknown,
  options?: DumpOptions,
): Promise<string>;

/** The libyeptris version string. */
export declare function version(): Promise<string>;

/** The wasm module instance the package caches (diagnostics). */
export declare function wasmModule(): Promise<Record<string, unknown>>;

// ---- the Sync postures (usable once the module is initialized) ---------
//
// The wasm executes synchronously; the async forms are async only
// because the FIRST call may initialize the module. After one
// `await init()` (or any async call) the same bodies run
// synchronously. Each Sync form throws unless the module is
// initialized — the lazy import is an async door.

/** Initialize the wasm module (idempotent). Unlocks the Sync forms. */
export declare function init(): Promise<void>;

/** load's Sync twin. Throws unless the module is initialized. */
export declare function loadSync(
  yaml: string,
  options?: ValidateOptions & { collect?: boolean },
): unknown;

/** loadAll's Sync twin. Throws unless the module is initialized. */
export declare function loadAllSync(
  yaml: string,
  options?: ValidateOptions,
): unknown[];

/** loadJSON's Sync twin. Throws unless the module is initialized. */
export declare function loadJSONSync(yaml: string): string;

/** events' Sync twin. Throws unless the module is initialized. */
export declare function eventsSync(
  yaml: string,
  options?: Record<string, unknown>,
): YamlEvent[];

/** dump's Sync twin. Throws unless the module is initialized. */
export declare function dumpSync(
  json: string | unknown,
  options?: DumpOptions,
): string;

/** validate's Sync twin. Throws unless the module is initialized. */
export declare function validateSync(
  input: string,
  options?: ValidateOptions,
): Diagnostic[];

/** isValid's Sync twin. Throws unless the module is initialized. */
export declare function isValidSync(
  input: string,
  options?: ValidateOptions,
): boolean;

/** compileSchema's Sync twin. Throws unless the module is initialized. */
export declare function compileSchemaSync(jsonSchema: object | string): Schema;

/** eachDocument's Sync twin: a sync generator over an initialized
 * module (await init() first). Throws unless the module is
 * initialized. Takes the same options door as eachDocument. */
export declare function eachDocumentSync(
  yaml: string,
  options?: EachDocumentOptions,
): Generator<EachDocumentResult, void, undefined>;

/** version's Sync twin. Throws unless the module is initialized. */
export declare function versionSync(): string;
