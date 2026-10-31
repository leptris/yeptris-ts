import { test } from "node:test";
import assert from "node:assert/strict";

import { load, loadJSON, dump, version } from "../src/index.js";

test("version reports the engine", async () => {
  const v = await version();
  assert.match(v, /^0\.6\.\d+/);
});

test("load: block map to a JS object", async () => {
  const doc = await load("name: yeptris\nlangs:\n- c\n- rust\n");
  assert.deepEqual(doc, { name: "yeptris", langs: ["c", "rust"] });
});

test("load: nested + flow + scalars", async () => {
  const doc = await load(
    "meta:\n  star: true\n  count: 42\n  pi: 3.5\n  none: null\nflow: [1, 2]\n");
  assert.deepEqual(doc, {
    meta: { star: true, count: 42, pi: 3.5, none: null },
    flow: [1, 2],
  });
});

test("load: malformed input carries line/column", async () => {
  await assert.rejects(
    () => load("a: [1,\n"),
    (e) => e.name === "YamlError" && e.line >= 1);
});

test("loadJSON: the canonical JSON string", async () => {
  const json = await loadJSON('{"a": [1, 2]}');
  assert.deepEqual(JSON.parse(json), { a: [1, 2] });
});

test("dump: JSON value to canonical YAML", async () => {
  const yaml = await dump({ name: "yeptris", langs: ["c", "rust"] });
  const back = await load(yaml);
  assert.deepEqual(back, { name: "yeptris", langs: ["c", "rust"] });
});

test("dump: JSON text to canonical YAML", async () => {
  const yaml = await dump('{"a": {"b": [true, null]}}');
  const back = await load(yaml);
  assert.deepEqual(back, { a: { b: [true, null] } });
});

test("round trip: yaml -> js -> yaml -> js is stable", async () => {
  const src = "a:\n  b:\n  - 1\n  - x: y\n";
  const once = await load(src);
  const twice = await load(await dump(once));
  assert.deepEqual(twice, once);
});

test("module guard: the built dist carries every guarded export", async () => {
  const factory = (await import("../dist/yeptris.mjs")).default;
  const m = await factory();
  for (const fn of ["_yeptris_wasm_load", "_yeptris_wasm_dump", "_yeptris_wasm_out",
                    "_yeptris_wasm_last_error", "_yeptris_wasm_free", "_malloc"]) {
    assert.equal(typeof m[fn], "function", `${fn} must be exported`);
  }
});

test("module guard: the wrapper refuses an export-less module", async () => {
  const { readFileSync } = await import("node:fs");
  const src = readFileSync(new URL("../src/index.js", import.meta.url), "utf8");
  assert.match(src, /missing the wasm export/, "the loud-failure guard is present");
});

test("non-ASCII documents parse whole (byte length, not UTF-16 units)", async () => {
  // input.length counts UTF-16 code units; the wire needs UTF-8 bytes —
  // every non-ASCII document parsed truncated before the fix
  const doc = await load('name: "café"\nemoji: " rocket"\n');
  assert.deepEqual(doc, { name: "café", emoji: " rocket" });
  const big = 'k: "ééééé ' + "x".repeat(4000) + '"\n';
  const back = await load(big);
  assert.equal(back.k.length, 5 + 1 + 4000);
  assert.equal(back.k.endsWith("x"), true);
});

test("the wasm output buffer frees after every call", async () => {
  // the C side mallocs one output per successful call; nothing else
  // freed it — a long-running process grew the wasm heap by one output
  // per call. After a call the global must be back to NULL.
  const { wasmModule } = await import("../src/index.js");
  const mod = await wasmModule(); // THE instance load()/dump() share
  await load("k: v\n");
  assert.equal(mod._yeptris_wasm_out(), 0, "output buffer not freed after load");
  await dump('{"k":"v"}');
  assert.equal(mod._yeptris_wasm_out(), 0, "output buffer not freed after dump");
  await assert.rejects(() => load("k: [unclosed\n")); // an ERROR call
  assert.equal(mod._yeptris_wasm_out(), 0);
});
