# yeptris

YAML for JavaScript at [libyeptris](https://github.com/leptris/yeptris)
speed — the engine compiled to WebAssembly (ES module, no fallback).

Part of the yeptris program: *the best (multiple dimensions)
YAML/JSON parser/builder ever* — conformance, performance,
safety/hardening, API surfaces, transport correctness, and trust,
held simultaneously, in C, Ruby, Rust, and JS. This binding is the
thinnest surface (the WASM module); the recorder bulk-drain rides the
`events()` face, iterparse is planned next (the engine's PLAN.md, "The
best-ever program").

```js
import { load, dump } from "yeptris";

const doc = await load("name: yeptris\nlangs:\n- c\n- rust\n");
// { name: "yeptris", langs: ["c", "rust"] }

const yaml = await dump({ name: "yeptris", langs: ["c", "rust"] });
// the engine's canonical, byte-stable YAML
```

## API

| Function | What it answers |
| --- | --- |
| `load(yaml, options?)` | The document as a JavaScript value (malformed input throws `YamlError` with line/column). A multi-document stream answers with the FIRST document. With a validation gate armed (below) it validates BEFORE loading. |
| `loadAll(yaml, options?)` | Every document of a multi-document stream, in stream order (`js-yaml` parity; an empty stream answers `[]`). An armed gate validates the whole stream first. |
| `loadJSON(yaml)` | The engine's canonical JSON string. |
| `dump(json, options?)` | JSON text (or any JSON-serializable value) to canonical YAML; JSON is a subset of YAML 1.2 flow, so the one parse engine takes it. Options (js-yaml parity names over the engine's `yeptris_emit_options`): `lineWidth` folds flow collections past that column (default 80, `-1` disables folding), `canonical` rides the engine's fixed form. `indent`/`flowLevel` have no engine option and are rejected, never silently ignored. |
| `events(yaml, options?)` | Every event of the stream as an array of plain objects, in stream order: the engine's recorder bulk drain (one wasm crossing per stream). The normalized event vocabulary is shared with the ruby/py/rs faces and documented as data in [`test/fixtures/events.json`](test/fixtures/events.json). |
| `validate(input, options?)` | The WHOLE validation report: every diagnostic, in engine order (`[]` when valid). |
| `isValid(input, options?)` | `true` when `validate` answers no diagnostics. |
| `compileSchema(jsonSchema)` | A compiled `Schema` handle — parse the schema ONCE, validate many streams. |
| `version()` | The libyeptris version string. |
| `wasmModule()` | The cached wasm module instance (diagnostics: heap/memory inspection, the engine's raw exports). |
| `init()` | Initializes the wasm module (idempotent) — what unlocks the `*Sync` forms. |

### Sync postures

The wasm executes synchronously; every async form is async only
because the FIRST call may initialize the module. After one
`await init()` (or any async call) the same bodies run synchronously:
`loadSync`, `loadAllSync`, `loadJSONSync`, `dumpSync`, `eventsSync`,
`validateSync`, `isValidSync`, `compileSchemaSync`, `versionSync` —
same arguments, same answers, no `Promise`. Each `*Sync` form refuses
to run uninitialized (the lazy import is an async door) and names the
way out in the error; the `Schema` handle's own methods
(`validate`/`isValid`) are synchronous by construction — a handle only
exists once the module is initialized.

```js
await init();                      // once, up front
const doc = loadSync(yaml);        // synchronous from here on
const diags = validateSync(yaml, { schema: "core" });
```

## Validation

```js
import { validate, isValid, compileSchema, load } from "yeptris";

const diags = await validate("a: 1\na: 2\n");
// [{ code: 4, keyword: "duplicateKey", path: [], pointer: "",
//    message: 'duplicate mapping key ("a")', line: 2, col: 1, offset: 5 }]

await isValid("a: 1\n");           // true

const schema = await compileSchema({ type: "object", required: ["name"] });
schema.validate("age: 1\n");       // [{ keyword: "required", pointer: "/name", ... }]
schema.isValid('name: "x"\n');     // true
schema.free();                     // once, when done — reuse afterwards throws
```

Every diagnostic is a plain object — never a raw pointer:
`code` (the engine's `YeptrisDiagCode`), `keyword` (the spec-spelled
keyword: the violated JSON Schema keyword for the 2020-12 walk, the
violated YAML rule for the family walk), `path` (the JSON-pointer
instance path split into `~0`/`~1`-unescaped segments, `[]` at the
root), `pointer` (its raw string form), `message` (the engine's
bounded detail), `line`/`col` (the offending node's 1-based mark,
`0` = unknown: scalars whose bytes the DOM owns have no input
position — the path still identifies them), and `offset`. Results are
WHOLE REPORTS: one call enumerates every miss, never first-fail.
A multi-document stream's paths name their document — `"/1/a"` is `a`
of document 1 (0-based); single-document streams keep bare pointers.

### Schema selection (the js-yaml `load(str, { schema })` analog)

| js-yaml | yeptris | What runs |
| --- | --- | --- |
| `schema: DEFAULT_SCHEMA` | `schema: "core"` (the default) | The YAML 1.2 CORE family walk: explicit-tag consistency (spec 10.3 resolution) and mapping-key uniqueness by canonical form (`0o13` and `0xB` are one key). |
| `schema: JSON_SCHEMA` | `schema: "json"` | The spec 10.2 JSON family walk: every plain scalar must match a JSON production, mapping keys must be strings. |
| — | `jsonSchema: object \| string` | JSON Schema draft 2020-12 (the engine's keyword subset; unknown keywords are ignored, per 2020-12). |
| — | `schema: compiledSchema` | The compiled economy: `compileSchema()` parses the schema once, the handle validates many streams. |

The doors are alternatives — passing `schema` AND `jsonSchema` throws
`TypeError` (pick the door you meant; nothing is silently dropped),
as is any unknown or mistyped option. A `$ref` that resolves nowhere
is a diagnostic, not an exception; a stream that does not parse
throws `YamlError` (a parse failure, not a validation report).

### Gating a load

`load`/`loadAll` accept the same options and validate BEFORE loading
(the ruby/py face law):

```js
await load("a: 1\na: 2\n", { schema: "core" });
// throws ValidationError (the input PARSED — not a YamlError);
// e.diagnostics carries the whole report

await load("a: 1\na: 2\n", { schema: "core", collect: true });
// { document: { a: 2 }, diagnostics: [ ... ] } — the load and the
// whole report answer together (collect requires an armed gate)
```

`loadAll` validates the whole stream before the parse and raises the
same `ValidationError` (collect rides `load` only).

The same model as [teptris-wasm](https://github.com/leptris/teptris-wasm):
a contained C bridge (`src/yeptris_wasm.c`) over the engine's
buffer-lifetime contract, compiled by `build.sh` (emcc) from the
sibling engine checkout, surfaced as an ES module.

## Build and test

```sh
./build.sh            # ENGINESRC=../yeptris/src by default
node --test test/test.mjs
```

CI builds the engine ref and runs the same on every push and PR.

## Releasing

`npm publish` runs through the dispatch-only workflow (Trusted
Publishing — configure `leptris/yeptris-ts` + `release.yml` as the
package's publisher on npmjs). The first publish is manual; after
that, dispatch **Release** and the dist build + publish + GitHub
release run as one train.
