import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { load, loadAll, loadJSON, dump, version, events } from "../src/index.js";

// The cross-runtime event fixture set (leptris/yeptris#886 item 5):
// canonical inputs + their expected normalized event sequences, one
// vocabulary shared with the ruby/py/rs faces. The vocabulary is
// documented as data in the file itself.
const FIXTURES = JSON.parse(
  readFileSync(new URL("./fixtures/events.json", import.meta.url), "utf8"));

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

// ---- multi-document streams (loadAll) ----

test("loadAll: a multi-document stream yields every document", async () => {
  assert.deepEqual(await loadAll("a: 1\n---\nb: 2\n"), [{ a: 1 }, { b: 2 }]);
});

test("loadAll: a single-document stream yields a length-1 array", async () => {
  assert.deepEqual(await loadAll("name: yeptris\n"), [{ name: "yeptris" }]);
});

test("loadAll: empty and comment-only streams yield an empty array", async () => {
  // the engine answers a content-less stream with zero documents
  assert.deepEqual(await loadAll(""), []);
  assert.deepEqual(await loadAll("# just a comment\n"), []);
});

test("loadAll: empty documents ride the stream as null", async () => {
  assert.deepEqual(await loadAll("---\n---\n"), [null, null]);
  assert.deepEqual(await loadAll("a: 1\n---\n"), [{ a: 1 }, null]);
  assert.deepEqual(await loadAll("a: 1\n--- # explicit\n---\nb: 2\n"),
                   [{ a: 1 }, null, { b: 2 }]);
});

test("loadAll: scalars, sequences and styles mirror load()", async () => {
  assert.deepEqual(await loadAll("1\n---\n2\n---\n3\n"), [1, 2, 3]);
  assert.deepEqual(await loadAll("- 1\n- 2\n---\n- 3\n"), [[1, 2], [3]]);
  // plain typed vs quoted/tagged forced-string, block scalars kept
  assert.deepEqual(
    await loadAll('a: !!str 42\n---\nb: "42"\n---\nc: |\n  l1\n'),
    [{ a: "42" }, { b: "42" }, { c: "l1\n" }]);
});

test("loadAll: aliases project to null and anchors drop (the JSON law)", async () => {
  // the JSON projection dereferences aliases to null (bounded — the
  // emitter's emit_alias_ex law); anchors spell nothing in JSON
  const src = "a: &x 1\nb: *x\nc:\n- k\n- *x\n";
  assert.deepEqual(await load(src), { a: 1, b: null, c: ["k", null] });
  assert.deepEqual(await loadAll(src), [{ a: 1, b: null, c: ["k", null] }]);
});

test("loadAll: options is accepted and reserved", async () => {
  assert.deepEqual(await loadAll("a: 1\n", {}), [{ a: 1 }]);
});

test("loadAll: malformed input carries line/column", async () => {
  await assert.rejects(
    () => loadAll("a: [1,\n---\nb: 2\n"),
    (e) => e.name === "YamlError" && e.line >= 1);
});

test("load: a multi-document stream answers with the FIRST document", async () => {
  // pinned: load() is the single-document form and json.h's contract
  // is "serializes the document's FIRST document". Before the fix the
  // multi-doc forest serialized EMPTY and load() died in JSON.parse
  // with a raw SyntaxError (not a YamlError).
  assert.deepEqual(await load("a: 1\n---\nb: 2\n"), { a: 1 });
  assert.equal(await loadJSON("a: 1\n---\nb: 2\n"), '{"a": 1}\n');
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

// ---- dump options (js-yaml parity over emit.h's yeptris_emit_options) ----

// 13 flow items: the emitter folds past a line's width, so the default
// (80) wraps while a huge width cannot
const LONG_ROW = {
  k: ["aaaa", "bbbb", "cccc", "dddd", "eeee", "ffff", "gggg", "hhhh",
      "iiii", "jjjj", "kkkk", "llll", "mmmm"],
};

test("dump: lineWidth folds flow collections past the column", async () => {
  const folded = await dump(LONG_ROW); // the 80 default folds
  const wide = await dump(LONG_ROW, { lineWidth: 100000 });
  assert.notEqual(folded, wide, "the default must fold a long flow seq");
  assert.ok(folded.includes("\n"), "the folded form carries newlines");
  // the emitter always writes a trailing newline — strip it before
  // judging interior folds
  assert.ok(!wide.trimEnd().includes("\n"), "a huge width must not fold");
  assert.deepEqual(await load(folded), LONG_ROW, "folded form re-parses");
  assert.deepEqual(await load(wide), LONG_ROW, "unfolded form re-parses");
});

test("dump: lineWidth -1 disables folding (js-yaml's unlimited)", async () => {
  const unlim = await dump(LONG_ROW, { lineWidth: -1 });
  assert.equal(unlim, await dump(LONG_ROW, { lineWidth: 100000 }));
});

test("dump: a narrower width folds tighter", async () => {
  const narrow = await dump(LONG_ROW, { lineWidth: 20 });
  const folded = await dump(LONG_ROW); // the 80 default
  assert.ok(
    narrow.split("\n").length > folded.split("\n").length,
    `width 20 must fold tighter than 80:\n${narrow}`);
  assert.deepEqual(await load(narrow), LONG_ROW, "the tight fold re-parses");
});

test("dump: canonical rides the engine's fixed form", async () => {
  const doc = { a: 1, b: "x", c: [1, 2] };
  const canon = await dump(doc, { canonical: true });
  assert.notEqual(canon, await dump(doc), "canonical differs from fidelity mode");
  assert.ok(canon.includes("{"), "canonical spells flow collections");
  assert.ok(canon.includes('"x"'), "canonical quotes strings");
  // the canonical law: a parse of the output re-serializes byte-identically
  assert.equal(await dump(await load(canon), { canonical: true }), canon);
});

test("dump: default options match the no-options dump byte-for-byte", async () => {
  // the wasm bridge's defaults (0, 0) are the library defaults: the
  // options call must be a drop-in for the plain one
  const doc = { a: [1, 2], b: { c: null }, d: "x".repeat(200) };
  assert.equal(await dump(doc, {}), await dump(doc));
  assert.equal(await dump(doc, { lineWidth: 80 }), await dump(doc));
  assert.equal(await dump(doc, null), await dump(doc));
});

test("dump: option validation", async () => {
  // known keys are typed strictly
  await assert.rejects(() => dump({}, { lineWidth: "80" }), TypeError);
  await assert.rejects(() => dump({}, { lineWidth: 1.5 }), TypeError);
  await assert.rejects(() => dump({}, { lineWidth: 0 }), TypeError);
  await assert.rejects(() => dump({}, { lineWidth: -2 }), TypeError);
  await assert.rejects(() => dump({}, { canonical: 1 }), TypeError);
  // indent/flowLevel have no engine option: rejected, never ignored
  await assert.rejects(() => dump({}, { indent: 4 }), TypeError);
  await assert.rejects(() => dump({}, { flowLevel: 1 }), TypeError);
});

test("round trip: yaml -> js -> yaml -> js is stable", async () => {
  const src = "a:\n  b:\n  - 1\n  - x: y\n";
  const once = await load(src);
  const twice = await load(await dump(once));
  assert.deepEqual(twice, once);
});

// ---- the event face (the recorder's bulk drain) ----

for (const fixture of FIXTURES.cases) {
  test(`events fixture: ${fixture.name}`, async () => {
    if (fixture.error) {
      await assert.rejects(
        () => events(fixture.input),
        (e) =>
          e.name === "YamlError" &&
          e.line === fixture.error.line &&
          e.column === fixture.error.column);
    } else {
      assert.deepEqual(await events(fixture.input), fixture.events);
    }
  });
}

test("events: document boundaries match loadAll for every fixture", async () => {
  // the same law in both faces: one document-start per loadAll element
  for (const fixture of FIXTURES.cases) {
    if (fixture.error) continue;
    const evs = await events(fixture.input);
    const docs = evs.filter((e) => e.type === "document-start");
    const all = await loadAll(fixture.input);
    assert.equal(all.length, docs.length,
                 `${fixture.name}: document-start count != loadAll length`);
  }
});

test("events: stream-start is first and stream-end is last", async () => {
  for (const fixture of FIXTURES.cases) {
    if (fixture.error) continue;
    const evs = await events(fixture.input);
    assert.equal(evs[0].type, "stream-start", fixture.name);
    assert.equal(evs[evs.length - 1].type, "stream-end", fixture.name);
  }
});

test("events: every scalar names its resolved kind", async () => {
  // a sequence: keys are scalars too, so a map would fold them in
  const evs = await events("- 42\n- text\n- 1.5\n- true\n- null\n");
  const kinds = evs.filter((e) => e.type === "scalar").map((e) => e.tagId);
  assert.deepEqual(kinds, ["int", "str", "float", "bool", "null"]);
});

test("events: aliases carry the alias name and anchors the anchor", async () => {
  const evs = await events("a: &x 1\nb: *x\n");
  const anchor = evs.find((e) => e.anchor === "x");
  assert.equal(anchor.type, "scalar");
  assert.equal(anchor.value, "1");
  const alias = evs.find((e) => e.type === "alias");
  assert.equal(alias.value, "x");
  assert.equal(alias.tagId, undefined);
  assert.equal(alias.style, undefined);
});

test("events: options is accepted and reserved", async () => {
  // stream/doc/map start+end, two scalars
  assert.equal((await events("k: v\n", {})).length, 8);
});

test("events: survives heap growth mid-call (the views re-bind on growth)", async () => {
  // ALLOW_MEMORY_GROWTH swaps the wasm buffer on demand; the decoder
  // must fetch the exported views AFTER the wasm call, never hold a
  // stale one across calls. A multi-MB stream forces the growth.
  const big = "k: \"" + "x".repeat(2 * 1024 * 1024) + "\"\n";
  const evs = await events(big);
  const scalar = evs.find((e) => e.type === "scalar" && e.value && e.value.length > 1000);
  assert.equal(scalar.value.length, 2 * 1024 * 1024);
  assert.equal(scalar.style, "double");
});

test("events: the drain frees after every call (success and error)", async () => {
  const { wasmModule } = await import("../src/index.js");
  const mod = await wasmModule();
  await events("k: v\n");
  assert.equal(mod._yeptris_wasm_event_count(), 0, "drain not freed after events");
  assert.equal(mod._yeptris_wasm_event_records(), 0);
  await assert.rejects(() => events("k: [unclosed\n"));
  assert.equal(mod._yeptris_wasm_event_count(), 0, "drain not freed after an error");
});

test("module guard: the built dist carries every guarded export", async () => {
  const factory = (await import("../dist/yeptris.mjs")).default;
  const m = await factory();
  for (const fn of ["_yeptris_wasm_load", "_yeptris_wasm_load_all", "_yeptris_wasm_dump",
                    "_yeptris_wasm_dump_ex",
                    "_yeptris_wasm_events", "_yeptris_wasm_event_records",
                    "_yeptris_wasm_event_count", "_yeptris_wasm_event_arena",
                    "_yeptris_wasm_event_arena_len", "_yeptris_wasm_events_free",
                    "_yeptris_wasm_out", "_yeptris_wasm_last_error", "_yeptris_wasm_free",
                    "_malloc"]) {
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
