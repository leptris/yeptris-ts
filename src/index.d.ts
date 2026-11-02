// Type declarations for yeptris (the wasm binding).
export declare class YamlError extends Error {
  line: number;
  column: number;
}

/** Parse YAML text and return the document as a JavaScript value
 * (the engine's canonical JSON, JSON.parse'd). Throws YamlError with
 * line/column on malformed input. A multi-document stream answers
 * with the FIRST document; every document comes from loadAll. */
export declare function load(yaml: string): Promise<unknown>;

/** Parse a multi-document YAML stream and return EVERY document as a
 * JavaScript value, in stream order. An empty or comment-only stream
 * answers []. options is reserved (no options today). Throws
 * YamlError with line/column on malformed input. */
export declare function loadAll(
  yaml: string,
  options?: Record<string, unknown>,
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
