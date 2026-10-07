// The public surface: YAML <-> JSON at libyeptris speed.
// Engine FFI through ../dist/yeptris.mjs (built by build.sh).

function cstr(mod, s) {
  const bytes = mod.lengthBytesUTF8(s) + 1;
  const ptr = mod._malloc(bytes);
  mod.stringToUTF8(s, ptr, bytes);
  return ptr;
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

function run(mod, fn, input) {
  const ptr = cstr(mod, input);
  try {
    const st = fn(ptr, input.length);
    if (st !== 0) {
      const msg = mod.UTF8ToString(mod._yeptris_wasm_last_error());
      throw new YamlError(
        msg,
        mod._yeptris_wasm_last_error_line(),
        mod._yeptris_wasm_last_error_column());
    }
    const out = mod.UTF8ToString(mod._yeptris_wasm_out());
    return out;
  } finally {
    freeCstr(mod, ptr);
  }
}

let cached = null;

async function mod() {
  if (cached) return cached;
  const factory = (await import("../dist/yeptris.mjs")).default;
  cached = await factory();
  return cached;
}

/** Parse YAML text and return the document as a JavaScript value
 * (the engine's canonical JSON, JSON.parse'd). Throws YamlError with
 * line/column on malformed input. */
export async function load(yaml) {
  const m = await mod();
  return JSON.parse(run(m, m._yeptris_wasm_load, yaml));
}

/** Parse YAML text and return the engine's canonical JSON string. */
export async function loadJSON(yaml) {
  const m = await mod();
  return run(m, m._yeptris_wasm_load, yaml);
}

/** Convert JSON text (or any JSON-serializable value) to the engine's
 * canonical, byte-stable YAML. JSON is a subset of YAML 1.2 flow, so
 * the one parse engine takes it. */
export async function dump(json) {
  const m = await mod();
  const text = typeof json === "string" ? json : JSON.stringify(json);
  return run(m, m._yeptris_wasm_dump, text);
}

/** The libyeptris version string. */
export async function version() {
  const m = await mod();
  return m.UTF8ToString(m._yeptris_wasm_version());
}
