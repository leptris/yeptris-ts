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
                    "_yeptris_wasm_validate", "_yeptris_wasm_json_schema_validate",
                    "_yeptris_wasm_schema_compile", "_yeptris_wasm_schema_validate",
                    "_yeptris_wasm_schema_free",
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

// ---- the validate face (validate.h; the ruby/py parity ask) -------------
//
// Diagnostic values here are pinned from live engine probes — the same
// discipline the py face's test_validate.py used (its 25 tests pin the
// engine's real answers, not wished-for ones).

test("validate: a clean document answers [] (the family default)", async () => {
  const { validate, isValid } = await import("../src/index.js");
  assert.deepEqual(await validate("a: 1\nb: [x, y]\n"), []);
  assert.deepEqual(await validate("a: 1\n", {}), []); // empty options = the default walk
  assert.equal(await isValid("a: 1\n"), true);
  assert.equal(await isValid("a: 1\na: 2\n"), false);
});

test("validate: the duplicate-key diagnostic pins whole (code/keyword/path/line/col)", async () => {
  const { validate } = await import("../src/index.js");
  const [d] = await validate("a: 1\na: 2\n");
  assert.deepEqual(d, {
    code: 4, // YEPTRIS_DIAG_DUPLICATE_KEY
    keyword: "duplicateKey",
    path: [], // the mapping root carries the miss
    pointer: "",
    message: 'duplicate mapping key ("a")',
    line: 2, // the SECOND key's position, 1-based
    col: 1,
    offset: 5,
  });
});

test("validate: keys compare by canonical form (0o13 == 0xB)", async () => {
  const { validate } = await import("../src/index.js");
  const [d] = await validate("k: 0o13\nk: 0xB\n");
  assert.equal(d.code, 4);
  assert.equal(d.keyword, "duplicateKey");
  assert.equal(d.line, 2);
});

test("validate: an explicit tag contradicting content rides path /k", async () => {
  const { validate } = await import("../src/index.js");
  const [d] = await validate("k: !!int notanint\n");
  assert.equal(d.code, 1);
  assert.equal(d.keyword, "tag");
  assert.deepEqual(d.path, ["k"]);
  assert.equal(d.pointer, "/k");
  assert.equal(d.message, "explicit int tag on string content");
  assert.equal(d.line, 1);
  assert.equal(d.col, 10);
});

test("validate: schema json applies the spec 10.2 productions", async () => {
  const { validate } = await import("../src/index.js");
  // the example-10.8 law (engine #908): implicit no-production plains
  // are STRINGS in the 12_JSON family — `k: y` validates clean; the
  // jsonType diagnostic fires on EXPLICIT non-JSON tags
  assert.deepEqual(await validate("k: y\n", { schema: "json" }), []);
  const diags = await validate("a: !foo 1\n", { schema: "json" });
  const a = diags.find((d) => d.pointer === "/a");
  assert.ok(a, "the /a scalar is named");
  assert.equal(a.code, 3);
  assert.equal(a.keyword, "jsonType");
  // a non-string mapping key: the key resolves as an int (a JSON
  // production), so only the keyType miss rides — no jsonType rider
  const [key] = await validate("1: one\n", { schema: "json" });
  assert.equal(key.code, 2);
  assert.equal(key.keyword, "keyType");
  // the same document is CLEAN under the core walk — the knob is real
  assert.deepEqual(await validate("a: !foo 1\n", { schema: "core" }), []);
});

test("validate: json walk answers on non-JSON types, not block style (probe discipline)", async () => {
  // PINNED from live probes on the 0.6.60 engine (the example-10.8
  // law, engine #908): the json12 family walk diagnoses non-JSON
  // TYPES — an explicit non-JSON tag, a non-string key — while block
  // STYLE itself is clean: block mappings/sequences whose content is
  // JSON-typed pass, flow and block answer identically.
  const { validate } = await import("../src/index.js");
  assert.deepEqual(await validate("a: 1\n", { schema: "json" }), []);
  assert.deepEqual(await validate("outer:\n  inner: 1\n", { schema: "json" }), []);
  assert.deepEqual(await validate("- 1\n- 2\n", { schema: "json" }), []); // block seq: clean
  assert.deepEqual(await validate('{"a": 1}\n', { schema: "json" }), []); // flow: clean
  assert.deepEqual(
    (await validate("a: !foo 1\n", { schema: "json" })).map((d) => [d.code, d.pointer, d.line, d.col]),
    [[3, "/a", 1, 9]]);
});

test("validate: a multi-document stream's paths name their document", async () => {
  // the stream-attribution law (#885 items 4+5): "/0/a" is of
  // document 0 (0-based); single-document streams keep bare pointers.
  // the marker-line law (#915): a glued marker value ("--- a: 1",
  // "--- - 1") is a parse error at the mark — fixtures ride the
  // vertical-marker shape.
  const { validate } = await import("../src/index.js");
  const diags = await validate("---\na: !foo 1\n---\nb: 2\n", { schema: "json" });
  assert.deepEqual(diags.map((d) => d.pointer), ["/0/a"]);
  assert.deepEqual(diags[0].path, ["0", "a"]);
  assert.equal(diags[0].line, 2); // input-absolute marks
  // and a single-document stream stays unprefixed
  const solo = await validate("a: !foo 1\n", { schema: "json" });
  assert.deepEqual(solo.map((d) => d.pointer), ["/a"]);
  // the glued-marker shape itself is a PARSE error, not a diagnostic
  await assert.rejects(
    () => validate("--- a: 1\n--- a: y\n", { schema: "json" }),
    (e) => e.name === "YamlError" && e.line === 1 && e.column === 6);
});

test("validate: jsonSchema (2020-12) — required + minimum ride their paths", async () => {
  const { validate } = await import("../src/index.js");
  const schema = {
    type: "object",
    required: ["name"],
    properties: { name: { type: "string" }, age: { type: "integer", minimum: 10 } },
  };
  const diags = await validate("age: 3\n", { jsonSchema: schema });
  assert.deepEqual(diags, [
    {
      code: 6,
      keyword: "required",
      path: ["name"],
      pointer: "/name",
      message: 'missing required property "name"',
      line: 1,
      col: 1,
      offset: 0,
    },
    {
      code: 10,
      keyword: "minimum",
      path: ["age"],
      pointer: "/age",
      message: "3 is less than the minimum 10",
      line: 1,
      col: 6,
      offset: 5,
    },
  ]);
  // valid instance answers []
  assert.deepEqual(await validate('name: "x"\nage: 12\n', { jsonSchema: schema }), []);
});

test("validate: jsonSchema takes a string document too", async () => {
  const { validate } = await import("../src/index.js");
  const diags = await validate("k: notanint\n", {
    jsonSchema: '{"type":"object","properties":{"k":{"type":"integer"}}}',
  });
  assert.equal(diags[0].keyword, "type");
  assert.equal(diags[0].pointer, "/k");
  assert.equal(diags[0].message, "expected integer, got string");
});

test("validate: jsonSchema — nested paths, local $ref, unresolvable $ref", async () => {
  const { validate } = await import("../src/index.js");
  const nested = await validate('list: ["ok", 42]\n', {
    jsonSchema: { type: "object", properties: { list: { type: "array", items: { type: "string" } } } },
  });
  assert.equal(nested[0].pointer, "/list/1");
  assert.equal(nested[0].keyword, "type");
  assert.equal(nested[0].message, "expected string, got integer");

  const ref = await validate("n: -5\n", {
    jsonSchema: {
      $defs: { pos: { type: "integer", minimum: 1 } },
      properties: { n: { $ref: "#/$defs/pos" } },
    },
  });
  assert.equal(ref[0].keyword, "minimum");
  assert.equal(ref[0].pointer, "/n");

  const bad = await validate("n: 1\n", {
    jsonSchema: { properties: { n: { $ref: "#/nowhere" } } },
  });
  assert.equal(bad[0].code, 23); // YEPTRIS_DIAG_REF — a diagnostic, not an exception
  assert.equal(bad[0].keyword, "$ref");
  assert.match(bad[0].message, /does not resolve/);
});

test("validate: an unparseable stream throws YamlError, not ValidationError", async () => {
  const { validate, ValidationError } = await import("../src/index.js");
  await assert.rejects(() => validate("a: [1,\n"), (e) => {
    assert.equal(e.name, "YamlError");
    assert.ok(!(e instanceof ValidationError));
    assert.equal(e.diagnostics, undefined); // no report: the PARSE failed
    return true;
  });
});

test("validate: the options door is strict (unknown/mistyped/combined)", async () => {
  const { validate, compileSchema } = await import("../src/index.js");
  await assert.rejects(() => validate("a: 1\n", { bogus: 1 }), TypeError);
  await assert.rejects(() => validate("a: 1\n", { schema: "nope" }), TypeError);
  await assert.rejects(() => validate("a: 1\n", { schema: {} }), TypeError);
  await assert.rejects(() => validate("a: 1\n", { jsonSchema: 42 }), TypeError);
  await assert.rejects(
    () => validate("a: 1\n", { schema: "core", jsonSchema: { type: "object" } }),
    TypeError); // the doors are alternatives — refuse, never silently pick
  const s = await compileSchema({ type: "object" });
  await assert.rejects(
    () => validate("a: 1\n", { schema: s, jsonSchema: { type: "object" } }),
    TypeError);
  s.free();
});

// ---- the compiled schema economy ----------------------------------------

test("compileSchema: one compile, many validates (the economy)", async () => {
  const { compileSchema, validate } = await import("../src/index.js");
  const doc = {
    type: "object",
    required: ["name"],
    properties: { name: { type: "string" }, age: { type: "integer", minimum: 10 } },
  };
  const schema = await compileSchema(doc);
  assert.equal(schema.validate("age: 3\n").length, 2);
  // reuse: same handle, second stream
  assert.deepEqual(await schema.validate('name: "x"\n'), []);
  // the compiled door answers exactly like the one-shot jsonSchema door
  // over the same schema document
  assert.deepEqual(
    await schema.validate("age: 3\n"),
    await validate("age: 3\n", { jsonSchema: doc }));
  assert.equal(schema.isValid("name: x\n"), true);
  assert.equal(schema.isValid("age: 3\n"), false);
});

test("compileSchema: a YAML-shaped schema compiles (JSON is YAML flow)", async () => {
  const { compileSchema } = await import("../src/index.js");
  const s = await compileSchema("type: object\nrequired:\n- name\n");
  const diags = s.validate("age: 1\n");
  assert.equal(diags[0].keyword, "required");
  s.free();
});

test("compileSchema: bad bytes raise, never return a dead handle", async () => {
  const { compileSchema } = await import("../src/index.js");
  await assert.rejects(() => compileSchema(""), TypeError); // "hold no document"
  await assert.rejects(() => compileSchema(undefined), TypeError);
  await assert.rejects(() => compileSchema("{{{ not yaml"), (e) => {
    assert.equal(e.name, "YamlError"); // the engine's why rides last_error
    return true;
  });
});

test("Schema: direct construction is refused; free is idempotent; reuse after free throws", async () => {
  const { compileSchema, Schema } = await import("../src/index.js");
  assert.throws(() => new Schema(), TypeError);
  const s = await compileSchema({ type: "object" });
  assert.equal(s.freed, false);
  s.free();
  assert.equal(s.freed, true);
  s.free(); // idempotent — a second free is a no-op, not a double-free
  assert.throws(() => s.validate("k: v\n"), /freed/);
  assert.throws(() => s.isValid("k: v\n"), /freed/);
});

test("validate: a compiled Schema rides the schema: door", async () => {
  const { compileSchema, validate } = await import("../src/index.js");
  const s = await compileSchema({ type: "object", properties: { k: { type: "integer" } } });
  const diags = await validate("k: notanint\n", { schema: s });
  assert.equal(diags[0].keyword, "type");
  assert.deepEqual(await validate("k: 3\n", { schema: s }), []);
  s.free();
});

// ---- load/loadAll gates (validate BEFORE loading; collect) ---------------

test("load with a gate: diagnostics raise ValidationError carrying the whole report", async () => {
  const { load, ValidationError } = await import("../src/index.js");
  await assert.rejects(
    () => load("a: 1\na: 2\n", { schema: "core" }),
    (e) => {
      assert.ok(e instanceof ValidationError);
      assert.notEqual(e.name, "YamlError"); // the input PARSED — not a parse error
      assert.equal(e.diagnostics.length, 1);
      assert.equal(e.diagnostics[0].keyword, "duplicateKey");
      assert.match(e.message, /validation failed \(1 diagnostics\): duplicateKey/);
      return true;
    });
  // a clean document loads as usual — the gate is invisible when green
  assert.deepEqual(await load("a: 1\n", { schema: "core" }), { a: 1 });
});

test("load with collect: the document AND the whole report answer together", async () => {
  const { load } = await import("../src/index.js");
  const dirty = await load("a: 1\na: 2\n", { schema: "core", collect: true });
  assert.deepEqual(dirty.document, { a: 2 }); // the load still happened
  assert.equal(dirty.diagnostics.length, 1);
  const clean = await load("a: 1\n", { schema: "core", collect: true });
  assert.deepEqual(clean, { document: { a: 1 }, diagnostics: [] });
  // collect requires an armed gate — with nothing to collect it lies
  await assert.rejects(() => load("a: 1\n", { collect: true }), TypeError);
});

test("loadAll with a gate: the WHOLE stream validates before the parse", async () => {
  const { loadAll, ValidationError } = await import("../src/index.js");
  await assert.rejects(
    () => loadAll("---\na: !foo 1\n---\nb: 2\n", { schema: "json" }),
    (e) => {
      assert.ok(e instanceof ValidationError);
      assert.equal(e.diagnostics[0].pointer, "/0/a"); // document 0's miss
      return true;
    });
  // clean streams load as usual — block style is json-clean under the
  // example-10.8 law, and fixtures ride the vertical-marker shape
  // (the marker-line law: a glued marker value is a parse error)
  assert.deepEqual(await loadAll("---\n- 1\n---\n- 2\n", { schema: "json" }),
                   [[1], [2]]);
  assert.deepEqual(await loadAll("---\na: 1\n---\nb: 2\n", { schema: "core" }),
                   [{ a: 1 }, { b: 2 }]);
  // collect rides load() only — loadAll answers documents, not pairs
  await assert.rejects(
    () => loadAll("a: 1\n", { schema: "core", collect: true }), TypeError);
});

// ---- the Sync postures (init-then-work is synchronous) -------------------

test("sync before init is refused with the way out", async () => {
  // a FRESH module instance (the ?query specifier) — the file's other
  // tests have already initialized the shared one
  const fresh = await import("../src/index.js?uninitialized");
  assert.throws(() => fresh.loadSync("k: v\n"), /await init\(\) first/);
  assert.throws(() => fresh.validateSync("k: v\n"), /await init\(\) first/);
  assert.throws(() => fresh.dumpSync({}), /await init\(\) first/);
  assert.throws(() => fresh.versionSync(), /await init\(\) first/);
});

test("sync after init: every form answers like its async twin", async () => {
  const I = await import("../src/index.js");
  await I.init(); // idempotent; the async forms already ran, but be explicit

  assert.deepEqual(I.loadSync("k: v\n"), { k: "v" });
  assert.deepEqual(I.loadAllSync("a: 1\n---\nb: 2\n"), [{ a: 1 }, { b: 2 }]);
  assert.equal(I.loadJSONSync("a: 1\n"), await I.loadJSON("a: 1\n"));
  assert.equal(I.dumpSync({ k: "v" }), await I.dump({ k: "v" }));
  assert.deepEqual(
    I.validateSync("a: 1\na: 2\n"),
    await I.validate("a: 1\na: 2\n"));
  assert.deepEqual(
    I.validateSync("k: y\n", { schema: "json" }),
    await I.validate("k: y\n", { schema: "json" }));
  assert.equal(I.isValidSync("a: 1\n"), true);
  assert.equal(I.isValidSync("a: 1\na: 2\n"), false);
  assert.equal(I.versionSync(), await I.version());
  assert.equal(I.eventsSync("k: v\n").length, (await I.events("k: v\n")).length);
  // the load gate + collect work synchronously too
  assert.throws(() => I.loadSync("a: 1\na: 2\n", { schema: "core" }), I.ValidationError);
  assert.deepEqual(
    I.loadSync("a: 1\na: 2\n", { schema: "core", collect: true }).diagnostics.length, 1);
});

test("sync compiled schema: compileSchemaSync reuses and frees like its twin", async () => {
  const I = await import("../src/index.js");
  await I.init();
  const s = I.compileSchemaSync({ type: "object", properties: { k: { type: "integer" } } });
  assert.deepEqual(s.validate("k: notanint\n").map((d) => d.keyword), ["type"]);
  assert.deepEqual(s.validate("k: 3\n"), []);
  s.free();
  assert.throws(() => s.validate("k: 3\n"), /freed/);
});

// ---- leak discipline across the new doors --------------------------------

test("the wasm output buffer frees after every validate call", async () => {
  const { wasmModule, compileSchema, validate } = await import("../src/index.js");
  const mod = await wasmModule();
  await validate("a: 1\na: 2\n"); // a diagnostic report
  assert.equal(mod._yeptris_wasm_out(), 0, "report not freed after validate");
  await validate("a: 1\n"); // an empty (valid) report
  assert.equal(mod._yeptris_wasm_out(), 0, "report not freed after a clean validate");
  await assert.rejects(() => validate("a: [1,\n")); // a parse failure
  assert.equal(mod._yeptris_wasm_out(), 0);
  const s = await compileSchema({ type: "object" });
  await s.validate("a: 1\na: 2\n");
  assert.equal(mod._yeptris_wasm_out(), 0, "report not freed after a schema validate");
  s.free();
});

// ---- the tolerant stream walk (eachDocument) ----

test("eachDocument: every valid document of a stream yields in order", async () => {
  const { eachDocument } = await import("../src/index.js");
  const items = [];
  for await (const it of eachDocument("--- 41\n--- \"42\"\n---\n")) items.push(it);
  assert.equal(items.length, 3);
  assert.deepEqual(items.map((i) => i.document), [41, "42", null]);
  assert.deepEqual(items.map((i) => i.index), [0, 1, 2]);
  assert.ok(items.every((i) => i.error === undefined));
});

test("eachDocument: keeps valid documents around a bad one", async () => {
  const { eachDocument } = await import("../src/index.js");
  const items = [];
  for await (const it of eachDocument("--- ok_one\n--- {{{{ bad\n--- ok_two\n")) {
    items.push(it);
  }
  assert.equal(items.length, 3);
  assert.equal(items[0].document, "ok_one");
  assert.equal(items[1].document, undefined);
  assert.ok(items[1].error instanceof Error);
  assert.equal(items[1].index, 1);
  assert.equal(items[1].error.line, 2); // the failed document's OWN geometry
  assert.ok(items[1].error.column > 0);
  assert.equal(items[2].document, "ok_two"); // the walk resumed past the bad frame
  assert.equal(items[2].index, 2);
});

test("eachDocument: the quoted-marker frame pins one failure + the raw tail", async () => {
  const { eachDocument } = await import("../src/index.js");
  const items = [];
  for await (const it of eachDocument("\"line one\n---\nline two\"\n")) items.push(it);
  // the grammar rejects the quoted scalar as ONE unit; the raw-marker
  // resync lands on the --- INSIDE the quoted text (the failed frame
  // has no scanner-defined extent), so the tail parses best-effort
  assert.equal(items.length, 2);
  assert.equal(items[0].document, undefined);
  assert.ok(items[0].error instanceof Error);
  assert.equal(items[1].document, "line two\"");
  assert.equal(items[1].index, 1);
});

test("eachDocument: a deep unrecoverable tail ends the walk", async () => {
  const { eachDocument } = await import("../src/index.js");
  // no second --- marker: the tail CONTINUES document 0, so the whole
  // input is one document that fails deep — one error item, index 0
  // (the rs face's pin)
  const deep = "--- [1]\n" + "[".repeat(1200) + "]".repeat(1200) + "\n";
  const items = [];
  for await (const it of eachDocument(deep)) items.push(it);
  assert.equal(items.length, 1);
  assert.equal(items[0].document, undefined);
  assert.ok(items[0].error instanceof Error);
  assert.equal(items[0].index, 0);
});

test("eachDocument: empty and comment streams yield nothing", async () => {
  const { eachDocument } = await import("../src/index.js");
  const a = [];
  for await (const it of eachDocument("")) a.push(it);
  const b = [];
  for await (const it of eachDocument("# just a comment\n")) b.push(it);
  assert.deepEqual([a.length, b.length], [0, 0]);
});

test("eachDocument: a depth failure surfaces as the failed frame's item", async () => {
  const { eachDocument } = await import("../src/index.js");
  const deep = "--- " + "[".repeat(1200) + "]".repeat(1200) + "\n";
  const items = [];
  for await (const it of eachDocument(deep)) items.push(it);
  assert.equal(items.length, 1);
  assert.equal(items[0].document, undefined);
  assert.equal(items[0].index, 0);
  assert.ok(items[0].error.line >= 1);
});

test("eachDocument: frames equal loadAll's documents on a valid stream", async () => {
  const { eachDocument, loadAll } = await import("../src/index.js");
  const src = "name: yeptris\nflow: [1, 2]\n---\n- a\n- b\n---\n~\n";
  const walked = [];
  for await (const it of eachDocument(src)) walked.push(it.document);
  assert.deepEqual(walked, await loadAll(src));
});

test("eachDocumentSync: the walk runs synchronously after init", async () => {
  const I = await import("../src/index.js");
  await I.init();
  const items = [...I.eachDocumentSync("--- 1\n--- {{{{ bad\n--- 3\n")];
  assert.equal(items.length, 3);
  assert.equal(items[0].document, 1);
  assert.equal(items[1].error instanceof Error, true);
  assert.equal(items[2].document, 3);
});

test("eachDocument: closed walks free their state (no retained wasm buffer)", async () => {
  const { wasmModule, eachDocument } = await import("../src/index.js");
  const mod = await wasmModule();
  for await (const _ of eachDocument("--- 1\n--- 2\n")) void _;
  // the walk is over: the shim closed its iterator — a raw next() is
  // refused with the ARG status, not a crash
  assert.notEqual(mod._yeptris_wasm_each_next(), 0);
  void eachDocument;
});

// ---- the options door (schema/max_depth — ruby/py parity) ----

test("eachDocument: the schema door retypes every frame (core vs json)", async () => {
  const { eachDocument } = await import("../src/index.js");
  // the face default rides the engine's 1.2 core resolver (the NULL
  // opts contract); the json family is the parse resolver's kind —
  // it has no True/FALSE/~ productions, so those plain scalars stay
  // strings while the JSON ones (true, 1.5) type identically
  const src = "--- True\n--- FALSE\n--- ~\n--- true\n--- 1.5\n";
  const core = [];
  for await (const it of eachDocument(src)) core.push(it.document);
  assert.deepEqual(core, [true, false, null, true, 1.5]);
  const json = [];
  for await (const it of eachDocument(src, { schema: "json" })) json.push(it.document);
  assert.deepEqual(json, ["True", "FALSE", "~", true, 1.5]);
});

test("eachDocument: max_depth bounds each frame and the walk recovers", async () => {
  const { eachDocument } = await import("../src/index.js");
  const deep = "[".repeat(50) + "]".repeat(50);
  // the library default cap (0 rides it) parses the 50-deep frame
  const plain = [];
  for await (const it of eachDocument(`--- ${deep}\n`)) plain.push(it);
  assert.equal(plain.length, 1);
  assert.ok(Array.isArray(plain[0].document));
  assert.equal(plain[0].error, undefined);
  // max_depth 10 fails the deep frame as ITS OWN item — the why is
  // the engine's depth verdict — and the walk continues at the next
  // marker, keeping the valid tail (the ruby face's pin)
  const items = [];
  for await (const it of eachDocument(`--- ${deep}\n--- recovered\n`, { max_depth: 10 })) {
    items.push(it);
  }
  assert.equal(items.length, 2);
  assert.equal(items[0].document, undefined);
  assert.match(items[0].error.message, /depth/);
  assert.equal(items[0].index, 0);
  assert.equal(items[1].document, "recovered");
  assert.equal(items[1].index, 1);
});

test("eachDocument: an options object arms the door at the same defaults", async () => {
  const { eachDocument } = await import("../src/index.js");
  const src = "--- 41\n--- [1]\n---\n";
  const ride = [];
  for await (const it of eachDocument(src)) ride.push(it.document);
  const empty = [];
  for await (const it of eachDocument(src, {})) empty.push(it.document);
  const explicit = [];
  for await (const it of eachDocument(src, { schema: "core", max_depth: 0 })) {
    explicit.push(it.document);
  }
  const nulled = [];
  for await (const it of eachDocument(src, null)) nulled.push(it.document);
  // zeroed options are the parse defaults (parse.h): identical to the
  // NULL ride — the face's shipped v1 contract either way
  assert.deepEqual(empty, ride);
  assert.deepEqual(explicit, ride);
  assert.deepEqual(nulled, ride);
});

test("eachDocument: the options door is strict (unknown or mistyped keys throw)", async () => {
  const { eachDocument, eachDocumentSync, compileSchema } = await import("../src/index.js");
  const bad = (opts) => async () => {
    for await (const _ of eachDocument("--- 1\n", opts)) void _;
  };
  await assert.rejects(bad({ schema: "compat" }), /schema must be/);
  await assert.rejects(bad({ schema: 2 }), /schema must be/);
  await assert.rejects(bad({ schema: "core", jsonSchema: {} }), /not supported/);
  await assert.rejects(bad({ max_depth: 1.5 }), /max_depth/);
  await assert.rejects(bad({ max_depth: -1 }), /max_depth/);
  await assert.rejects(bad({ collect: true }), /not supported/);
  await assert.rejects(bad("core"), /must be an object/);
  // the compiled/JSON-Schema doors have no seat on the walk — the
  // iterator takes the resolver kind, not a schema document
  const s = await compileSchema({ type: "object" });
  await assert.rejects(bad({ schema: s }), /validate\(\)\/load\(\)/);
  s.free();
  // the Sync twin validates at call time
  assert.throws(() => eachDocumentSync("--- 1\n", { schema: "compat" }), /schema must be/);
  assert.throws(() => eachDocumentSync("--- 1\n", { max_depth: "10" }), /max_depth/);
});

test("eachDocumentSync: the options door rides synchronously", async () => {
  const I = await import("../src/index.js");
  await I.init();
  const items = [...I.eachDocumentSync("--- True\n--- {{{{ bad\n--- 3\n", { schema: "json" })];
  assert.equal(items[0].document, "True");
  assert.ok(items[1].error instanceof Error);
  assert.equal(items[2].document, 3);
});

test("eachDocument: the capability probe refuses options on a stale shim", async () => {
  const { eachDocument, eachDocumentSync, wasmModule } = await import("../src/index.js");
  const mod = await wasmModule();
  const realEx = mod._yeptris_wasm_each_open_ex;
  // the pre-door shim: the _ex export simply isn't there (the v1 open
  // keeps its frozen 2-param ABI). Hide the export and pin both
  // halves of the probe: the no-options walk keeps working (the v1
  // contract on the old era), the door refuses loudly instead of
  // pretending the options had a seat.
  mod._yeptris_wasm_each_open_ex = undefined;
  try {
    const items = [];
    for await (const it of eachDocument("--- 1\n--- 2\n")) items.push(it.document);
    assert.deepEqual(items, [1, 2]);
    await assert.rejects(
      async () => {
        for await (const _ of eachDocument("--- 1\n", { schema: "json" })) void _;
      },
      /options door/,
    );
    assert.throws(() => [...eachDocumentSync("--- 1\n", { max_depth: 5 })], /options door/);
  } finally {
    mod._yeptris_wasm_each_open_ex = realEx;
  }
  // the export is back: the door answers again
  const items = [];
  for await (const it of eachDocument("--- 0o7\n", { schema: "json" })) items.push(it.document);
  assert.deepEqual(items, ["0o7"]);
});
