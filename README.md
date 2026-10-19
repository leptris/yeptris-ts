# yeptris-wasm

YAML for JavaScript at [libyeptris](https://github.com/leptris/yeptris)
speed — the engine compiled to WebAssembly (ES module, no fallback).

Part of the yeptris program: *the best (multiple dimensions)
YAML/JSON parser/builder ever* — conformance, performance,
safety/hardening, API surfaces, transport correctness, and trust,
held simultaneously, in C, Ruby, Rust, and JS. This binding is the
thinnest surface (the WASM module); the recorder bulk-drain and
iterparse are planned next (the engine's PLAN.md, "The best-ever
program").

```js
import { load, dump } from "yeptris-wasm";

const doc = await load("name: yeptris\nlangs:\n- c\n- rust\n");
// { name: "yeptris", langs: ["c", "rust"] }

const yaml = await dump({ name: "yeptris", langs: ["c", "rust"] });
// the engine's canonical, byte-stable YAML
```

- `load(yaml)` — parse to a JavaScript value (malformed input throws
  `YamlError` with line/column).
- `loadJSON(yaml)` — the engine's canonical JSON string.
- `dump(json)` — JSON text (or any JSON-serializable value) to
  canonical YAML; JSON is a subset of YAML 1.2 flow, so the one parse
  engine takes it.

The same model as [teptris-wasm](https://github.com/leptris/teptris-wasm):
a contained C bridge (`src/yeptris_wasm.c`) over the engine's
buffer-lifetime contract, compiled by `build.sh` (emcc) from the
sibling engine checkout, surfaced as an ES module.

## Build and test

```sh
./build.sh            # ENGINESRC=../yeptris/src by default
node --test test/test.mjs
```

CI builds the engine tag and runs the same on every push and PR.

## Releasing

`npm publish` runs through the dispatch-only workflow (Trusted
Publishing — configure `leptris/yeptris-wasm` + `release.yml` as the
package's publisher on npmjs). The first publish is manual; after
that, dispatch **Release** and the dist build + publish + GitHub
release run as one train.
