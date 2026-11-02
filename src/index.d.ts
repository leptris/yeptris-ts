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

/** Convert JSON text (or any JSON-serializable value) to the engine's
 * canonical, byte-stable YAML. */
export declare function dump(json: string | unknown): Promise<string>;

/** The libyeptris version string. */
export declare function version(): Promise<string>;

/** The wasm module instance the package caches (diagnostics). */
export declare function wasmModule(): Promise<Record<string, unknown>>;
