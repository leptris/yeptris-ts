# Changelog

All notable changes to this project are documented in this file. Versions
are bumped by `scripts/bump-version.sh` (CMakeLists.txt is the single
source of truth; this file, vcpkg.json are synced from it).

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]
### Fixed
Post-0.6.32 fuzz catches and the tag-props family (each pinned;
crashers in the regression corpus).
- **parse: step_scan tracks flow-value quotes and verbatim tags** —
  the incremental cut scanner's token-start rule after a plain-led
  key left the quote at a flow VALUE position untracked, so the cut
  landed inside the quoted span (`{f '}` + `---` + `'}'`: whole saw
  the far close and swallowed the `---` marker, chunked errored on
  the unterminated prefix); and a cut inside a `!` verbatim tag
  (`{ !< }` + `---`) made the partial prefix's flow pre-scan fail
  before `MAP_START` emitted while the whole buffer's succeeded. In a
  flow a space now always re-arms the token start, `!` verbatim tags
  are tracked (and a possibly-truncated `!` holds the tail), and the
  flow pre-scan defers unclosed collections on partial feeds while
  complete-buffer errors stay (nightly runs 38530395946 and
  38534965021, catches 92 and 93).
- **parse: the break normalizer keeps the input's own length** — the
  NEL/LS/PS copy is null-terminated and the run measured it with
  `strlen`, so an input carrying a NUL byte of its own was truncated
  at the first one: `\0\n---\r<U+2028>` whole-parsed as an empty
  stream (the line separator triggered the copy, the NUL cut it)
  while a chunked feed cut before the separator and never copied —
  two different streams for the same bytes (nightly run 38524928004,
  catch 91). The copied length is the normalizer's own count now.
- **parse: the flow key lookahead restores the line state with the
  cursor** — the key detection's whitespace lookahead crosses the
  newline after a flow value hunting for a `:`, and the no-colon
  rollback put the cursor back but left `e->line` one high: a
  multi-line flow collection's END (and every mark after it) parsed
  one line low in any chunked feed, while whole feeds took the JSON
  fast path (its own line walk) and never noticed — whole and chunked
  streams disagreed on the same bytes (nightly run 38518507064,
  catch 90).
- **parse: the JSON fast paths are byte-for-byte JSON** — the flow
  walk's lenient mode accepted number and literal keys, so a whole
  parse of `{9:1}` rode the fast path and emitted `{9 -> 1}` while a
  chunked feed fell under the small-span gate to the general kernel,
  which correctly reads `9:1` as one plain scalar (`{"9:1" -> null}`):
  the same bytes produced two different streams depending on how the
  input was split. The engine's pass-1 walk, the DOM fused builder,
  and the ytape probe all walk strict now — non-JSON spans fall back
  to the general kernel's colon rules in every mode (nightly run
  38513849602, catch 89).
- **parse: nested explicit-key pairs flush their nulls at their own
  levels** — `? ? k : a` stacked two pending pair values over a single
  depth slot: the inner pair clobbered the outer's level, the outer
  null flushed inside the wrong map, and a following anchored value
  lost its anchor — the trailing alias emitted a dangling reference
  and the emitted document failed to re-parse. The pending levels are
  a stack now; every `key: value` completion funnels through one
  wrapper that resolves the innermost pending, and each null flushes
  directly inside its own `?` map (matching the libyaml oracle
  `{{{"k" => "a"} => nil} => nil}`; nightly run 38387614146, catch 88).
- **parse: a second props line cannot displace a pending anchor** —
  a props-only line followed by another props line carrying its own
  anchor ("&a\n&a1\nF: *a", or the '? &a' explicit-key twin) let the
  same-line anchor win the merge, so the displaced anchor stayed
  defined in the nametab while never appearing in the output: the
  trailing alias emitted a dangling reference and the emitted document
  failed to re-parse. Every nesting variant is rejected now, matching
  libyaml ("did not find expected <document start>" / "did not find
  expected key"); a TAG-only continuation ("&a\n!!str v") stays legal
  (nightly run 38271222289, catch 78).
- **parse: a props-only line at a pair position rejects** — a line
  carrying only properties (anchor, tag, or both) where the engine
  expected a mapping key ("k: 1\n!!t: &b\n  n: m") was consumed as
  node props: the pair's key event vanished, the key's tag leaked onto
  the value node, and a long-tag value then lost its anchor at emit —
  the trailing alias emitted a dangling reference. libyaml rejects
  every such shape ("could not find expected ':' while scanning a
  simple key"); node-position props lines stay legal, including the
  first pair of a fresh map (nightly run 38275208009, catch 79).
- **parse: the flow floor covers the closer line on every route** —
  the JSON fast path's floor scan stopped at the flow's close, so the
  closer's own line escaped the suite-strict 9C9N floor whenever the
  span took the fast path, while short spans (chunk cuts, small
  documents) took the general kernel and rejected: whole vs chunked
  feeds disagreed over the same bytes ("k: {\n\n} \n---\nk2: v"
  parsed whole and failed chunked). The fast scan now covers the
  closer line. The key pre-scan also restores its line state with the
  cursor, so a multi-line flow's open event marks at the '{' on the
  slow route too (it marked at the pre-scan's far line, diverging
  from the fast path's mark) (nightly run 38280372447, catch 80).
- **parse: the '?' / ':' pair state resets at document boundaries** —
  doc 4's flow set q_key_pending and the '---' boundary left it set,
  so doc 5's bare ':' skipped its empty-key event: the whole-buffer
  stream lost a record the chunked feed (which rebuilds per cut)
  emitted — transport changed the event stream. The boundary clears
  the pair state now, and the bare ':' key event carries the ':' mark
  instead of a zeroed one (nightly run 38287390219, catch 81).
- **feed: the step scan holds block-scalar state across cuts** — the
  scan had no '>' / '|' notion: a cut inside an open fold changed the
  scalar's value or split it into phantom documents (">\n---\n"
  whole vs chunked), and fold content like "'* bullet" poisoned the
  scanner's quote tracking so later cuts landed mid-fold. The scan now
  consumes a fold's lines wholesale — they cannot poison quote, flow,
  or comment state — and cuts only after the reader's own fold-end
  rule: a dedented line that is not blank, not a comment, not a
  marker; truncated marker fragments wait for the next chunk (nightly
  run 38291177166, catch 82).
- **feed: doc-inline block scalars hold the cut too** — the scan's
  header check only fired at line starts, so "--- |" left the fold
  untracked: the fold's flow-looking content poisoned the scanner's
  quote/flow state and the split diverged. The marker arm opens the
  state now, and doc-inline folds follow the root rule — only a later
  marker ends them; col-0 lines are content (nightly run 38352718743,
  catch 83).
- **parse: node props cannot precede a second '?' on the same line** —
  "? &a ? b" opened the nested key map with the props dropped: the
  displaced anchor stayed defined in the nametab while never emitting,
  so a later alias emitted a dangling reference and the round-trip
  failed. libyaml: "mapping keys are not allowed in this context";
  the cross-line twin ("? &a\n  ? b") composes, and the props-less
  "? ? b" stays a legal nested key (nightly run 38355778230,
  catch 84).
- **feed: the step scan tracks quotes at word starts** — a quote byte
  after any indicator char opened phantom quote state, and plain
  content like "A!'- %YAML" carried it across the "--- |" fold
  marker: the fold went untracked and a later cut orphaned the
  "%YAML" directive from its "---", shifting the document-start mark
  by a line. Quotes now open only at a word start; a word that began
  with an indicator still admits a quoted node after it (nightly run
  38359507422, catch 85).
- **feed: flow separators re-arm the word-start quote rule** — the
  quote in "{\"O\" ..." sits adjacent to the '{' with no space, so
  the word-start rule (catch 85) left it untracked: the scan counted
  the ']' inside the quoted scalar as closing the flow and cut at a
  '---' the engine never saw outside the quote — the chunked feed died
  on an unterminated quoted scalar the whole parse had closed. ','
  ':' '[' ']' '{' '}' re-arm tok_start immediately (nightly run
  38366643713, catch 86).
- **emit: the explicit pair's value keeps its anchor** — the block
  map's explicit-key arm wrote "? key" then ": " and emitted the value
  directly, skipping the simple arm's props-tail + fresh-line path for
  a nested block collection: an anchored map value lost its anchor (or
  glued it to the first key, "&basename"), and a later alias emitted a
  dangling reference the re-parse rejected. The explicit form now
  mirrors the simple arm exactly (nightly run 38371134997, catch 87).
- **emit: the core shorthand's suffix must re-scan** — a core-URI
  tag whose suffix carries a byte outside the parse side's tag charset
  (ours terminates on ',' — the lenience vs libyaml's URI set)
  re-parsed truncated when emitted `!!<suffix>`, killing the
  round-trip (nightly run 38132505901, the verbatim tag with an
  interior ','). Those fall back to the verbatim `!<...>` wrap; clean
  suffixes keep the `!!` form.
- **emit: the core namespace rides the !! secondary handle** —
  `tag:yaml.org,2002:X` tags emit as `!!X` at every tag site (scalar,
  collection line-tail, root props), matching libyaml's default-handle
  behavior instead of the `!<tag:yaml.org,2002:X>` verbatim wrap. A
  node tagged with the verbatim shorthand "!!str" collides with a
  LOCAL tag of the same bytes and rode the catch-27 %21 escape — the
  binding's chevron-key dump emitted `!%21str '<<': *1` (a local tag)
  and the key re-loaded as a merge, breaking the round-trip that
  v0.6.32's `!!str '<<'` form carried. Hosts pass the full core URI;
  local bang-bearing tags keep the %21 escape.
- **parse: the flat-run's key honors plain-first** (#513) — the
  line-head bail listed '[' but not the plain-first law; a ']'
  sibling key parsed while the marshal rejected.
- **parse: \\U escapes stay under U+10FFFF** (#514) — an 8-digit
  escape can name unencodable code points; the nightly's
  "\\UA66DA66D" parsed and never re-parsed. Psych rejects above the
  boundary.
- **emit: flow plains carrying quotes take the quoted route** — a
  plain scalar with '"' or '\\' rode plain inside a flow map, where
  the bytes flip the scanner into quoted/escape mode ("unterminated
  quoted scalar"; nightly run 38107485738, `? !str a"\u1: -…`). The
  catch-25 flow gate covers them.
- **emit: a null flow-seq item rides '~'** — the item position
  emitted as nothing, so "[, x]" read as a leading comma and neither
  libyaml nor Psych re-parse it (nightly run 38106246351,
  `? - \n  - 2002-07-*2Yon`). Flow-map values keep the bare form
  (`{k: }` is valid).
- **parse: the repeat-alias memo resets per document** — anchors are
  per-document, but the alias memo (the merge-key fast path)
  answered doc 2's identical alias name from doc 1's memoized
  ordinal across the boundary: the whole-buffer parse resolved the
  alias while the chunked feed (which rebuilds the engine per cut)
  correctly rejected it — transport changed semantics. The memo
  invalidates at every anchor-table reset (nightly run 38266487549,
  catch 77).
- **parse: a sibling pair flushes the buffered explicit key** — a
  same-column plain-start pair line under an explicit key was
  consumed as the key's content, stranding the buffered "? &a" key
  inside it: the trailing alias emitted a dangling reference to the
  dropped anchor and the output never re-parsed. The continuation
  stops at such pairs (dashes keep the old path — a block seq is the
  key's content); the null value flushes, the pair proceeds, and the
  anchor emits (issue #570, nightly run 38261610465, catch 75).
- **emit: a leading BOM content key rides quoted** — the plain-safety
  predicate let a LEADING U+FEFF ride raw (a UTF-16 double-BOM
  transcodes to one as CONTENT); the re-parse consumed it as a
  document BOM and the key became empty, every round shedding it.
  Only its escape ("\uFEFF") is byte-stable; mid-token BOMs ride
  plain (nightly run 38258674337, catch 74).
- **emit: flow plains quote the separators** — a plain carrying ": "
  rode plain inside a flow-rendered pair, and the second ':' died at
  the completed-pair law ("{a: b : c}"); the gate gains ':' and its
  explicit-key twin '?' (nightly run 38255134508, catch 73).
- **emit: flow plains quote the anchor indicators** — a plain
  carrying '&' (or its alias twin '*') rode plain inside a
  flow-rendered explicit key, the '&' opened an anchor scan and the
  re-parse died ("did not find expected alphabetic or numeric
  character while scanning an anchor"). The flow-context DQ gate
  gains both bytes; the suite's M2N8 parse is untouched (nightly run
  38250474732, catch 72).
- **parse: a props-only line under an explicit key rejects** —
  continuing "? &a\n&F" absorbed the new props INTO the key and
  DROPPED the key's own anchor, so an alias referencing it emitted a
  dangling reference that never re-parsed. libyaml rejects the shape
  ("could not find expected ':' while scanning a simple key"); the
  props-only-line law now covers the explicit-key context too, and
  the legal explicit-key shapes ride (nightly run 38207965687,
  catch 71).
- **emit: a key's anchor counts toward the written-token law** — the
  estimate counted the key's tag (catch 63) but not its anchor;
  anchor names carry interior colons (Y2GN), so a ~1700-byte name
  rode a 2-byte key past the re-parse's 1024 law and the emission
  never re-parsed. Over-limit anchored keys now ride the explicit
  form (nightly run 38206305189, catch 70).
- **parse: a mid-token '#' is content in prop lines** — the props-line
  classifier ended the scan at any '#', so "&a ?#:" read as
  props-plus-comment (only_props) and failed, while libyaml parses it
  — and the emitter's own re-spelling of such keys was the form our
  parser rejected. '#' opens a comment only after a blank; mid-token
  it rides the plain (nightly run 38204470906, catch 69).
- **parse: a deep anchored pending rides the engine** — the fused
  runner's anchored-pending deferral only flushes its records at
  depth 1; a pending resolved into an anchored map BELOW the root
  frame dropped its pairs at EOF ("4:\n4: r\n5:\n &5\n 5: e" lost
  the anchored map and every round shed bytes). Deeper anchored
  pendings now bail to the engine, which resolves them; the depth-1
  anchored shape keeps the fused path (nightly run 38202382251,
  catch 68).
- **emit: a tagged EMPTY key counts its tag** — the written-token
  estimate early-returned on an empty key value before the tag arm,
  so a tagged empty key wrote ~2300 bytes of escaped tag as a simple
  key and died at the re-parse's 1024 law. The tag arm now runs for
  empty keys too; over-limit tagged keys ride the explicit form
  (nightly run 38200627503, catch 67).
- **emit: only single-line tagged block keys take the explicit form**
  — the clip-chomp twin of catch 64's key (":\n! |\n r\n:") keeps its
  trailing break, so the style chooser demotes it to quoted even in
  the explicit path (as_key): promoting it made round one explicit
  and round two simple — every round differed. Multiline tagged block
  keys now ride the simple form in both rounds, stably; single-line
  tagged ones keep the catch-64 explicit form (nightly run
  38181819580, catch 66).
- **parse: a dedented pair closes the child map's column** — after a
  pending key resolved into its child map, the frame kept the CHILD's
  content column: a dedented root pair closed the child's phantom map
  but the next more-indented line rode the stale frame as a phantom
  sibling — the runner accepted what the engine and libyaml reject
  ("mapping values are not allowed in this context") and parse and
  marshal disagreed. Off-column pairs on resolved frames now bail to
  the engine, which owns both the error and the legit dedent; the
  common `k:\n  nested: v` / dedent / same-column shapes ride the
  fused path unchanged (nightly run 38179552341, catch 65).
- **emit: a tagged block key rides the explicit form; explicit-key
  props emit once** — a TAGGED literal/folded key keeps the block
  form in the style chooser (the #290 law), so as a map key the value
  separator glued onto the block's content line (colons are data
  there): the re-parse read a different tree and every round shed
  bytes. Such keys now take the `? key` form, which keeps the block
  form — both rounds agree. Untagged/empty block styles demote to
  quoted/plain and ride simple stably (the suite's 5WE3 unchanged).
  Along the way: the explicit-key writer pre-emitted the key's props
  AND let the node re-emit them ("? !" + "! |-...") — the catch-56
  law, explicit-key side: the DQ-promoted path keeps its pre-emit,
  everything else carries tag+anchor inline (nightly run 38177622733,
  catch 64).
- **emit: a key's tag counts toward the written-token law** — the
  simple-key length check estimated the quoted key's written size but
  ignored the tag riding the same token; wr_tag_uri escapes every
  byte outside its URI set as %XX (even `_`), so a ~975-raw-byte tag
  wrote ~1030 and the emission died at the re-parse's 1024 law
  ("simple key exceeds 1024"). Over-limit tagged keys now ride the
  explicit form; short-tagged keys stay simple (nightly run
  38176016091, catch 63).
- **step: fused flow openers carry the general kernel's marks** —
  the fused flow walk (spans >= 24 bytes) emitted collection opens
  one column right of the general kernel, so a tiny document cut at
  a `---` boundary (general kernel) and the same bytes parsed whole
  (fused) disagreed on the open's column. The fused pass-2 now
  mirrors the general kernel exactly (nightly run 38173499680,
  catch 62 — fuzz_feed's whole-vs-chunked invariant).
- **parse: the flow pre-scan reads tokens, not bytes** — a quote
  opened a quoted scalar wherever it appeared, so a tag suffix or a
  plain carrying one (`[!'str b]`, `[a'b]`) died as "unterminated
  quoted scalar" before the real scan ran, and the emitted form of
  such documents never re-parsed. Quotes now open only at token
  starts (libyaml accepts both shapes; nightly run 38171171360,
  catch 61).
- **parse: anchors and aliases have no quoting in flow** — a quote
  byte in a flow anchor/alias name rejects (Psych: "did not find
  expected alphabetic or numeric character"). Block names keep
  riding quotes — the suite's W5VH anchor `&:@*!$"<foo>:` is
  untouched.
- **parse: a completed flow pair owns its value slot** — `[s: 2:]`,
  `[a: b: c]`, `[s: : v]` opened a second single-pair wrapper inside
  the first pair's value, unbalancing the event stream (the DOM then
  silently dropped the `2`). Every completed-pair-plus-`:` shape now
  rejects, matching libyaml; the legal `[a: b, c: d]` / `[s: , x]`
  shapes are untouched (nightly run 38168927841, catch 60).
- **emit: a block-styled collection nested in a flow member renders
  flow** — the writer honored the node's parsed block form even
  inside brackets, writing `- ` where only node content is legal.
  The catch-55 law, collection arm: `w->in_flow` rides the dispatch.
- **emit: root props emit once** — the root-collection props rode
  their own leading line AND the empty collection re-emitted them
  inline (`! &a\n! &a {}`), so every round shed a line (nightly run
  38167237445, `! &a\n  ? &e...`). The catch-56 law, root side: an
  empty propertied collection takes the inline empty-form.
- **emit: the local shorthand's suffix must re-scan** — the catch-47
  law, on the local arm: a `!suffix` tag whose suffix carries a byte
  the parse side's charset terminates on (ours: ',') wrote it raw,
  re-parsed truncated, and the round-trip died (nightly run
  38165476712, `!<!<<b,,,,,-,,,,ar> :yam`). Non-scannable suffixes
  fall to the verbatim `!<...>` wrap.
- **parse: a leading blank floors the literal block** — a blank
  line with MORE indent than the block's first content line floors it
  (libyaml's auto-detect takes the blanks' max; the shallower content
  breaks the mapping — Psych: "did not find expected key"); the
  runner accepted what the engine rejected, so parseable input failed
  to marshal (nightly run 38163867815, `a: |\n    \n  b`). The
  runner bails the shape; deeper blanks AFTER content stay legal
  content.
- **emit: a seq item's props emit once** — the dash's props arm
  wrote "- " plus the props separator's own space (a DOUBLE space),
  and the empty propertied collection re-emitted the props
  `emit_node` had already written — every round duplicated
  (`- &a\n  &a {}`) until the shape stabilized wrong (nightly run
  38161738783, `- &a\n  ? &e...`). The props arm's dash carries no
  trailing space; an empty propertied collection rides the compact
  form (`- &a {}`).
- **emit: block scalars never ride flow keys** — a flow-rendered
  explicit key's subtree carried LITERAL members (`yaml: |` + content
  lines inside what reads as a flow context) and the emission could
  never re-parse (nightly run 38154662070, `? !\n- name: ...`).
  BLOCK SCALARS ARE ILLEGAL IN FLOW: in flow context the style
  chooser answers DQ for literal/folded styles.
- **emit: C1 controls escape on output** — `\x99` decodes to
  U+0099: valid UTF-8 but NON-PRINTABLE, so the raw pair in the
  emission failed the re-parse's own printability gate (nightly run
  38151866348, an escape-heavy doc). The whole C1 range
  (U+0080..U+009F) rides the `\xNN` escape on output — Psych's form;
  the escape re-enters the document the same way it left.
- **parse: the implicit-null mark stays in bounds** — after a flow
  close scan the cursor could stand one past EOF, and the deferred
  key's implicit-null position mark read one byte out of bounds
  (fuzz_parse ASan, nightly run 38147359781, a 23-byte exact buffer).
  The mark clamps to the buffer end first; zero-copy means every
  read is bounds-checked.
- **emit: the simple-key law counts the written token** — the key
  predicates estimated the RAW span; a ~1000-raw key whose quoted
  render expands past 1024 (escapes) rode as a simple key and the
  emission could never re-parse (nightly run 38145696566, the
  `? licit k:[…]` family). One conservative rendered-span estimate
  (factor-4 per expandable byte) now feeds all four sites — the flow
  member loop, the block member loop, the flat-map specialist, and
  the quoted-explicit-key emission — and the promoted scalar key
  rides QUOTED so its interior ": " sequences cannot re-parse as
  separators.
- **parse: the lenient flow walk enforces the simple-key law** — the
  DOM fast path's lenient walk carried no >1024 key check (the record
  walk's yt_flow_scan has one); a flow doc with a long key wrapped and
  its YAML serialization could never re-parse. The wrap now defers to
  the engine's clean error; the tape API keeps accepting (strict JSON
  has no key-length limit).
- **emit: a long flow key rides the explicit form** — JSON has no
  key-length limit, so the strict-JSON route parses >1024-char keys —
  but the YAML simple-key law caps one-line keys at 1024 and the
  emitted flow form could never re-parse (nightly run 38143486175, a
  JSON flow doc with >1000-char keys). The flow map member writer
  rides the explicit `? key : ` form past the limit, libyaml's escape
  for long keys.
- **emit: flow-rendered plains quote the tag and quote indicators** —
  the explicit-key entry's inner map renders FLOW, and a plain
  carrying '!' (a tag indicator everywhere) or a lone '\'' (opens
  flow single-quote mode) rode plain inside "{}" — the emitted form
  died in tag/quote scanning and never re-parsed (nightly run
  38139449828, `- &a\n ? : a!%!!a\n ? : a'\n-\t `). The flow gate
  gains both bytes; the suite's M2N8 ("- ? : x" must parse) keeps its
  parse-side form.
- **parse: the literal block's pool buffer is block-sized** — the
  clip lives in the span (`flen = clen - tb`) but the copy paths write
  every measured line, so allocating the clipped length wrote `tb`
  bytes past the pool block whenever a literal ended with
  stripped-empty lines (nightly run 38137087116, ASan
  heap-buffer-overflow WRITE in the re-walk). The block allocates the
  measured size; the span stays clipped.
- **parse: a pending map's root resolution tracks the root column** —
  the same missing root bookkeeping the seq arm gained, in the
  pending->map resolution: a dedent past a map-valued root's column
  popped to a content-column-0 root and the document accepted the
  second root the engine rejects (nightly run 38134498534,
  `  e:\n   f: g\nh: i`). The catch-39 law's map-valued variant;
  col-0 root siblings and same-column maps parse unchanged.
- **parse: a pending seq's root resolution tracks the root column** —
  the unanchored resolution set the opened map's content column (the
  catch-41 fix) but never the root's bookkeeping the anchored branch
  carries; a dedent past a seq-valued root map's column popped to a
  content-column-0 root and the document SPLIT INTO PHANTOM DOCS
  (`---\nb\n---\nv`) while the engine rejected (nightly run
  38128905644, ` american:\n  - ...\nnational:`). The catch-39
  second-root law, seq-valued variant; col-0 root siblings and nested
  dedents parse unchanged.
- **parse: a pending "key:" over a seq frame rejects** — the pending
  arm lacked the kind check the valued arm carries: a bare ` bar:`
  after an indentless seq item pushed the pending key UNDER the
  sequence and it materialized as an ITEM while the engine rejected
  (nightly run 38127584324, `~foo:\n- 42\n bar:\n  - 45 e.`).
  libyaml: "mapping values are not allowed in this context". Same-column
  siblings and legal containers parse unchanged.
- **parse: an alias node is complete** — props or a second node
  after an alias on the same line (`<<: *base &bar`, `*x *x`, the
  glued `*x#y`) error in libyaml ("did not find expected key");
  the DOM route absorbed the stray props while the value drain
  rejected — parseable input failed to marshal (nightly run
  38123150843, a merge-key churn family). One follower law at the
  engine's alias scan and the runner's alias-value branch; alias keys
  (`? *x`), flow aliases, comments, and plain merges parse unchanged.
- **parse: an anchor name ends at a blank or flow indicator** — a
  glued '#' is junk on the name (libyaml: "did not find expected
  alphabetic or numeric character"); the engine accepted it MANGLED —
  anchor kept, the bytes after '#' dropped — while the runner kept a
  different shape, so parseable input failed to marshal (nightly run
  38121449804, `ow0: &f#o   `). One boundary check at all three anchor
  captures; a separated '#' stays a comment and the suite charset
  (W5VH's `&:@*!$"<foo>:`) parses unchanged.
- **parse: a block-scalar header tail is blank-or-comment** — the
  runner's bare-literal arm took the block and silently dropped
  trailing header bytes (`ext: | c* `) while the engine rejected —
  parseable input failed to marshal (nightly run 38119775008, a
  multi-doc literal family). The engine's header scan also accepted
  params after a separator and repeated digits (`| 12`, `|12`).
  libyaml's law lands at both sites: the indentation digit and chomp
  sign ride the indicator directly, one each; after them only blanks
  and an optional comment ("did not find expected comment or line
  break"). All legal header forms parse unchanged.
- **parse: a pending key's seq resolution tracks the content column**
  — the unanchored resolution never recorded the opened map's content
  column (the anchored branch did), so the off-column guard could not
  fire and a key line BETWEEN the resolved seq's column and the
  parent's fell through to a SILENTLY TRUNCATED document — lines
  dropped with a successful parse while the engine rejected (nightly
  run 38118641722, `ue:\n  - one\n woc:\n  - one\n wt o-  : thrt`,
  Psych: "did not find expected key"). The opened map now carries its
  pairs' column; between-column keys reject everywhere.
- **parse: an indented seq owns its column** — a block sequence
  indented past its parent map's key column rejects a non-dash line at
  that column (libyaml: "did not find expected '-' indicator"); the
  runner had popped the seq and silently DROPPED the remaining lines
  while the engine rejected — parseable input truncated to a smaller
  tree (nightly run 38117287882, `amer:\n  - j\n  n:\n  - Bksl,} --
  }`). Only the indentless seq — parent keys at the same column —
  closes on a key line there; dedents to the parent column and dash
  continuations parse unchanged.
- **parse: a dedent below the root's own column is a second root**
  — a line left of a root map opened past column 0 ends the root
  node; content there is a second root without `---`, which libyaml
  and Psych's load_stream reject ("did not find expected <document
  start>") and the 1.2 grammar forbids (Psych.load only appears to
  accept because it stops reading at the first document end). The
  runner's root-floor exemption rode col-0 pair lines flat while the
  engine nested them, so both the catch-31 shape (run 38105178194,
  ` i1ak: ?o\nfr: ?ff`) and its three-line sibling (run
  38115641999, ` k2: v2\nk1: v\n k2: v2`) split parse from marshal;
  both routes now reject, matching the seq variant's existing
  verdict. Same-column siblings, post-root markers, and comments
  parse unchanged; the suite has no case pinning the shape.
- **parse: chained same-column pendings keep the tree** — resolving
  a pending "a:" via a same-column sibling key ("a:\na:") corrupted
  the runner's frame mapping when the container carried valued pairs:
  the OPEN'd map swallowed the second key as the first pair's value
  ("a: a", nightly run 38110714729). The runner bails the shape to
  the engine, whose tree is byte-identical to the input.
- **emit: plain-prefix document markers quote** — a plain whose
  value STARTS with "--- " or "... " (plus blank) emitted plain and
  re-read as a document marker at line start (nightly run 38109609214,
  `--- --- >\nlfoo ]…`). The bare-marker law now covers prefix forms;
  libyaml quotes both.
- **emit: flow-context anchor names that cannot ride raw are refused**
  — anchors have no quoting syntax, and an indicator byte in a name
  can never re-parse in flow context (the nightly's `&"`, bang, and
  question veins — runs 38103630412, 38108543435, 38112005844,
  38113259764). Parse keeps the full suite charset (W5VH wants
  `&:@*!$"<foo>:` to parse; block-context emission round-trips
  byte-stably); serialize returns NULL for the flow forms rather than
  emitting output that corrupts.

- **parse: `%TAG` allows only a comment after the prefix** — the
  directive's prefix scan ran to end-of-line, so trailing content rode
  the prefix and the resolved tag could never re-parse (nightly run
  38102517540, `%TAG !m! !mymy 3…`). libyaml rejects the family ("did
  not find expected comment or line break").
- **parse: the flat-run's value honors plain-first** — the
  specialist's value arm bailed on indicators but skipped the
  plain-first law, so a ']' first byte rode as a plain value and
  parseable input failed to marshal (nightly run 38101390216,
  `k1: --a &p{\n0: ]`). The engine rejects the family; the flat-run
  now bails to it — #489's law, fifth site.
- **emit: a shorthand suffix escapes every bang** — a suffix-leading
  `!` re-parses as the secondary handle (`!!bz` -> 
  `tag:yaml.org,2002:bz`, run 38099113018) and an interior one as a
  NAMED-handle boundary (`!(4!bz` -> undefined handle, run
  38100254427): the parse side only tolerates the byte when
  escape-hidden. The shorthand emitter writes `%21` for each `!` in
  the suffix, mirroring the escape grammar the parse side decodes;
  verbatim `!<...>` URIs keep the raw form.
- **emit: flow scalars quote the flow indicators** — the style
  chooser's plain predicate is block-context law, so ']' rode plain
  INSIDE "{}" where it terminates the scalar; the emitted form
  re-parsed as a stray close (nightly run 38097567398,
  `?  :\t555555ta]`). Flow-context scalars carrying ',', '[', ']',
  '{' or '}' now take the quoted routes. (The fix also initializes
  the new in-flow flag at every emitter site — the field-by-field
  stack init had left it reading garbage.)
- **parse: a lone CR after a literal is a 1-byte break** — the
  runner's literal-block line advance treated any '\r' as CRLF and
  skipped 2 bytes, so the byte after a lone CR vanished as a phantom
  line break and a trailing key-less line was silently dropped instead
  of rejected (nightly run 38096448601, `2: |\n  h!\ro`; parseable
  input then failed to marshal). Psych: "could not find expected ':'".
- **parse: tag URI escapes need two hex digits** — a stray '%' rode
  the tag raw while only the well-formed escape decoded; the stored
  tag re-encoded to a form that parsed differently (nightly run
  38095309044, `!e%2%21bz`). libyaml rejects the family at the tag
  scan ("did not find URI escaped octet") — truncated, non-hex, and
  verbatim forms included.
- **parse: tag suffixes scan the URI set** — `!>` and friends
  parsed as tags whose serialization could never re-parse (nightly
  run 38057695714, `!>\r?\r\r:\r`). libyaml scans tag suffixes over
  its URI charset and rejects the stray byte as the follower ("did
  not find expected whitespace or line break while scanning a tag").
- **parse: pend props stay off the first key** — a props-only line
  above a mapping attaches its tag/anchor to the mapping, but the
  quoted- and flow-key promotions emitted the key carrying the merged
  props; re-parse produced a tagged first key and the serialization
  never re-stabilized (nightly run 38092896626, `!\r!\r?\r?\r:\r\r:\r`).
  Same-line props stay on the key — the Psych law in both directions.
- **emit: root block props carry their separator** — a tagged,
  anchored root collection emitted `!<...>&anchor` with no space;
  libyaml requires blank or break after a tag, so the anchor-first
  9KAX documents could not re-parse.

## [0.6.32] - 2026-10-10
### Fixed
The nightly fuzz hunt's second wave — five more engine defects, each
Psych-parity-verified and pinned (crashers in the regression corpus).
- **emit: tag bytes re-encode as %XX** (#501) — the parse side decodes
  tag URI escapes per the spec; the emitter now re-encodes unsafe
  bytes (a decoded newline emitted raw drifted every round).
- **parse: an empty-key ':' line is never a pending value** (#502) —
  libyaml rejects the family; the following-lines arm now does too.
- **parse: the runner's seq item honors plain-first** (#503) — a
  plain item starting with ']' parsed but never marshaled.
- **emit: the U+FFFE/U+FFFF noncharacters ride their escapes** (#505)
  — raw, the printable validator rejects them on re-parse.
- **marshal: the value drains share the parse entry's encoding
  gate** (#506) — UTF-16/32 input marshaled raw bytes; parseable
  input failed to marshal. One fix, three consumers (marshal, visit,
  drain).
- **parse: a line-end dash is an indicator** (#508) — the runner's
  nested-dash detection required a following blank byte, so a
  line-ending '-' became the plain scalar "-" and the emitted form
  re-parsed differently than its input.
- **parse: the document-end marker owns its line** (#509) — the fused
  runners had no marker law; a '...' line carrying content was
  swallowed as a plain pair while the engine and Psych reject.

## [0.6.31] - 2026-10-09
### Fixed
The resurrected nightly fuzz harness's first full hunt — nine engine
defects, each Psych-parity-verified and pinned (every crasher joins
the `fuzz-parse-regression` corpus the suite re-executes).
- **parse: the flow-node read after props hit EOF** (#488) — an
  exact-buffer heap overread (`[!<<
!]`, 8 bytes).
- **parse: the fused runner rejects indicator-first plain scalars**
  (#489) — `u: }` parsed but never marshaled.
- **parse: an anchor needs a name** (#491) — the empty form was
  accepted at four sites; serialize then emitted an empty document.
- **parse: a verbatim tag closes `>` on the same line** (#492) —
  unterminated/empty/spaced forms accepted with drifting output.
- **emit: synthesized flow nulls are plain** (#493) — value-less flow
  entries round-tripped as `""`, drifting per serialize/re-parse round.
- **emit: U+0085 (NEL) escapes as `\N`** (#494, #495) — it is a YAML
  break; raw it folded on re-parse. The law matches the UTF-8 pair C2
  85 only — a lone 0x85 is a continuation byte.
- **emit: U+FEFF escapes as `\uFEFF`** (#496) — raw it IS the BOM and
  re-parse strips it.
- **emit: LS/PS escape as `\L`/`\P`** (#497) — the break set complete.
- **parse: the props/value-line recursion is capped at max_depth**
  (#498) — the frame-less chain stack-overflowed; it now rejects with
  `YEPTRIS_ERROR_DEPTH`.
### CI
- fuzz-nightly: the valgrind corpus path + the gtest compiler pin
  (#486); the fuzzer main links only into the harnesses (#487).

## [0.6.30] - 2026-10-07
### Performance
- **emit: the flat plain member specialist (#352)** — a strictly
  narrower fast path at the top of `emit_block_map`'s member loop
  (plain unpropertied key + plain unpropertied scalar value, proven by
  the exact predicates `sc_route` consults); anything else falls
  through untouched, so byte parity holds by construction. serialbench
  medium serialize-only -11.4%; gen table: wide-mapping +16.5%,
  scalar-heavy +5.3%, json-users +2.7%.
- **emit: fused flat-member write + lazy nametab + shrink guard
  (#352)** — the specialist's whole member writes with one capacity
  reservation and raw memcpy (plain-safe spans carry no breaks, so the
  col scan is arithmetic); the emitter's canonical-names table
  initializes only for canonical runs; the shrink-to-fit realloc is
  skipped when the waste is under cap/8. Cumulative on the gen table:
  wide-mapping +34%, scalar-heavy +20%, block-heavy +14%; serialbench
  serialize-only 1.38x.
### Fixed
- **cmake: install survives `YEPTRIS_BUILD_STATIC=OFF` — and installs
  the shared lib** — `install(TARGETS yeptris_static)` was
  unconditional while the target only exists under the flag (and
  nothing joined the export set with static off, so `install(EXPORT)`
  failed too; `yeptris_shared` was never installed at all). Found by
  yeptris-rs's CI.

## [0.6.29] - 2026-10-04
### Fixed
- **parse: a block-scalar header at the key's column is the pending
  value** — libyaml never validates a `|` / `>` header's own column
  (only the content must out-indent the parent block), but the
  engine's following-lines scan granted that same-column continuation
  only to flow nodes (`k:
[a]`). `prefatory-content:
|
  text` now
  parses as the pending key's literal value instead of failing with
  an unexpected-state error at the header — the metanorma-cli
  collection fixture shape (yeptris-ruby#258). A stray header after a
  completed pair still rejects, matching psych on both sides.

### Changed
- **release: gems belong to the binding's train** — the engine's
  release flow no longer pushes any gem variant (the retired
  trusted-publisher entry made every remaining gem step fail); it
  keeps the version cut, the tag + GitHub Release, the native asset,
  and now *reports* the lockstep binding tag instead of gating on it.

## [0.6.28] - 2026-09-30
### Fixed
- **parse: the fused runner bails on three anchored shapes it mishandled**
  — an anchored key whose "content" sits at the key's own column (the
  value is null; the line is a sibling) became a nested map; an
  indentless anchored sequence lost every following sibling; and an
  alias to an anchor whose container never opened (null-valued or
  still pending) corrupted the tape. All three now fall back to the
  engine route, byte-exact with it (the runner's own contract: any
  line the loop does not own bails). Found by the binding's Psych
  merge-key port (yeptris-ruby's CI gates the lockstep tag); the C
  suite had no coverage for these shapes.


## [0.6.27] - 2026-09-30
### Changed
- **parse: the fused block runner owns every block-family shape** —
  quoted values, literal block scalars, and anchored containers join
  the owned set, and chained same-column pendings no longer leak a
  frame per record (the 256-frame guard at ~254 documents was
  block-heavy's silent bail). block-heavy, scalar-heavy, and
  anchor-heavy now run the fused SWAR runner end-to-end
  (block-heavy ~2.4x, scalar-heavy ~4x, anchor-heavy 2.8x their eager
  routes); block-scalar content rides a per-run pool transferred to
  the document, and the first pair inside an anchored container bakes
  the engine's tag classification via the threaded resolver.
  Byte-exact tape differential over all five block-family corpora and
  the yaml-test-suite inputs.
- **parse: the fused block runner owns nested block YAML** — deep
  nesting, indentless/nested sequences, sibling maps after child
  closes, and null-resolved pending keys now run the fused SWAR
  runner instead of falling back to the engine. The frame model
  mirrors the engine's tape emission exactly (strict dedents,
  per-column phantom content maps, null scalars for same-column
  siblings, the repeat-alias memo). Deep-nesting parses 3.09x the
  eager route (1078 MB/s DOM); wide-mapping holds 2.06x; byte-exact
  tape differential over the yaml-test-suite inputs.
- **parse: the fused block runner owns monomorphic block documents**
  — one SWAR sweep per line with inline classification writes the
  packed tape directly, no engine pass; any line the loop does not
  own bails to the engine route unchanged (restart fallback).
  Wide-mapping parses at 2.06x (598 vs 290 MB/s); other shapes ride
  the fallback until the column model widens coverage (#79 session 3).

## [0.6.26] - 2026-09-27
### Fixed
- **parse: NEL/LS/PS documents no longer dangle** — the one-shot
  `yeptris_parse` normalized Unicode breaks (NEL, LS, PS) into an
  ENGINE-owned buffer that dies at engine teardown, so every document
  containing one returned views into freed memory (garbage values or
  segfault on access); no test had ever parsed a real NEL document.
  The copy now rides the document's `transcoded` ownership before the
  engine runs; the streaming feeds keep the engine's per-chunk copy.
### Changed
- **parse: the Unicode-break probe rides the SIMD stopset kernel** —
  the whole-document NEL/LS/PS pre-pass walked every byte with a
  two-prefix check (~6 compares/byte); it now zero-detects the two
  lead bytes per 8-byte word via `stopset_find` and byte-checks only
  candidates. Block-heavy +13%, scalar-heavy +31% (order-alternated
  DOM-lane medians). The lead-byte stopset literal is pinned by
  `Parse.NormBreakLeadsMatchRuntimeBuild`.

## [0.6.25] - 2026-09-26
### Fixed
- **recorder: the compat schema's engine runs floor-free** — the
  recorder's engine creation set only the resolver, so every consumer
  of `yeptris_recorder_new_ex(COMPAT)` + feed (the py safe_load path)
  kept the flow indent floor #431 had removed everywhere else and
  rejected the multi-line flows libyaml accepts; caught by the PyPI
  clean-venv verification of the 0.6.24.1 wheel (#431 follow-up).

## [0.6.24] - 2026-09-26
### Fixed
- **parse: the 11-COMPAT grammar accepts multi-line flows libyaml
  accepts** — the block-level flow indent floor (9C9N, VJP3) rejected
  real-world locale files whose flow mappings close at their parent's
  column ("k: {\n  a: b\n}"); libyaml/Psych parse those, the floor is
  a yaml-test-suite rule the strict engine keeps. The floor now stays
  on for the default 1.2 engine only; every 11-COMPAT parse entry
  (yeptris_parse_ex, the value drains, the marshal fast path) runs
  floor-free (metanorma-standoc CI breaker, isodoc i18n-en.yaml).

## [0.6.23] - 2026-09-26
### Fixed
- **emit: parsed scalars `---`/`...` emitted plain** — the '-'/'?' head
  case early-returned past the document-marker check; libyaml/Psych
  quote the bare markers, and the head now falls through to the rule
  that owns them (#352).

### Changed
- **json parse: the member cycle's compact fast paths + the SWAR digit
  run** — the walk's whitespace-skip loop topped the arm64 profile (~8
  instructions per member even when it exits on byte one; compact JSON
  makes every exit byte one), the per-byte digit run followed: the
  map/seq cycles now dispatch value `,` key / key `:` value / value `,`
  value directly through the byte LWS would land on (the general form
  remains for whitespace-bearing input), and the initial digit run
  scans SWAR words. Same-host calibrated reads: json-doc flat at
  parity, json-users 0.73x to 0.90x (#342).
- **engine: the pair path's value event assembles only on the
  fallback** — the whole-line fast path filled the ~100-byte event even
  when the sink took the block value and ignored it (every
  default-route consumer); the fallback rebuilds it field-for-field
  with the pair facts taken before the fold (item 79 seam).
- **emit: the plain-safety interior scan runs in SWAR words** — five
  zero-detects per 8 bytes plus shifted-mask adjacency with boundary
  carries replace the per-byte branch ladder (22-35% of emit);
  differential-pinned against a reference loop over adversarial
  alphabets — the pin caught two mask bugs and the marker bug before
  they shipped (#352).
## [0.6.22] - 2026-09-25
### Fixed
- **bench: the simdjson referee lane parses a pre-built padded string** —
  the raw-pointer parse overload's `realloc_if_needed` copies the whole
  input on every timed parse (a tax simdjson's own harness and the
  serialbench fixture runner don't pay), flattering every head-to-head
  ratio the table printed: the "parity band" was partly instrument tax.
  Calibrated local read: json-doc 1.07x, json-users 0.73x (#342, #420).
- **json parse: the strict number arm's digits-only flag clears only on a
  consumed byte** — the flag was cleared when *inspecting* the run's
  terminator (comma, bracket, newline), routing every delimiter-terminated
  integer into the strict single-pass validator (~15% of json-doc parse
  re-validating runs the cheap path already proved). CI referee: json-doc
  at the simdjson parity band (0.97x ubuntu / 1.05x macos interleaved
  medians, from 0.88x) (#342).

### Changed
- **json parse: the string arm settles inline to 32 bytes** — four clean
  SWAR words before the `quote_scan` kernel: the 9-31-byte clean strings
  (the email class; ~100k per parse on the users corpus) no longer pay
  the kernel call plus a redundant content re-sweep, and the strict
  number validator is always_inline (its out-of-line call cost more than
  the 3-8-byte spans it checks). Round 1 is untouched — sub-8-byte
  strings pay zero (#342).

## [0.6.21] - 2026-09-24
### Fixed
- **dom: the #377 child-index cache reads ride the mutex** — the hit path read
  the cache unlocked while a concurrent builder published it under midx.mu (a
  schedule-dependent TSAN race on read-only sharing, present since 0.6.17;
  caught by the Threads.ReadOnlySharing leg) (#414).

### Changed
- **json parse: the secondary passes are gone** — numbers validate in-walk at
  record time (the settle pass deleted; digits-only spans settle with a
  leading-zero check, dot/exp spans through a single-pass strict validator),
  the nametab pre-pass no longer runs in JSON mode, and the entry's
  printability/tab pre-passes are replaced by the walk's own byte guarantees
  (clean_only: tabs and non-ASCII reject in-walk; the strict sequence owns
  the pinned precedences and re-gates only when taken). CI referee:
  json-users at the simdjson parity band (0.95-1.01x); the lenient tape
  entry's deferred number contract is unchanged (#342, #410/#411/#412).

## [0.6.20] - 2026-09-24
### Fixed
- **emit: the writer's grow trigger left no terminator slot** — a serialized size landing
  exactly on the buffer capacity wrote the closing NUL one byte past the allocation
  (the next heap block's header). Corruption deterministic, detection dependent on
  allocation history: the multi-year "abort under load, position varies with GC timing"
  flake (yeptris-ruby#182). The writer now reserves the terminator slot; the
  boundary-sweep regression gate fires AddressSanitizer at the exact site when
  reverted (#405).
- lazy YAML-tape route: the parse-only document no longer allocates (and leaks) the
  eager sink's DOM; MSVC C4701 zero-init (#404 follow-up).

### Changed
- **the packed YAML record tape is the DEFAULT YAML parse route (#378 slice 2)**:
  `yeptris_parse` records the engine's committed ops into 8-byte words and the
  tree materializes on first access — parse-only workloads never build nodes.
  CI referee (parse-only, interleaved medians): tape/eager 1.06-1.52x on the four
  referee shapes, both ISA tables. The tree materialization is single-shot under
  a per-document mutex (the read-only-sharing contract holds); the eager form
  stays reachable internally for the A/B. Defects the flip exposed and fixed:
  the flow `max_depth` now rides the tape, a failed materialization answers 0
  from `yeptris_document_count` instead of dereferencing, and the marshal fast
  path routes through the lazy choke point.

### Added
- **the packed YAML record tape (#378 slice 1)**: a recording sink over the engine's
  seven committed-op callbacks packs the stream into 8-byte words; replay decodes
  into the same builder functions. Differential-gated (30-shape inline corpus +
  the 67-file psych-pure sweep + compat-11 + error parity), behind the internal
  `yeptris_parse_ytape_ex` seam — the default route is unchanged (#404).

### Changed
- **json parse: the fused lenient walk's map/seq member cycles** replace the
  per-token state machine (#402); the three string arms collapse into one
  SWAR/quote_scan fast path with the escape kernel as fallback, and pure-integer
  spans settle without the number kernel (#403). CI referee: json-doc 0.77x ->
  0.93x simdjson, json-users 0.64x -> 0.88x (#342).

## [0.6.19] - 2026-09-23
### Added
- libyaml-parity event end marks (#179): `yep_event`, `YeptrisEvent`, and
  the recorder's `YeptrisEventRecord` (36 -> 44 bytes) carry
  `end_line`/`end_col` with libyaml's exact end_mark semantics — every
  event family (stream, document with directive-inclusive starts and
  `---`/`...` spans, block and flow collections, plain/quoted/
  literal/folded/multiline scalars, keys, aliases, deferred nulls)
  matches stdlib psych's `event_location` marks byte-for-byte, pinned by
  the psych-frozen `EventMarks` gate over the 13-fixture corpus
  (`scripts/marks_oracle` regenerates it). The bindings mirror the
  record layout in this lockstep window.

## [0.6.18] - 2026-09-22
### Fixed
- The emit's 128-bit float tier (`yep_u128`) compiles for any compiler:
  the portable lo/hi-limbs fallback serves 32-bit GCC (the armv7
  platform gems), with the limb math pinned by the forced-tier
  `FloatLimbs.*` tests on 64-bit hosts.

## [0.6.17] - 2026-09-22
### Added
- The CBOR decode sink seam (#157): the grammar loop drives a
  `yep_cbor_sink` (text/int/float/str/bytes/open/close); the DOM
  builder is the default sink and the native bindings' VALUE builder
  rides the same grammar — decode to Ruby objects in one pass with no
  DOM intermediate and no per-number text round trip.
### Changed
- The emit front's double-quote and plain-scalar routing is
  run-batched (#352): the writer batches by run instead of
  re-deriving per scalar.
- CBOR map keys intern in the native walk (#157): repeated keys share
  one VALUE per document.
### Fixed
- The qemu platform-gem cells (#376): the per-cell setup now reaches
  the container, the debian cells install their packages, the s390x
  cell pins ruby:3.2-bookworm (3.3+ dropped the variant), and the
  setup string is parsed as shell (env expansion never re-parses
  metacharacters). ppc64le and s390x gems shipped in the 0.6.16.1
  binding release.
- The `yeptris_desc_node` layout gate is arch-relative
  (`sizeof(const char*) + 16`): 32-bit ARM builds again (armv7 gems
  follow the next binding release).

## [0.6.16] - 2026-09-21
### Added
- The lazy child-index (#377): `yeptris_node_seq_at` and
  `yeptris_node_map_at` are O(1) after the first call (the last
  indexed container's child ids cache under the dom's lazy-init
  mutex; invalidated by every link/unlink). The bulk drain
  (`yeptris_node_children`) fixed the bindings' loops; this fixes
  the accessor itself. Differential-tested against mutation.
- The qemu platform-gem matrix (#376): arm-linux, arm-linux-musl,
  ppc64le-linux, s390x-linux — the family's 14-gem target. The
  teptris-ruby recipe (binfmt + the arch ruby image, the same
  build-platform-gem.sh script the musl leg rides, smoke inside
  each container).
- The `index-rows` benchmark corpus (#179d): a top-level block
  sequence of symbol-keyed maps — the relaton index shape that hid
  the materialization cliff.
### Fixed
- The qemu cells use the right package manager per image (#376
  follow-up): the `||` fallback (apt-get || apk) broke on emulated
  Debian — each matrix cell now carries its own setup command.

## [0.6.15] - 2026-09-21
### Fixed
- `yeptris_marshal_node` bails (`YEPTRIS_ERROR_UNSUPPORTED`) when any
  node in the subtree carries an explicit tag: the value records hold
  the resolver's verdict, not the source's tag, so `!ruby/object`
  mappings marshaled as plain hashes with the tag silently dropped.
  Load-bearing for yeptris-ruby#178's `unsafe_load` fast path (the
  11 MB relaton index materializes in 0.71 s end to end — 4× stdlib).

## [0.6.14] - 2026-09-21
### Added
- Lazy DOM materialization (#342 slice 2): `yeptris_parse_json`'s
  gate-clean route rides the fused lenient walk (one pass, records
  only) with the deferred number grammar settled inline, and the tape
  travels on the document — `dom_from_tape` builds the tree on first
  access through the new internal `yep_doc_dom` choke point, so
  parse-only workloads never pay the DOM lane. Tab-carrying buffers
  and scalar roots keep the eager routes (pinned behavior).
- `yeptris_node_children` (yeptris-ruby#168): the bulk child drain —
  one walk fills a handle buffer (sequence elements in order; mapping
  key,value pairs interleaved), returning the total count. The
  per-index `seq_at`/`map_at` walks are O(i) sibling hops each; the
  bindings' per-element loops made an 80k-row sequence quadratic (the
  relaton index parsed in 238-328 s where stdlib takes ~3 s).

### Fixed
- `dom_from_tape`: restored the seven cleanup-eaten guard bodies
  (args, columns, depth cap, open/place, close underflow, arena tail)
  — load-bearing now that the materializer is the lazy path.
- Every document constructor now initializes the new `lazy_tape`
  field (build.c already memset; the three field-by-field ctors set
  it): a garbage word made `yeptris_document_free` free an
  indeterminate pointer — glibc's reused pages segfaulted every CBOR
  leg on Linux/Windows while macOS's zeroed fresh pages hid it.

## [0.6.13] - 2026-09-20
### Added
- The schema-descriptor API's declared headroom, executed (#238):
  `YEP_ST_ANY` materializes the resolver's verdict as one
  YeptrisValue record per value (kind/tag from the node, the
  borrowed span, converted numeric payloads; under compat the DOM's
  own resolver carries the schema — it types the nodes the verdict
  reads), and `YEP_SF_FIRST_WINS` bounds a duplicate key's column
  to its first match within one mapping instance (sequence
  elements still accumulate across theirs).
- `dom_from_tape` (#342 slice 1): builds the node tree from a
  parsed tape's records — the lazy-DOM seam. Reads the columns
  (the one representation valid on every route), classifies the
  lenient route's deferred number spans at materialization, and is
  differential-tested structurally identical to the direct fused
  parse.

## [0.6.12] - 2026-09-20
### Added
- The `!binary` contract (yeptris-ruby#168): the compat schema
  resolves psych's LOCAL `!binary` shorthand to the core BINARY tag
  id (stdlib to_ruby accepts both spellings; core12 keeps it
  CUSTOM); a tagged literal-styled scalar keeps the block form even
  single-line (`!binary |-` + one base64 line — psych's exact
  bytes); the bulk-build ABI admits exactly one trailing TAG on a
  closed scalar root; the marshal fast path returns UNSUPPORTED for
  binary-tagged scalars so hosts decode.
- The lazy document-open path (yeptris-ruby#157's fixed-cost cut):
  the pool's first block materializes on first use, and the handle
  pool defers to the first handle acquisition — every access rides
  the dom's lazy-init mutex (the map-index discipline; the first
  cut raced TSAN's read-only-sharing test).

## [0.6.11] - 2026-09-20
### Changed
- `yeptris_serialize` / `yeptris_serialize_ex` emit in ONE wet pass
  into a growable buffer (estimate: input size + 64, doubling on
  demand, shrink-to-fit at the end) — the exact-sizing dry pass
  measured 0.54-0.73x of the one-pass route on CI-fresh runners and
  is now exclusive to `yeptris_serialize_into` (whose contract needs
  the byte count before writing). serialize gained ~1.8x, judged by
  the emit-split table's within-run 2p/1p arm (#352).
- The dry pass records per-scalar emission routes (long scalars only,
  32B threshold) that the wet pass consumes — `serialize_into`'s
  remaining two-pass route halves its derive work.
- The benchmark artifact's emit table carries a third interleaved
  arm: the streaming route (one derive-while-writing pass) — the
  within-run referee for pass-count questions.

## [0.6.10] - 2026-09-19
### Changed
- The strict-JSON fused DOM builder resolves literal tags by first
  byte (the walker already validated the span is exactly
  true/false/null) instead of dispatching the resolver — no indirect
  call per literal token.
- The benchmark artifact carries two referee tables: the simdjson
  head-to-head now covers `json-users` (serialbench's medium.json
  shape — the attribution corpus for the DOM-build lane, #342), and
  an emit kernel split (serialize vs serialize_into, the allocation
  share — #352) rides every push.

## [0.6.9] - 2026-09-19
### Changed
- The lenient JSON tape's primary storage is ONE interleaved 8-byte
  record per token (off:u32 | len:u23 | kind:u8; spans over 16 MiB
  carry a CONT extension) instead of three column stores — item 07's
  slice 1 toward simdjson-class walk cost.
- The kinds/offs/lens columns stay as the compatibility ABI,
  materialized lazily: `yeptris_tape_columns` fills them on first
  touch; `yeptris_tape_convert`, `yeptris_tape_plan_walk`, tape-diff
  and the unit fixtures route through it. The strict route remains
  column-primary (unchanged).

## [0.6.8] - 2026-09-19
### Fixed
- The canonical CBOR encoder's sizing pass no longer holds a map
  pointer across child recursion: a nested map's canonical prep
  reallocates the map table, and the dangling pointer made the next
  pair read walk freed memory — a heap-use-after-free that segfaulted
  `yeptris_cbor_encode` on documents from 11 canonical-sorted map
  entries (the 16->32 growth crossing; yeptris-ruby#152). The map
  INDEX is held and the pointer re-derived for every read. ASAN-verified
  at 11/15/100/1000 items; insertion-order encoding was unaffected.

## [0.6.7] - 2026-09-18
### Fixed
- The marshal encoder's magnitude bookkeeping is width-safe: the
  byte-width and bignum limb-count loops read the magnitude through
  `unsigned long`, which is 32-bit on Windows (LLP64) — every integer
  with |v| >= 2^32 truncated to its low 32 bits through the Ruby
  binding's marshal load path (1234567890123456789 materialized as
  2112454933; caught by the windows-11-arm spec leg). The negative
  payload's borrow is now masked at k == 8, removing the `1UL << 64`
  shift UB reachable on LP64.

## [0.6.6] - 2026-09-18
### Added
- `yeptris_free` — the allocator-matching free for the serialize*
  family's owned buffers (#318's Windows root cause): the host's
  libc `free` is a different CRT instance than the DLL's on Windows,
  and FFI cannot resolve bare `free` against the DLL's exports at
  all. NULL is a no-op.

## [0.6.5] - 2026-09-18
### Changed
- The cycle fast path (TODO.restructure/79 carve 1a): lines that
  continue the top frame — a map key at the frame's column, a seq
  dash — with no pending state call the classified arm directly
  from the line loop, skipping the node prologue and dispatch tail
  on the monomorphic cycle lines. block-heavy parses run 8-12%
  faster (interleaved A/B, ledgered in benchmarks/PERF-LEDGER.md);
  the event streams are byte-identical (the differential and
  yaml-test-suite gates pin them).

## [0.6.4] - 2026-09-17
### Added
- The plan walk's segmented paths and the YAML/DOM leg (#293 /
  TODO.restructure/87, slice three): the spec's `path` accepts an
  ARRAY of strings (walked mapping by mapping to the rows container,
  both legs), and `yeptris_document_plan_walk` applies the same
  compiled plan to a `yeptris_parse` document — block YAML hydrates
  to typed columns with no intermediate tree. Typed extraction rides
  the parse-time tag ids (the typing SSOT) plus the number kernels;
  string columns expose `(ptr,len)` views via
  `yeptris_plan_result_strs` (the result borrows the document);
  aliases resolve to their targets; map-root containers yield their
  mapping values as rows.

## [0.6.3] - 2026-09-17
### Added
- The compiled plan walk (#293 / TODO.restructure/87, slice one):
  `yeptris_plan_compile` builds a plan from a strict-JSON descriptor
  spec (`{"kind":"seq|map","path":..., "children":[{"name":...,
  "kind":"int|float|str|bool"}...]}`), and `yeptris_tape_plan_walk`
  applies it to a parsed JSON tape in ONE C pass producing typed
  COLUMNAR output (per-column int64/double/span arrays + null
  bitmaps) — no host tree, no per-node dispatch. Slice one covers
  the lutaml-model shape (a root collection of scalar-leaf rows);
  nested paths, the YAML leg, and partial descriptors ride the
  board item. Works over both the strict and lenient tapes.

## [0.6.2] - 2026-09-17
### Added
- `YEPTRIS_BUILD_TAG` — the bulk build API can tag the last-placed
  node (tag bytes in the blob). The bulk ABI otherwise could not
  express tags; #300's nil mapping keys need Psych's explicit `!`
  form (`! ''`). Pinned by BulkBuild.TagAppliesToLastPlacedNode.

## [0.6.1] - 2026-09-17
### Fixed
- libyaml's null rendering and block-scalar indicators (#290, the C
  half of the six remaining Psych byte-parity families): an empty
  plain scalar value rides the key bare (`k:`), a null sequence item
  is a bare dash, an explicitly-empty block scalar stays `""` (a bare
  empty would re-read as null — corpus-pinned), and literal blocks
  emit the explicit indent only when the first body line starts with
  a space or is blank, with blank interior lines unpadded.

## [0.6.0] - 2026-09-16
### Added
- `yeptris_parse_json_tape_lenient` — the simdjson deferred contract,
  opt-in: structural checks stay at parse while number grammar defers
  to `yeptris_tape_convert` (charset runs record as `YEP_T_NUM`;
  "1.2.3" rejects at drain where simdjson's number parse would).
  Tab whitespace is legal (RFC 8259); non-ASCII takes the strict
  route; on everything strict accepts the tapes are structurally
  identical. json-doc: 528 -> 705-765 MB/s vs simdjson's ~1041
  (0.70-0.73x; PERF-LEDGER verdict eight records the endgame design
  for the last stretch). `yeptris_tape_convert` now requires
  full-span consumption — a latent partial-span acceptance the
  lenient differential caught (`1]`), pinned.
### Performance
- The lenient walk carries a SWAR close-quote settle (one u64 window
  for 1-8 byte strings) and colon/comma peek fusion on the byte walk.
  The bench matrix gains the tape_lenient leg on the simdjson
  head-to-head.

## [0.5.2] - 2026-09-16
### Fixed
- Empty built collections ride the key line flow (`k: []` / `k: {}`)
  — the 0.5.1 seq-indent change had sent an empty built sequence down
  the next-line path, emitting a bare `[]` at the mapping's own
  column, which is invalid YAML (pyyaml rejects it; caught by
  yeptris-py's random-document differential). Non-empty sequences
  keep the key-column indent.

## [0.5.1] - 2026-09-16
### Fixed
- libyaml/Psych dump parity (yeptris-ruby#95, PR #284): a plain-styled
  scalar that cannot ride plain but needs no escapes (digit-leading
  strings like `5014`, indicator leads) now single-quotes — libyaml's
  choose_scalar_style; double quotes stay reserved for text that
  actually requires escapes. A block sequence nested under a mapping
  key now indents at the key's column (`a:\n- 1\n- 2\n`) as libyaml
  does; mappings still indent +2. With the Ruby binding's
  explicit_doc_start wiring, `Yeptris::Psych.dump` is byte-identical
  to stdlib Psych for the relaton matrix in that issue.

## [0.5.0] - 2026-09-16
### Added
- `yeptris_emit_options.explicit_doc_start` — libyaml's
  implicit=false parity: a nonzero value puts the leading `---`
  marker on every document, with scalar roots riding the marker line
  (`--- 42`) exactly as Psych/libyaml do. Size-versioned (callers
  compiled against the older struct keep the implicit single-document
  behavior — guarded by offsetof+size, pinned by a test). Default
  unchanged. The four JSON serializer entries zero it explicitly.
### Performance
- The `on_scalar` direct sink path (TODO.restructure/79 slice one):
  `emit_now` routes scalar events straight to the DOM's node build
  when the sink provides the hook — no event-struct fill through the
  switch, no indirect dispatch. The engine still resolves tags (one
  typing SSOT); recorder/values sinks keep the unchanged event path.
  Neutral on block-heavy under load (the hot pairs already bypass
  events via `on_block_pair`); the groundwork for the loop fusion.

## [0.4.0] - 2026-09-16
### Changed
- **BREAKING — tape v2 records** (TODO.restructure/85, the verdict
  six/seven campaign): `yeptris_json_tape` records are THREE columns
  (kinds/offs/lens — 9 bytes per record, was 20; the vals column is
  gone). `YEP_T_TRUE`/`YEP_T_FALSE` replace `YEP_T_BOOL` (the kind IS
  the value); container links ride the offs column (OPEN.off = its
  CLOSE index, CLOSE.off = its OPEN); numbers record spans at parse
  (the lean `yep_json_number_shape` validates grammar without
  magnitude work) and convert at materialize via the new
  `yeptris_tape_convert` (bulk, parallel-to-record int64/double
  arrays; `int_min` is discovered then; the tape retains `_src` so
  the call is self-contained). Bindings update in lockstep
  (yeptris-ruby 0.4.0.1).
### Added
- The flow-kernel chunk classifier as a kernels-table slot
  (`json_chunk`): quote/backslash/structural/value-start/C0 masks per
  32-byte chunk — scalar reference, NEON + AVX2 twins, one dispatch
  point, differential-pinned. Stage 1 alone: 598 -> 1294 MB/s.
- `yep_json_number_shape`: the number grammar walk without conversion
  (the lazy tape's arm; same accepts, rejects, and advance as the
  full scan).
- The structural indexer `json_stage1` (the simdjson token-contract
  front, gated research route): 64-byte blocks, escape-scanner borrow
  trick + sequential prefix-xor in u64 chains, operators + string
  opens + scalar-run starts as u32 positions. NEON + AVX2 twins,
  differential-pinned. The two-stage route it feeds stays gated
  (verdict seven: the fused walk remains the optimum for the
  strict-validation contract).
### Performance
- The JSON tape entry: 510 -> 546 MB/s on json-doc (packed literal
  compares, punctuation inlining in the fused walk — commas/colons
  consumed by the value arms; ~400k loop iterations gone). PERF-LEDGER
  verdicts five through seven bound the exploration: simdjson DOM
  measured directly at 1017-1039 MB/s on this corpus.

## [0.3.0] - 2026-09-15
### Added
- CBOR (RFC 8949) decode over the shared DOM (TODO.cbor/01):
  `yeptris_cbor_decode` builds the same tree `yeptris_parse_json`
  builds — zero-copy string spans, tag chains on the node's tag field,
  one data item per call, byte-offset errors. Definite + indefinite
  lengths, half/single/double floats (the RFC's Appendix D decoder,
  libm-free), bignums kept as tag+bytes, strict mode (minimal length
  arguments, text-string map keys). RFC 8949 Appendix A pins every
  vector; Appendix F's non-well-formed corpus pins the rejects; a
  300k-iteration ASAN random-bytes fuzz smoke is clean. Compiles only
  with YEPTRIS_WITH_CBOR (default ON).
- CBOR encode over the shared DOM (TODO.cbor/02):
  `yeptris_cbor_encode` / `_into` — the item-13 discipline (one
  exact-sizing walk, one allocation, linear writes), minimal length
  arguments everywhere, preferred floats (smallest of half/single/
  double that round-trips; NaN canonicalizes to f9 7e00;
  round-to-nearest-even double->half in-tree, libm-free), and
  `YEPTRIS_CBOR_CANONICAL` — RFC 8949 s4.2.1 core deterministic
  profile with map keys sorted bytewise on their encoded forms. The
  02 stability contract holds (repeated encodes byte-identical,
  200k-iteration ASAN roundtrip fuzz clean); Appendix A re-encodes
  byte-exact except the ledgered divergences, each pinned:
  beyond-int64 integers -> preferred floats, indefinite -> definite,
  multi-width NaN/Infinity -> shortest, non-text scalars as map keys
  -> text diagnostics (undefined/simple(N) recognized back — the
  diagnostic mapping is two-way), ASCII-valid byte strings -> text.
  Invalid-UTF-8 tag-str scalars re-encode as byte strings (mt2), so
  binary data round-trips byte-exact. Aliases and non-decimal tag
  text encode to YEPTRIS_ERROR_UNSUPPORTED.
- CBOR Sequences (RFC 8742, TODO.cbor/03):
  `yeptris_cbor_decode_sequence` iterates concatenated top-level items
  — one document per item, callback-owned, nonzero aborts; the error
  channel carries the failing item's index and byte offset; an empty
  sequence is valid. `yeptris_cbor_encode_sequence`/`_into` concatenate
  per-item encodings under the same sizing contract. Gates: item-by-item
  tree identity vs standalone decodes, truncation-index errors, the
  boundary-split differential at every cut, and a 20k-run ASAN random
  split fuzz — 0 mismatches.
- CBOR conformance + differentials + fuzz (TODO.cbor/06): the codec's
  verification matrix. fuzz_cbor joins the item-19 harness (invariant
  traps, seed corpus of the RFC 8949 Appendix A + F vectors — checked
  in via scripts/gen-cbor-seeds.py — libFuzzer build + the nightly
  job). Differential vs Python cbor2 (209 shared cases: 177 canonical
  byte-exact, 18 both-reject, 14 divergences ALL within seven
  documented waiver classes — bytes-as-text, int-width, tag-semantics,
  non-text-keys, duplicate-keys, trailing-bytes, depth-cap; zero
  unclassified) and an informational pass against the Ruby cbor gem
  (which additionally accepts two RFC-invalid inputs the ledger
  records). The cbor_canon driver backs both scripts.
- CBOR benchmarks (TODO.cbor/07): the matrix gains a CBOR-vs-JSON tier
  on the same DOM (decode/encode ratios, encoded size, MB/s; rides the
  item-18 artifact). First artifacts: size 0.48-0.52x JSON (target
  met); decode 0.92-0.95x and encode 0.64-1.01x against the 2x/1.5x
  targets — the ledger records the measured reasons (numbers-as-text
  DOM conversion; the O(n^2) canonical-map walks were found BY the
  tier and fixed: encode was 0.09x before the linearization).

### Changed
- scan_stats (NEON) accumulates vertically — pairwise add-accumulate
  into u16 lanes with one horizontal reduce per 4095-chunk batch,
  replacing ten per-chunk UADDV reductions (the leptris count-char
  lesson). The first cut wrapped: vaddvq_u16 returns uint16_t, so a
  4096-chunk dense batch sums to 65536 and reduces to zero — the
  differential's new 64KB/128KB boundary battery caught it. Kernel
  A/B 1.29x on every shape (3.5 → 4.5 GB/s).

## [0.2.7] - 2026-09-14
### Fixed
- The JSON writer's escape table was off by one: `\b` sat at index
  7 but 0x08 is index 8, so backspace emitted `\u0008` instead of
  `\b` (the serialbench byte-parity report). Float exponents now
  pad to two digits (1.0e-07 — Float#to_s/JSON.generate shape);
  canonical output stays a fixed point.

## [0.2.6]## [0.2.6] - 2026-09-14
### Changed
- simdjson's eight-digits SWAR (their parse_eight_digits_unrolled,
  verbatim constants) rides yep_json_number_scan's integer loop:
  eight branch-free ops per 8 digits, the overflow-promotion contract
  intact. Long-number JSON (12-18 digit ids/ns) 2.2x on the tape;
  short-number corpora unchanged (the window never fires).

## [0.2.5]## [0.2.5] - 2026-09-14
### Changed
- TODO.restructure/86 wave 3 (simdjson front): the structural index
  measured DEAD for the tape in both materializations (u32 offset
  array; simdjson's bitmask shape) — gap validation IS the walker's
  fused ws-skip relocated, so the index removes no scan work and
  adds a pass. The verdict is ledgered; the honest levers left are
  the record append and the number kernel. What survived the build:
- Fixed (fuzz-found): yep_json_document and the strict walker
  ACCEPTED a container at map-key position, building inconsistent
  trees — all three grammar machines now reject it (json-suite-strict
  pins unchanged 283/283). tape-diff gains a permanent 2M-case
  status+content parity fuzz; JsonTape pins the gap classes and
  tabs-as-ws parity.
- Milestone (the ledger's standing table): ryml fully beaten — all
  eight shapes over parity on the CI referee (anchor-heavy 0.90x ->
  1.23x across the 0.2.x line).

## [0.2.4]## [0.2.4] - 2026-09-14
### Changed
- TODO.restructure (the all-platforms round): MSVC is a first-class
  build — windows-latest joins the test matrix (302/302 there), the
  ISA selection follows CMAKE_OSX_ARCHITECTURES (x86_64 cross-builds
  on arm Macs link the AVX2 TU, not NEON), the u128 float printer
  rides one portable helper surface, the pool/mapindex mutexes share
  common/mutex.h (SRWLOCK on Windows), the corpus walkers get a
  dirent compat shim, and every checkout/clone pins eol=lf (Windows
  autocrlf corrupted the corpora). The Python wheels matrix grows
  linux-aarch64 and macOS x86_64 (cross) — see yeptris-py.

## [0.2.3] - 2026-09-13
### Changed
- TODO.restructure/84 follow-up (the ledger's 2026-09-14 wave): the
  line-facts short-line gate is ONE capped SWAR walk (8 bytes per
  step, one data-dependent exit) replacing the 32-byte probe plus
  scalar rescan — short lines were scanned twice. anchor-heavy +7%,
  block-heavy +12%, wide-mapping +15% on the same machine. The SWAR
  equality test is the carry-free form (the classic subtract-based
  haszero false-flags '!' after a space run — pinned by the corpus
  and the extended LineFacts alphabet).

## [0.2.1] - 2026-09-13
### Changed
- TODO.restructure/84: the line-facts vector gate tests the LINE, not
  the remaining buffer — a 32-byte break probe routes short lines to
  the scalar fused walk (76's gate was true for every line but the
  document last; the vector sweep was anchor-heavy's hottest entry).

## [0.2.0] - 2026-09-13
### Fixed
- TODO.restructure/82: x86 kernel dispatch via raw cpuid —
  __builtin_cpu_supports returned 0 for AVX/AVX2 on the CI Xeons, so
  every ubuntu benchmark had silently run the SCALAR table. With the
  kernels live, the CI referee (first honest table): flow-json 1.34x,
  json-doc 1.46x, flow-single 1.12x, deep-nesting 1.10x,
  scalar-heavy 1.03x, wide-mapping 1.01x vs ryml (block 0.98x,
  anchor 0.91x). Also fixes the latent AVX2 stopset signedness bug
  the live differential caught the same minute (a group's 8th member
  set lane-mask 0x80, which signed cmpgt_epi8 read as no-hit).

## [0.2.2] - 2026-09-13
### Added
- TODO.restructure/85: the JSON tape — `yeptris_parse_json_tape`
  drives the strict fused walk into compact parallel columns (kinds,
  offsets, lengths, values carved from one block; strings stay
  zero-copy spans, numbers convert inline, container records link
  their matching index for O(1) skipping). parse_json_tape 301 MB/s
  vs parse_json 207 MB/s on json-doc (0.30x vs simdjson, from
  0.20x) — the DOM build eliminated, the walk is the floor. Gates:
  a tape-vs-DOM differential (`tape-diff`, 340 cases) pins record
  stream == tree for every JSONTestSuite document plus status
  parity with parse_json; 13 JsonTape unit specs pin the record
  contract.
- TODO.restructure/83 (issue #238): the schema-descriptor
  materialization API — `yeptris_schema_load` walks a
  caller-compiled descriptor (flat 24-byte plan nodes; kinds
  SCALAR/SEQUENCE/MAPPING/CALLBACK; shared child slices give
  two-names-one-slot merges) and fills typed columns; the
  intermediate generic document never exists. Whole-document
  results in caller-allocated buffers; `YEPTRIS_DESC_ABI` gates in
  lockstep; new status `YEPTRIS_ERROR_SCHEMA` names a missing
  REQUIRED node. docs/schema-abi.md is the co-design doc for the
  committed consumer.

## [0.1.32] - 2026-09-13
### Changed
- TODO.restructure/81 stage 2: the strict fused JSON walk — the walker
  enforces RFC 8259 keys through a strict bit (the lenient flow class
  keeps unquoted YAML keys); yeptris_parse_json's clean route is one
  SIMD gate pass plus ONE strict walk that validates and builds.
  parse_json 157 -> 203 MB/s (+29 percent), 0.20x vs simdjson.

## [0.1.31] - 2026-09-13
### Changed
- TODO.restructure/81: the JSON-field campaign opens — simdjson
  (pinned tree) joins the bench references with the json-doc corpus
  and an order-alternating referee; the clean gate skips the
  whole-document UTF-8 pass in yeptris_parse_json (measured: no
  movement, kept as pass-elimination). The build-then-validate
  fusion was rejected by json-suite-strict (the DOM builder's walker
  is the lenient flow class) — the strict validator rules first.
- TODO.restructure/80: bench artifacts state the active kernel table
  and CPU vector flags; the head-to-head tables ride the markdown.
- TODO.restructure/79: the fused block engine (the last >10 percent
  structural lever) is specced with scope estimate.

## [0.1.30] - 2026-09-12
### Changed
- TODO.restructure/78: the dash-item batch — `on_block_item` mirrors
  the pair contract for classified `- value` lines (block-pair-diff
  extended with dash corpora; the fast-path surface is now
  pair/open/item).
- TODO.restructure/77: key-typing deferral closed BLOCKED by the
  binding audit — the Ruby materializer reads key tag_id for `<<`
  merge detection and typed keys (Psych parity). Consumers recorded.

## [0.1.29] - 2026-09-12
### Changed
- TODO.restructure/76: the line-facts architecture — `yep_line_facts`
  (end/indent/plain-stop) computed once per line (scalar for short
  spans, NEON/AVX2 kernels gated at >=64B); line info and the line
  shape both derive from it, and the shape's key span comes from the
  stop byte instead of re-walking the span. anchor-heavy +13 percent
  absolute (99-103 -> 116 MB/s same-machine); the vector sweep below
  64B measured 2x SLOWER than the tight scalar loops and is gated —
  the dead end is recorded in the perf ledger.

## [0.1.28] - 2026-09-12
### Changed
- TODO.restructure/73: alias names borrow in both paths (the event
  path copied every alias name into the DOM string arena — 266k
  copies per anchor-heavy parse); node init is one 48-byte template
  copy.
- TODO.restructure/74: `stopset_find` / `find_not` walk 32 bytes per
  NEON iteration (16-byte epilogue keeps every prefix exact).
- TODO.restructure/75: the engine's unwind loop hoists the dash-blank
  test and loads the top frame once per iteration.

## [0.1.27] - 2026-09-12
### Changed
- TODO.restructure/68: SIMD `stopset_find` — the stop class becomes a
  precompiled `yep_stopset` (bitmap truth + nibble-class tables),
  with NEON `tbl` and AVX2 `pshufb` kernels; the scalar bitmap walk
  was 9-22 percent of every losing shape (kernel 2156 -> 621 samples
  on scalar-heavy).
- TODO.restructure/69: BOTH `gate_scan` vector masks were broken since
  the kernel landed — AVX2 OR'd `~allow` in (the gate tripped on every
  input, running the whole-document SWAR validator on every x86 parse
  — the ubuntu scalar-heavy asymmetry), NEON ANDed the allow mask with
  zero (every TAB/LF/CR tripped it), and both missed byte 0x1F (a raw
  0x1F bypassed printable validation — closed). `SimdText.GateScan`
  now pins kernel == naive. The validator rides 32-byte gate chunks
  with the exact per-byte walk only for dirty chunks.
- TODO.restructure/70: `yep_dnode` 56 -> 48 bytes — node line/col was
  written-never-read; the fused flow walker drops per-token line
  bookkeeping. The event stream remains the position SSOT.
- TODO.restructure/71: core12 single-branch shape (word leads return
  without the walk: 649 -> 249 samples on anchor-heavy), the engine's
  repeat-alias memo (merge-key YAML), and anchor-table first-alloc
  sizing (no 18-step growth cascade per anchor-heavy parse).
- TODO.restructure/72: e_node audit — it is the nested-map opener, not
  a fast-arm miss; the engine-loop rework is deferred with numbers.
- The interleaved h2h referee alternates order per round (yeptris-first
  handed ryml the warmed core; ±0.2 of swing was that bias).

## [0.1.26] - 2026-09-12
### Changed
- TODO.restructure/67: the resolver first-byte gate (a lead byte
  that can begin no core word is a string in one compare — the
  reject chain was 6.5 percent of deep-nesting on the ubuntu
  profile) and the last scan_stats routes (parse_json, values) ride
  heuristic sizing like the main path. Local head-to-head:
  flow-single 1.08x, flow-json 1.47x, deep-nesting 1.01x.

## [0.1.25] - 2026-09-12
### Changed
- TODO.restructure/66: the stats pre-pass is dead — the ubuntu perf
  profile named the whole-buffer multi-class sweep (~12-13 percent
  on the asymmetry shapes). The encoding gate rides a dedicated
  gate_scan kernel; DOM sizing rides length heuristics; the nametab
  reserve keeps a memchr-chain amp count. The 18B allocation table
  is unchanged. Local head-to-head: deep-nesting 1.00x,
  flow-single 0.96x, flow-json 1.37x.

## [0.1.24] - 2026-09-11
### Changed
- TODO.restructure/64-2a: the 56-byte node — kind/style/flow/
  implicit pack into one bitfield byte and the mutation-only
  attached/depth fields leave the record for lazily-grown side
  tables (parse writes five fewer bytes per node, two fewer stores
  per link). Local head-to-head: flow-json 1.46x, anchor-heavy
  1.12x. The node-size gate is 56B.

## [0.1.23] - 2026-09-11
### Changed
- TODO.restructure/63 phase 1: the key-event defer — classified
  lines stop building-and-discarding the key event (48 stores per
  line); it constructs only on emitting paths, from the pre-fold
  line fact. Local head-to-head: block-heavy 1.03x, flow-json 1.25x.

## [0.1.22] - 2026-09-10
### Changed
- The resolver number hook: spans the walker already validated as
  strict numbers skip resolve()'s digit re-walk (the walker reports
  is_float as a fact; the schema decides the tag — the typing SSOT
  holds). Local head-to-head: flow-json 2.04x, flow-single 1.00x,
  deep-nesting 1.03x vs rapidyaml.

## [0.1.21] - 2026-09-10
### Changed
- TODO.restructure/58: line-fact memo seeding (block lines were
  double-scanned) and short-span scalar walks (the SIMD stopset
  dispatch cost more than the 2-8 byte key scans it served; scalar
  walk below the same 64-byte gate scan_line uses). deep-nesting
  0.59 -> ~1.0x, flow-single ~0.99x, block-heavy ~0.98x on local
  head-to-head medians. Unsigned stopset indexing in the short-span
  walk (multibyte UTF-8 shifted signed-char negative — found by CI
  sanitizers).

## [0.1.20] - 2026-09-10
### Fixed
- The zero-copy borrow (TODO.restructure/56): dom->input_base is now
  set BEFORE the engine run — input-slice scalars, keys, and anchor
  names are views instead of arena copies. (The document already kept
  the transcoded buffer alive for this; the ordering contradicted the
  design.)

### Added
- TODO.restructure/57: on_block_open — a `key:` line opening a fresh
  mapping builds the map and key nodes in one sink call; the engine
  pushes its frame silently. block-pair-diff extended.

### Changed
- flow-json 1.05-1.69x, block-heavy/anchor-heavy/wide-mapping cross
  1.0x on CI head-to-head medians; deep-nesting, scalar-heavy and
  flow-single remain the active campaign (TODO.prompt.md wave 3).

## [0.1.19] - 2026-09-10
### Added
- TODO.restructure/53: fused validate+build — one walk per
  JSON-class flow span. The sink contract is a trio (build/commit/
  rollback); the DOM stages scratch nodes past ncount, pairs them on
  its live stack, and commits the root only after the engine's
  grammar checks pass — every fallback stays byte-identical. The
  grammar walker is scan/json.c's SSOT; the engine's runtime
  max-depth threads through.

- The bounded-parse law: every DOM growth path is capped by input
  size (nodes ≤ len + 1024, docs/anchors sparser, arena 128×) — a
  parse over N bytes allocates O(N) whatever the bug, failing
  YEPTR_MEMORY instead of ballooning; the quadratic %TAG-expansion
  DoS is bounded the same way. MemGuard specs pin the caps.

### Changed
- flow-json runs 1.11x (mac) / 1.15x (ubuntu) of rapidyaml on the CI
  head-to-head medians — the first ryml-comparable shape fully won.
  Sink literals are designated initializers throughout.

## [0.1.18] - 2026-09-10
### Added
- TODO.restructure/49: the one-pass line classifier — scan emits
  per-line shape facts once; the engine's fast arms dispatch a line
  in one decision (plain/alias/anchor/flow values, dash entries),
  strict bail on every deviation. 21 new specs pin the classes.

- TODO.restructure/50: the flow sink fast path — `yep_sink` gains an
  optional `on_flow_json`; the DOM builds engine-validated JSON-class
  spans directly through its own placement laws (the parse_json
  jbuilder unified onto them). PERMANENT flow-direct-diff gate.

- TODO.restructure/54: the block pair fast path — the classified
  `key: value` line is offered whole to the sink (`on_block_pair`);
  the DOM builds both nodes with one resolver decision per scalar.
  PERMANENT block-pair-diff gate (tree comparator shared:
  test/flow/tree_diff.h).

- The benchmark matrix carries an interleaved head-to-head table vs
  rapidyaml (median of per-round ratios) — the campaign referee
  (separate-phase best-of rides phase bias; runners are bimodal).

### Fixed
- Key-anchored scalars (`&a: key`) now bind their anchor ordinal in
  the DOM — their aliases previously resolved to an unwritten slot
  (node 0 or heap garbage; found by flow-direct-diff on 2SXE/E76Z).

### Changed
- flow-json (block-of-flow) runs 0.50x -> ~0.95x of rapidyaml on
  the CI head-to-head medians; wide-mapping and flow-single moved up
  (0.67->0.74x, 0.51->0.80x mac). Full parity remains on the board
  (TODO.restructure/53, 55).

## [0.1.17] - 2026-09-09
### Changed
- TODO.restructure/45 Phase-B slice one: single-line flow spans
  (the dominant `key: {…}` / `- {…}` shapes) skip the flow_enforce
  rescan and the per-event line bookkeeping in the engine's flow
  kernel — one memchr decides. Fresh-runner CI bench now carries
  the rapidyaml columns: block-heavy runs 1.47x FASTER than ryml.

- The escape grammar is ONE authority: `yep_json_string`'s escape
  validation extracted to a single validator (TODO.restructure/47's
  survivor; the structural-index descent itself was measured dead —
  every benchmark shape regressed — and reverted, ledgered in full).

## [0.1.16] - 2026-09-08
### Added
- TODO.restructure/46: `qbc_find` — a one-pass SIMD string-stop
  kernel (first quote, backslash, or C0 control; AVX2/NEON/scalar
  TUs + dispatch) now backs `yep_json_string`. Hardened
  differential + C0-deep-in-vector-path rejection tests; measured
  +2% on 55-byte-string JSON cells, flat elsewhere (the ledger
  records the host-materialization verdict).

## [0.1.15] - 2026-09-08
### Added
- TODO.restructure/43 (canon parity): Psych's sexagesimal weights —
  the fold is now weight-based (component e weighs 60^|e-2|, sign on
  the first component): 2-component values are H:M (`1:30` = 5400,
  not 90; `-1:30` = -1800; `1:30.5` = 5430.0); 3-component unchanged.
- Beyond-int64 integers marshal as Integer (Psych parity): the
  emitter rebuilds Bignum limbs from the decimal text under Psych's
  integer shape (no leading zeros, `_`/`,` separators).

### Changed
- yeptris_node_int/float delegate to the yep_num_* kernels — the
  typed accessors carried a THIRD copy of the number conversion
  (clean + bases + inf/nan + sexagesimal); numbers.c is the one
  fold (MECE), ~150 duplicated lines deleted.

- TODO.restructure/41 (Python binding): yeptris.json.loads — the
  native JSON loader over the exported scan kernels beats json.loads
  (CI-gated 1.00: ubuntu 0.785x mean 198/200, macos 0.699x 181/200;
  was ~15x behind). The ledger records the intern-tax and
  NaN-parity corrections; the next lever (kernel TUs compiled into
  the extension) is scoped for item 42's wave.

- Release integrity (TODO.restructure/40): `scripts/smoke-gem.sh` —
  every gem (ruby + platform) is installed into an isolated GEM_HOME
  and must pass the binding artifact battery BEFORE `gem push`; the
  binding CI checks out the C core at the newest RELEASE TAG (the
  artifact users get); branch protection on both repos makes red
  merges structurally impossible.

## [0.1.14] - 2026-09-08
### Fixed
- core_12 marshal typing (TODO.restructure/32 completion): the parse's
  schema is now a document property (`yeptris_document.schema`, set by
  both parse paths — strict JSON is core by construction), and the
  Marshal emitter threads it: Psych's dot-required float quirk applies
  ONLY under `YEPTRIS_SCHEMA_11_COMPAT`. Under core, `1e3` marshals as
  `{"k"=>1000.0}` (spec 10.3.2: the core float regexp has an OPTIONAL
  dot); under compat it stays `{"k"=>"1e3"}`, byte-identical to Psych.
  Regression test: `Marshal.SchemaConditionedFloatTyping` (both the
  direct `yeptris_marshal` and `yeptris_marshal_node` paths).
- The Ruby binding's core12 spec suite ran green only against a
  working-tree C build; the shipped 0.1.13.x gems vendor C v0.1.13,
  which lacked this fix — the native YAML.load path returned the
  compat typing under core_12. Fixed by this change; the next lockstep
  gem release carries it.

## [0.1.13] - 2026-09-07
### Added
- `yep_json_number_scan` (scan/json.c): the JSON number grammar walk
  FUSED with conversion — one scan validates and converts (integer
  fast-path into int64, INT64_MIN exact; `*is_float` reports the text
  shape: 0=int, 1=float, 2=integer-beyond-int64 with an approximate
  `*dv`). `yep_json_number` is now a thin wrapper. The Ruby native
  materializer's number path is one call — no more validate-then-
  reconvert double walk.
- `scripts/build-native-asset.sh` + release.yml: every GitHub Release
  carries a prebuilt native-materializer tarball for the publish
  runner's platform (build recipe versioned in scripts/, never inline
  workflow YAML).

## [0.1.12] - 2026-09-07
### Added
- The visit API (`yeptris/visit.h`): `yeptris_visit`,
  `yeptris_visit_json` (fused RFC 8259 scan — no DOM, no records),
  `yeptris_visit_node`; the JSON scan kernels and number converters
  are exported for host materializers. The Ruby native materializer
  riding this beats JSON.parse (mean 0.72x on the 152 KB corpus).

## [0.1.11] - 2026-09-07
### Added
- `yeptris_marshal`/`yeptris_marshal_node`/`yeptris_marshal_free`
  (TODO.restructure/21): the C side converts value records into Ruby
  Marshal 4.8 bytes; the binding materializes the whole object graph
  with one `Marshal.load` call. ~10× faster than the columnar walk on
  JSON-shaped input and ~5× on YAML, ~50× on the per-node DOM walk
  (`Node#to_ruby` becomes bulk). Alias identity preserved through `@`
  links; merge keys and timestamps return `ERROR_UNSUPPORTED` for the
  record-walk fallback.

### Changed
- The value-drain entry (`yep_values_from_input`) sniffs strict-JSON
  (`{`/`[` as the first non-space byte) and routes through the JSON
  scanner + DOM linearizer on the same path the YAML engine takes;
  any grammar surprise defers to the engine. Records stay byte-
  identical across routes (the Ruby binding's 2.7k-corpus
  differential pins the equivalence).

### Fixed
- DOM `lin_node` linearizer: pending anchors decorate the value that
  *follows* them, even inside a container; nested anchor bindings no
  longer clobber the outer pending index (ASAN caught a stale-index
  write on `&a [&b x]`).
- Marshal `anchor_find`: YAML lets a later `&anchor` shadow an earlier
  one of the same name; lookup scans newest-first
  (libyaml snapshot 3GZX).

## [0.1.10] - 2026-09-06
### Fixed
- MECE: the `\n`/`\r` break stop set has ONE home (scan.c) — the
  quoted-scalar path rebuilt a bit-for-bit duplicate per quote.

## [0.1.9] - 2026-09-06
### Performance
- The JSON string stop set becomes a constant (34 bitmap bits were
  cleared and set per string scan); strict-JSON strings at 479 MB/s.
- The plain-scalar stop sets become constants (clear + 4-9 adds per
  scan, twice per line). scalar-heavy parse reaches 3.36x libyaml —
  the fastest engine yet. Both bitmaps pinned against runtime builds
  (a hand-written drift ends every plain scalar early).

## [0.1.8] - 2026-09-06
### Performance
- Plain-value spans scan once: e_plain_multiline takes the caller's
  span (every plain value was scanned twice — colon decision, then
  the fold). All shapes at their bests (scalar 3.23x).

## [0.1.7] - 2026-09-05
### Fixed
- `stopset_find`'s differential test hardening (rare-byte sets let
  broken vector paths pass); the nibble-method SIMD attempt was
  built, measured a net loss, reverted — analysis ledgered.

## [0.1.6] - 2026-09-05
### Fixed
- UBSan-caught UB: NULL+0 pointer arithmetic in the zero-entry
  columnar drain and the scan_stats tails (both guarded).

## [0.1.5] - 2026-09-05
### Performance
- Prefix-slot interner: 16-byte slots carry the key's first-8-byte
  prefix + length + value INLINE — one cache line per probe for
  keys <=8 bytes (nametab-GET misses were ~14% of anchor-heavy
  parse). Best absolute times on every shape at release.

## [0.1.4] - 2026-09-04
### Performance
- scan_line rides the SIMD kernels for line end + indent behind a
  64-byte span gate (below it, dispatch overhead beats the loop).
- Fused pre-scan: one SIMD pass computes the ten occurrence counts
  plus the printable/ASCII flags (four full-buffer passes before).

## [0.1.3] - 2026-09-04
### Added
- `yeptris_value_drain_columns`: the value stream as parallel typed
  buffers carved from one allocation — column i is byte-faithful to
  record i (equivalence-pinned).

## [0.1.2] - 2026-09-04
### Fixed
- The pend anchor id rides the pend view: a props-only line before
  its node ("--- &id001" / "- *id001") carried the anchor view with
  a zero id — aliases to it failed with an empty error. Found by the
  Ruby port's self-referencing-structures spec; four-case C
  regression test added.

## [0.1.1] - 2026-09-04
### Performance
- Engine pass 1: anchor ordinals end to end, interner pre-sizing
  from the '&' count, word-at-a-time hash + inline view equality,
  per-line scan memo, DOM node-hint floor, constant-size resolver
  word checks. anchor-heavy 1.27x -> 1.96x libyaml.
### Fixed
- The line-scan memo could rewind a mid-line position and spin
  forever (found by the roundtrip harness on a directive +
  inline-comment document); loop heads that legitimately see mid-line
  positions keep the scan-from-pos semantics.

## [0.1.0] - 2026-09-03

### Added

- Bootstrap scaffold (TODO.impl/01): CMake build (C11, LTO for Release,
  ASAN/TSAN options, scoped warnings), public header skeleton (`yeptris.h`
  umbrella, opaque pointer-sized handles, pinned status enum, generated
  version header), CLI with command registry (`yeptris version`), ABI
  pinning test, CI workflows (test matrix + ASAN), `validate.sh` and
  `bump-version.sh`.
