/* test_parse.cpp — end-to-end parse smoke tests (TODO.impl/07/09/11).
 * Representative documents → public API queries; errors → status + line.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <yeptris.h>
#include <yeptris/marshal.h>

#include "common/chartype.h"
#include "common/simd_text.h"
#include "parse/engine.h"
#include "scan/scan.h"
#include <yeptris/json.h>

namespace {

std::string val(YeptrisNode n) {
    size_t len = 0;
    const char* p = yeptris_node_value(n, &len);
    return p ? std::string(p, len) : std::string();
}

/* First value for a mapping key (shortcut). */
std::string map_str(YeptrisNode map, const char* key) {
    YeptrisNode v = yeptris_node_map_get(map, key, strlen(key));
    return val(v);
}

} // namespace

TEST(Parse, FlowPropsAtEofExactBuffer) {
    /* the nightly fuzz_parse crasher (2026-10-08): flow properties
     * consumed to EOF, then the node read ran one byte past the
     * input. Pinned with an EXACT-SIZE heap buffer (no NUL slack) so
     * the ASAN build sees any regression of the bound. */
    const char bytes[8] = {'[', '!', '<', '<', '\r', '\n', '!', ']'};
    char* y = (char*)malloc(sizeof bytes);
    ASSERT_NE(y, nullptr);
    memcpy(y, bytes, sizeof bytes);
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, sizeof bytes, &st);
    EXPECT_EQ(doc, nullptr);
    EXPECT_EQ(st, YEPTRIS_ERROR_PARSE);
    free(y);
}

TEST(Parse, RunnerRejectsIndicatorFirstPlain) {
    /* the nightly fuzz_roundtrip trap (2026-10-08), "u: }": the fused
     * block runner accepted plain scalars opening with a reserved
     * indicator (c-indicator set) while the engine chain, the emitter,
     * Psych and PyYAML all reject. Every form must fail cleanly. */
    const char* bad[] = {"u: }", "u: ]", "u: ,", "u: @x", "u: `x", "u: %x", "k: }", "}: v", "- }"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_NE(st, YEPTRIS_OK) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, FlowQuoteEscapeAtEofExactBuffer) {
    /* the nightly fuzz_parse crasher (2026-10-08): a flow double-quoted
     * scalar whose final input byte is a lone backslash drove the AVX2
     * quote-scan tail past the buffer end. Exact-size heap buffer (no
     * NUL slack) so ASAN builds see any regression of the bound; the
     * parse must reject cleanly. */
    static const unsigned char bytes[] = {
        0x5b, 0x22, 0x5c, 0x75, 0x30, 0x79, 0x3a, 0x20, 0x2d, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c,
        0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x30, 0x36, 0x30, 0x68, 0x68, 0x68, 0x36, 0x30,
        0x68, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x68, 0x68,
        0x68, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x20, 0x68, 0x68,
        0x68, 0x68, 0x68, 0x30, 0x31, 0x62, 0x0a, 0x32, 0x60, 0x5c};
    char* y = (char*)malloc(sizeof bytes);
    ASSERT_NE(y, nullptr);
    memcpy(y, bytes, sizeof bytes);
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, sizeof bytes, &st);
    EXPECT_EQ(doc, nullptr);
    EXPECT_NE(st, YEPTRIS_OK);
    free(y);
}

TEST(Parse, AnchorNeedsName) {
    /* the nightly fuzz_roundtrip trap (2026-10-08), input "&": an
     * anchor with an EMPTY name was accepted (root, value, flow), the
     * serializer emitted an empty document, and its re-parse returned
     * NULL — parseable-implies-marshalable broken. libyaml/Psych
     * reject all of these ("did not find expected alphabetic or
     * numeric character"); a bare '!' tag stays legal. */
    const char* bad[] = {"&", "&\n", "& x", "k: & v", "- & x", "k: &\n", "[&]", "[& ]", "{a: &}"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_NE(st, YEPTRIS_OK) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    /* the legal forms still round-trip: serialize then re-parse */
    const char* ok[] = {"!", "&a", "k: &a v", "[&a]", "a: &b c"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        EXPECT_NE(doc2, nullptr) << y << " -> " << s1;
        if (doc2 != NULL) {
            size_t l2 = 0;
            char* s2 = yeptris_serialize(doc2, &l2);
            EXPECT_TRUE(s2 == NULL || (l1 == l2 && memcmp(s1, s2, l1) == 0)) << y;
            free(s2);
            yeptris_document_free(doc2);
        }
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, VerbatimTagClosesSameLine) {
    /* the nightly fuzz_roundtrip byte-instability trap (2026-10-08),
     * input "!<": an unterminated/empty/spaced verbatim tag was
     * accepted, then serialize/re-parse grew a blank line per round.
     * libyaml requires the '>' to close on the same line with no
     * interior space and non-empty content. */
    const char* bad[] = {"!<", "!<\n", "!<>", "!<a", "k: !<", "!<a b>", "[!<]"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_NE(st, YEPTRIS_OK) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"!<a>", "!<a> v", "!", "!foo v", "!!str v", "a: !!int 1"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, FlowNullStyleRoundTrips) {
    /* the nightly fuzz_roundtrip byte-instability trap (2026-10-08),
     * input "? - :": a synthesized flow null carried style ANY, so the
     * emitter chose double-quotes and each serialize/re-parse round
     * drifted ("{"k": }" -> "{"k": ""}"). Synthesized nulls are PLAIN:
     * Psych resolves every one of these to nil. */
    const char* ins[] = {"{\"k\": }", "{\"k\":}", "{\"k\": , \"j\": v}",
                         "? - :",     "[a, ]",    "{a: , b: 1}"};
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, Utf16RoundTripThroughContinuationByte) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-08): a
     * UTF-16LE document decoding to U+315B (E3 85 9B). The NEL fix's
     * lone-0x85 escape fired on the CONTINUATION byte, corrupting the
     * sequence into E3 + the TEXT "\\x85" + 9B — un-re-parseable.
     * NEL is the C2 85 PAIR only; a lone 0x85 byte is a continuation
     * byte that must ride untouched. */
    static const unsigned char bytes[] = {0xff, 0xfe, 0x6f, 0x59, 0x26, 0x2b, 0x5b, 0x31};
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse((const char*)bytes, sizeof bytes, &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(l1, 10u) << "plain UTF-8, no quoting";
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0);
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, BomCharEscapesAsUFEFF) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-08, run
     * 37833916837): a UTF-32LE document whose content decodes to a
     * lone U+FEFF. Emitted raw, those three bytes ARE the UTF-8 BOM —
     * the re-parse strips them and the document vanishes. Psych
     * escapes U+FEFF everywhere: "\\uFEFF". */
    static const unsigned char bytes[] = {0xff, 0xfe, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00};
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse((const char*)bytes, sizeof bytes, &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    static const char want[] = "\"\\uFEFF\"\n";
    EXPECT_EQ(l1, sizeof want - 1);
    if (l1 == sizeof want - 1) {
        EXPECT_EQ(0, memcmp(s1, want, l1));
    }
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0);
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, PropsChainRecursionCapped) {
    /* the nightly fuzz_parse STACK OVERFLOW (2026-10-08, run
     * 37839718900): the e_node <-> e_parse_value props/value-line
     * chain opens NO container frame, so e->depth never grew and the
     * C recursion ran unbounded (props-only lines at document root).
     * A recursion counter capped at max_depth now rejects with
     * YEPTRIS_ERROR_DEPTH — the depth-guard law: error, never crash. */
    static const char rep[] = "!t\n";
    size_t n = strlen(rep) * 20000;
    char* y = (char*)malloc(n);
    ASSERT_NE(y, nullptr);
    for (size_t i = 0; i < 20000; i++) {
        memcpy(y + i * strlen(rep), rep, strlen(rep));
    }
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, n, &st);
    EXPECT_EQ(doc, nullptr);
    EXPECT_EQ(st, YEPTRIS_ERROR_DEPTH);
    free(y);
    /* an ANCHORED props chain cannot reach the cap: the second line's
     * anchor displaces the pend anchor (same-line-wins), which leaves
     * the displaced anchor defined but unemitted — a later alias would
     * dangle (the nightly's run 38271222289). libyaml rejects every
     * nesting variant ("did not find expected <document start>"); a
     * TAG-only continuation ("&a\n!!str v") stays legal. */
    static const char rep2[] = "&a\n";
    size_t n2 = strlen(rep2) * 20000;
    char* y2 = (char*)malloc(n2);
    ASSERT_NE(y2, nullptr);
    for (size_t i = 0; i < 20000; i++) {
        memcpy(y2 + i * strlen(rep2), rep2, strlen(rep2));
    }
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc3 = yeptris_parse(y2, n2, &st2);
    EXPECT_EQ(doc3, nullptr);
    EXPECT_EQ(st2, YEPTRIS_ERROR_PARSE);
    free(y2);
    /* ordinary nesting still parses: the cap rides the same 1000 */
    static const char nested[] = "a:\n";
    size_t m = strlen(nested) * 50;
    char* z = (char*)malloc(m + 2);
    ASSERT_NE(z, nullptr);
    for (size_t i = 0; i < 50; i++) {
        memcpy(z + i * strlen(nested), nested, strlen(nested));
    }
    z[m] = ' ';
    z[m + 1] = '1';
    YeptrisStatus st3 = YEPTRIS_OK;
    YeptrisDocument doc4 = yeptris_parse(z, m + 2, &st3);
    EXPECT_NE(doc4, nullptr);
    if (doc4 != NULL) {
        yeptris_document_free(doc4);
    }
    free(z);
}

TEST(Parse, TagPercentEscapeRoundTrips) {
    /* the nightly fuzz_roundtrip byte-instability trap (2026-10-09,
     * run 37855784068, input "%.\n!%0a"): the parse side decodes tag
     * URI escapes per the spec (libyaml does too) — "!%0a" becomes a
     * tag containing a raw newline. The emitter must RE-ENCODE unsafe
     * tag bytes as %XX; raw they corrupt the document each round. */
    const char* ins[] = {"!%0a", "a: !%0a", "!%41", "%YAML 1.1\n--- !%0a"};
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, TagSuffixRejectsNonUriFollower) {
    /* libyaml scans tag suffixes over its URI set only; bytes like
     * '>' leave the suffix and surface as the tag follower, which
     * must be rejected ("did not find expected whitespace or line
     * break while scanning a tag"). The nightly fuzz_roundtrip trap
     * (2026-10-10, run 38057695714, input "!>\r?\r\r:\r") parsed
     * "!>" as a tag whose serialization could never re-parse. */
    const char* bad[] = {"!>\r?\r\r:\r", "!>x y", "!\"x y", "!|x y", "!`x y", "!\\x y"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    /* @ and ~ and % are URI bytes: these stay legal */
    const char* good[] = {"!@x y", "!~x y", "!%41 y"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_NE(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, RootTagAnchorSerializeSeparator) {
    /* a root block collection with both tag and anchor carried its
     * props as "!<...>&a4" — no separator; libyaml requires blank or
     * line break after a tag, so the output could not re-parse (the
     * 9KAX emit-diff family). The writer must put a space between
     * them. */
    const char* y = "&a4 !!map\n&a5 !!str key5: value4\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    int sep_found = 0;
    for (size_t i = 0; i + 9 <= l1; i++) {
        if (memcmp(s1 + i, "!!map &a4", 9) == 0) {
            sep_found = 1;
            break;
        }
    }
    EXPECT_TRUE(sep_found) << s1;
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0);
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, PendPropsDoNotLeakOntoTheFirstKey) {
    /* the nightly fuzz_roundtrip byte-instability trap (2026-10-10,
     * run 38092896626, input "!\r!\r?\r\r:\r"): a props-only line
     * above a mapping attaches its tag/anchor to the MAPPING
     * (Psych: "!t\n\"a\": v" -> map tagged, key clean), but the
     * quoted- and flow-key promotions emitted the key carrying the
     * merged props — re-parse produced a tagged first key and the
     * serialization never re-stabilized. Same-line props stay on the
     * key (Psych: "!t \"a\": v" -> key tagged). */
    const char* stable[] = {"!\r!\r?\r?\r:\r\r:\r", "!<t>\n\"a\": v\n", "!<t> \"a\": v\n",
                            "!<t>\n[a]: v\n",       "!<t> [a]: v\n",    "&p\n!<t>\n\"a\": v\n"};
    for (const char* y : stable) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y << "\n"
                                                                       << s1 << "\nvs\n"
                                                                       << (s2 ? s2 : "");
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, TagUriEscapesNeedTwoHexDigits) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-10, run
     * 38095309044, input "!e%2%21bz"): the URI decoder passed a stray
     * '%' through raw (decoding only the well-formed escape), so the
     * stored tag re-encoded to a form that parsed differently.
     * libyaml rejects the whole family at the tag scan ("did not
     * find URI escaped octet"). */
    const char* bad[] = {"!e%2%21bz\n", "!e%2\n", "!e%\n", "!e%zz v\n", "!<%2>x> v\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* good[] = {"!e%21bz v\n", "!e%0a\n"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_NE(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, LoneCrAfterLiteralIsABreakNotCrlf) {
    /* the nightly fuzz_roundtrip marshal trap (2026-10-11, run
     * 38096448601, input "2: |\n  h!\ro"): the runner's literal-block
     * line advance treated ANY '\r' as CRLF and skipped 2 bytes, so a
     * lone CR swallowed the next content byte as a phantom line break
     * -- the trailing key-less line vanished instead of rejecting
     * ("could not find expected ':'", the Psych verdict). */
    const char* bad[] = {"2: |\n  h!\ro", "a: |\n  x\rb", "k:\n  v\rw"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    /* real CRLF and plain LF literals keep working */
    const char* good[] = {"2: |\n  h!\r\n", "2: |\n  h!\n"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_NE(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, FlowScalarsQuoteTheFlowIndicators) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-11, run
     * 38097567398, "?  :\t555555ta]"): the explicit-key nesting emits
     * the inner map in flow style, and the style chooser's plain
     * predicate is block-context law — ']' rides plain INSIDE "{}"
     * where it is a flow terminator and the emitted form re-parses as
     * a stray close. Flow-context scalars now take the quoted routes
     * whenever they carry ',', '[', ']', '{' or '}'. */
    const char* y = "?  :\t555555ta]\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    /* the indicator must not ride plain inside the flow form */
    int raw_close = 0;
    for (size_t i = 0; i + 4 <= l1; i++) {
        if (memcmp(s1 + i, "ta]}", 4) == 0) {
            raw_close = 1;
            break;
        }
    }
    EXPECT_FALSE(raw_close) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, ShorthandSuffixEscapesItsLeadingBang) {
    /* the nightly fuzz_roundtrip byte-instability trap (2026-10-11,
     * run 38099113018, "!!%21bz\n!%21bz\n"): a local tag whose URI
     * is "!!bz" re-emitted verbatim re-parses as the SECONDARY handle
     * ("tag:yaml.org,2002:bz") — the tag changes meaning every
     * round. The shorthand emitter escapes a suffix-leading '!' as
     * %21, mirroring the escape grammar the parse side decodes. */
    const char* ins[] = {"!%21bz\n", "!!%21bz\n!%21bz\n", "!foo\n", "!(4%21bz\n", "!a%21bz\n"};
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, FlatRunValueHonorsPlainFirst) {
    /* the nightly fuzz_roundtrip marshal trap (2026-10-11, run
     * 38101390216, "k1: --a &p{\n0: ]"): the flat-run specialist's
     * value arm bailed on indicators but skipped the plain-first law
     * — a ']' first byte rode as a plain value and parseable input
     * failed to marshal. The engine rejects the family; now the
     * flat-run bails to it (#489's law, fifth site). */
    const char* bad[] = {"k1: --a &p{\n0: ]\n", "k: ]\n", "k: }\n", "k: ,x\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, TagDirectiveAllowsOnlyCommentAfterPrefix) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-11, run
     * 38102517540, "%TAG !m! !mymy 3\t..."): the %TAG prefix scan ran
     * to end-of-line, so trailing content rode the prefix and the
     * resolved tag could never re-parse. libyaml allows only blanks
     * and an optional comment after the prefix ("did not find
     * expected comment or line break"). */
    const char* bad[] = {"%TAG !m! !mymy 3\n", "%TAG !m! !mymy 3\t\t,-tye\n!m!lie@n\n",
                         "%TAG !m! !mymy !x\n--- v\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* good[] = {"%TAG !m! !mymy\n!m!lie@n\n", "%TAG !m! !mymy # c\n!m!x\n"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_NE(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, AnchorNamesRideBlockAndRefuseFlow) {
    /* the anchor-name law, scoped by the suite: names keep the full
     * suite charset at PARSE (W5VH wants "&:@*!$\"<foo>:" to parse;
     * colons ride — 2SXE; unicode — 8XYN) and ride raw fine in BLOCK
     * emission (byte-stable round-trips). But anchors have no quoting
     * syntax, and in FLOW context an indicator byte can never re-parse
     * (the nightly's "&\"", bang, and question veins) — such documents
     * have no safe spelling and serialize REFUSES them (NULL) rather
     * than emitting a form that corrupts. */
    const char* block_ok[] = {"a: &:@*!$\"<foo>: scalar a\nb: *:@*!$\"<foo>:\n", "&a*b v\n",
                              "&a!b v\n", "&?2 v\n", "&a: key: &a value\nfoo:\n  *a:\n"};
    for (const char* y : block_ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
    /* flow-context: an anchor name cannot start with a quote there —
     * the flow prop scan ends empty and the empty-name law rejects
     * (Psych agrees); names that DO parse in flow but carry unsafe
     * bytes get the serialize refusal below */
    const char* flow_rejects[] = {"[\"&\"\n?:\n? \n:\nu20\"]\n", "[&a\"b c]\n"};
    for (const char* y : flow_rejects) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* flow_refused[] = {"[&a*b c]\n", "{k: &x!y: v}\n"};
    for (const char* y : flow_refused) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        EXPECT_EQ(s1, nullptr) << y;
        if (s1 != NULL) {
            free(s1);
        }
        yeptris_document_free(doc);
    }
}

TEST(Parse, IndentedSeqOwnsItsColumn) {
    /* the nightly's run 38117287882 ("amer:\n  - j\n  n:\n  - Bksl,} --
     * }"): a block seq INDENTED past its parent map's key column owns
     * that column — a non-dash line there can neither join nor close
     * it (libyaml: "did not find expected '-' indicator"). The runner
     * popped the seq and SILENTLY DROPPED the remaining lines while
     * the engine rejected. Only the INDENTLESS seq — parent map keys
     * at the same column — closes on a key line there. */
    const char* bad[] = {"amer:\n  - j\n  n:\n  - Bksl,} -- }", "k:\n  - j\n  n: v\n",
                         "k:\n  - j\n  n:\n  - x\n", "k:\n  - j\n  n:\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"k:\n  - j\nx: v\n", /* dedent to parent col */
                        "k:\n- j\nn: v\n",   /* indentless seq */
                        "k:\n  - j\n  - n\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, PendingSeqResolutionTracksContentColumn) {
    /* the nightly's run 38118641722 ("ue:\n  - one\n woc:\n  - one\n
     * wt o-  : thrt"): the pending-key->seq resolution never recorded
     * the opened map's content column, so the off-column guard could
     * not fire and a key line BETWEEN the seq's column and the
     * parent's fell through to a SILENTLY TRUNCATED document (lines
     * 3-5 dropped) while the engine rejected. Psych: "did not find
     * expected key". The anchored branch already tracked it. */
    const char* bad[] = {"ue:\n  - one\n woc:\n  - one\n wt o-  : thrt", "ue:\n  - one\n woc: v\n",
                         "a:\n  - x\n b: v\n", "ue:\n  - one\n woc:\n  - one",
                         "a:\n  b:\n    - x\n   c: v"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"k:\n- j\nn: v\n",   /* indentless sibling */
                        "k:\n  - j\nx: v\n", /* dedent to parent col */
                        "ue:\n  - one\nnext: v\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, BlockScalarHeaderTailIsBlankOrComment) {
    /* the nightly's run 38119775008 ("ext: | c* " in a multi-doc
     * literal family): the runner's bare-literal arm took the block and
     * SILENTLY DROPPED the trailing header bytes while the engine
     * rejected. libyaml's header law: the params (one indentation
     * digit, one chomp sign) ride the indicator DIRECTLY; after them
     * (or after a blank) only an optional comment may precede the
     * break — "did not find expected comment or line break". */
    const char* bad[] = {"k: | c* \n  d\n", "k: | x\n  d\n",  "k: | c\n  d\n",
                         "k: | 12\n  d\n",  "k: |12\n  d\n",  "k: | -\n  d\n",
                         "k: | 2\n  d\n",   "k: |\t-\n  d\n", "k: > 2\n  d\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"k: |\n  d\n",   "k: |-\n  d\n",  "k: |+\n  d\n",  "k: |2\n  d\n",
                        "k: |2-\n  d\n", "k: |+2\n  d\n", "k: |2+\n  d\n", "k: | # c\n  d\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, AliasNodeIsComplete) {
    /* the nightly's run 38123150843 ("base: &base\n    n)ame: ...\n
     * <<: *base &bar\n ..."): an alias node is COMPLETE — it carries
     * no props and only a blank-then-comment may follow on the line.
     * libyaml errors ("did not find expected key" / "alphabetic or
     * numeric character" for a glued '#'); the DOM route absorbed the
     * stray props while the value drain rejected — the routes split. */
    const char* full = "base: &base\n    n)ame: Everyone has same o\n    <<: *base &bar\n    <<: "
                       "*base\n    age: 20\n";
    const char* bad[] = {full, "a: &x\n  k: 1\nb:\n  <<: *x &y\n",
                         "a: &x\n  k: 1\nb:\n  <<: *x !str\n", "a: &x\n  k: 1\nb: *x *x\n",
                         "a: &x\n  k: 1\nb: *x#y\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"a: &x\n  k: 1\n? *x\n: v\n", "a: &x\n  k: 1\nb: [*x, 2]\n",
                        "a: &x\n  k: 1\nb: *x # c\n", "a: &x\n  k: 1\nb: *x\n",
                        "a: &x\n  k: 1\nb:\n  <<: *x\n  j: 2\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, AnchorNameFollowerLaw) {
    /* the nightly's run 38121449804 ("ow0: &f#o   \n  - :-0~on"): a
     * glued '#' is junk on the anchor name — libyaml errors ("did not
     * find expected alphabetic or numeric character"); the engine
     * accepted and MANGLED the node (anchor kept, bytes after '#'
     * dropped) while the runner kept a different shape. The name ends
     * at a blank, break, or flow indicator; '#' needs a separating
     * blank to be a comment. Suite charset cases (W5VH's
     * &:@*!$"<foo>:) still parse — the check is the BOUNDARY only. */
    const char* bad[] = {"ow0: &f#o   \n  - :-0~on\n", "a: &f#o x\n",
                         "a: &f#o\n  b: 1\n",          "- &f#o x\n",
                         "a:\n  - &f#o x\n",           "a: [&f#o x]\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"a: &f x\n", "a: &f-o x\n", "a: &f #o x\n",
                        "a: &:@*!\"<foo>: k\n" /* the W5VH charset */};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, PendingKeyOverSeqFrameRejects) {
    /* the nightly's run 38127584324 ("~foo:\n- 42\n bar:\n  - 45
     * e."): a pending "key:" line over a SEQ frame — the key cannot
     * join the sequence and the pending pushed UNDER the seq
     * materialized as an ITEM while the engine rejected (libyaml:
     * "mapping values are not allowed in this context"). The valued
     * arm already carried the kind check; the pending arm now does. */
    const char* bad[] = {"~foo:\n- 42\n bar:\n  - 45 e.", "k:\n- 42\n bar:\n  - 45 e.\n",
                         "k:\n- 42\n bar: v\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"k:\n- 42\nbar: v\n", "k:\n- 42\nnext:\n  - 45 e.\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, PendingSeqRootTracksRootContentColumn) {
    /* the nightly's run 38128905644 (" american:\n  - Boston...\n
     * national:\n 0.: ..."): the unanchored pending-key->seq
     * resolution set the OPENED map's content column (catch 41) but
     * never the ROOT's st[0] bookkeeping the anchored branch carries
     * — a dedent past the root map's column popped to a
     * content_col-0 st[0], the pair arm read "root pairs at column
     * 0", and the document SPLIT INTO PHANTOM DOCS ("---\nb\n---
     * \nv") while the engine rejected. The catch-39 law, seq-valued
     * variant. */
    const char* bad[] = {" a:\n  - x\nb: v\n", " a:\n  - x\nb: v\n c: w\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"a:\n  b: 1\nc: 2\n", "a:\n  - x\nc: 2\n", "k:\n  - j\nx: v\n",
                        "a: 1\n b: 2\n" /* rejects: a col-1 line under a col-0 root */};
    for (const char* y : ok) {
        /* the last entry is the ERROR side of the same law */
        if (strcmp(y, "a: 1\n b: 2\n") == 0) {
            YeptrisStatus st = YEPTRIS_OK;
            YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
            EXPECT_EQ(doc, nullptr) << y;
            continue;
        }
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, CoreShorthandSuffixMustRescan) {
    /* the nightly's run 38132505901 (the verbatim tag
     * "!<tag:yaml.org,2002:sta:...intl.org,002:stt+1r>"): the core
     * namespace shorthand only holds when the suffix re-scans over
     * the parse side's tag charset — ours terminates on ',' (the
     * lenience vs libyaml's URI set) — so an interior ',' emitted
     * !!<suffix>, re-parsed truncated, and the round-trip died. The
     * verbatim !<...> wrap carries those. */
    const char* full = "!<tag:yaml.org,2002:sta:yaml.org,2002.org,2002:intl.org,002:stt+1r>";
    const char* ins[] = {full, "k: !<tag:yaml.org,2002:a,b> v\n"};
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        /* the verbatim wrap appears, not the !! shorthand */
        int saw_verbatim = 0;
        for (size_t vi = 0; vi + 2 <= l1; vi++) {
            if (memcmp(s1 + vi, "!<", 2) == 0) {
                saw_verbatim = 1;
                break;
            }
        }
        EXPECT_TRUE(saw_verbatim) << y << " -> " << s1;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, PendingMapRootTracksRootContentColumn) {
    /* the nightly's run 38134498534 ("  e:\n   f: g\nh: i"): the
     * pending->MAP resolution lacked the root bookkeeping the seq arm
     * gained (catch 46) — a dedent past a map-valued root's column
     * popped to a content_col-0 root and the doc accepted the second
     * root the engine rejects. The catch-39 law's map-valued variant. */
    const char* bad[] = {"  e:\n   f: g\nh: i\n", " a:\n   f: g\nh: i\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"a:\n   f: g\nh: i\n", " a:\n   f: g\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, LiteralTrailingBlankClipBounds) {
    /* the nightly's run 38137087116 (ASan heap-buffer-overflow, WRITE
     * in the literal arm's re-walk): the clip lives in the SPAN
     * (flen = clen - tb) but the copy paths write every measured line
     * (clen bytes) — the pool block was allocated flen and the
     * trailing stripped-empty lines wrote tb bytes past it. The block
     * is now clen-sized; the span stays clipped. */
    std::string y = "k: |\n";
    for (int i = 0; i < 3000; i++) {
        y += "  x" + std::to_string(i) + "\n";
    }
    y += "  \n  \n"; /* two stripped-empty trailing lines */
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y.data(), y.size(), &st);
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_document_root(doc, 0);
    YeptrisNode val = NULL;
    ASSERT_EQ(yeptris_node_map_at(root, 0, NULL, &val), 0);
    size_t vl = 0;
    const char* v = yeptris_node_value(val, &vl);
    ASSERT_NE(v, nullptr);
    /* "x" + digits + break per content line; the trailing blanks clip */
    size_t want = 0;
    for (int i = 0; i < 3000; i++) {
        want += 2; /* 'x' + the break */
        int d = i;
        do {
            want += 1; /* '0' renders one digit even for i == 0 */
            d /= 10;
        } while (d > 0);
    }
    EXPECT_EQ(vl, want);
    ASSERT_GE(vl, (size_t)4);
    EXPECT_EQ(v[vl - 1], '\n');
    EXPECT_EQ(v[vl - 2], '9'); /* the last content line, not a blank */
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0);
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, FlowRenderedPlainsQuoteIndicators) {
    /* the nightly's run 38139449828 ("- &a\n ? : a!%!!a\n ? : a'\n
     * -\t "): the explicit-key entry's inner map renders FLOW, and a
     * plain carrying '!' (a tag indicator everywhere) or a lone '\''
     * (opens flow single-quote mode) rode plain inside "{}" — the
     * emitted form died in tag/quote scanning. The flow gate gains
     * both bytes; the suite's M2N8 ("- ? : x" must parse) is
     * untouched — the PARSE side keeps the form. */
    const char* y = "- &a\n ? : a!%!!a\n ? : a'\n-\t ";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
    /* the suite's M2N8 form still parses and round-trips */
    const char* m2n8 = "- ? : x\n";
    st = YEPTRIS_OK;
    doc = yeptris_parse(m2n8, strlen(m2n8), &st);
    ASSERT_NE(doc, nullptr);
    s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    st2 = YEPTRIS_OK;
    doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0);
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, LongFlowKeyRidesTheExplicitForm) {
    /* the nightly's run 38143486175 (a JSON flow doc whose keys'
     * rendered spans cross 1024): the YAML simple-key law caps
     * one-line keys at 1024, so the emitter rides the explicit
     * "? key : " form past the limit — libyaml's escape for long
     * keys. Built docs exercise the emitter directly. */
    YeptrisDocument doc = yeptris_document_new();
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_node_new_mapping(doc);
    ASSERT_EQ(yeptris_document_set_root(doc, root), YEPTRIS_OK);
    std::string key(1030, 'k');
    ASSERT_EQ(yeptris_node_map_add(root, key.data(), key.size(),
                                   yeptris_node_new_scalar(doc, "1", 1, YEPTRIS_STYLE_PLAIN)),
              YEPTRIS_OK);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    ASSERT_GE(l1, (size_t)2);
    EXPECT_EQ(s1[0], '?'); /* the explicit-key form at the head */
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, EscapeLongKeyRidesTheExplicitForm) {
    /* the nightly's run 38145696566: the simple-key law counts the
     * WRITTEN token — a ~1000-raw key whose double-quoted render
     * expands past 1024 rode as a simple key and the emission could
     * never re-parse. The key-span predicates now estimate the
     * rendered length (conservative factor-4 per expandable byte);
     * the promoted scalar key rides QUOTED in the explicit form so
     * its interior ": " sequences cannot re-parse as separators. */
    YeptrisDocument doc = yeptris_document_new();
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_node_new_mapping(doc);
    ASSERT_EQ(yeptris_document_set_root(doc, root), YEPTRIS_OK);
    std::string key; /* ~350 quotes: raw 350, rendered ~1400 */
    for (int i = 0; i < 350; i++) {
        key += "\"";
    }
    ASSERT_EQ(yeptris_node_map_add(root, key.data(), key.size(),
                                   yeptris_node_new_scalar(doc, "1", 1, YEPTRIS_STYLE_PLAIN)),
              YEPTRIS_OK);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    ASSERT_GE(l1, (size_t)2);
    EXPECT_EQ(s1[0], '?'); /* the explicit form at the head */
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, ImplicitNullMarkStaysInBounds) {
    /* the nightly's run 38147359781 (fuzz_parse ASan, READ 1 past a
     * 23-byte exact buffer): after the flow close scan the cursor
     * stood one past EOF, and the implicit-null position mark read
     * e->p[at-1] out of bounds. The mark now clamps to e->len first
     * — zero-copy means every read is bounds-checked. */
    const char* y = "[&\"[?:\n?(\nu\n?\"]:\n\nu\n?\"]";
    YeptrisStatus st = YEPTRIS_OK;
    size_t n = strlen(y);
    char* exact = (char*)malloc(n); /* no NUL slack: ASAN-precise */
    ASSERT_NE(exact, nullptr);
    memcpy(exact, y, n);
    YeptrisDocument doc = yeptris_parse(exact, n, &st);
    /* either verdict is fine — the bounds are the pin (the anchor
     * &"[?: trips the flow-context serialize refusal, catch 38's
     * documented NULL). The document BORROWS the buffer (zero-copy):
     * serialize before free, or the read is a use-after-free */
    if (doc != nullptr) {
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1); /* must not crash */
        free(s1);
        yeptris_document_free(doc);
    }
    free(exact);
}

TEST(Parse, C1ControlsEscapeOnOutput) {
    /* the nightly's run 38151866348 (fuzz_roundtrip, an escape-heavy
     * doc): "\x99" decodes to U+0099 — VALID UTF-8 but NON-PRINTABLE
     * — and the raw pair c2 99 in the emission failed the re-parse's
     * printability gate. The whole C1 range (U+0080..U+009F) rides
     * the \xNN escape on output, Psych's form; the escape re-enters
     * the document the same way it left. */
    const char* y = "hex esc: \"0\\x999\\t2000\\n\"\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    /* the emitted form carries the escape, not the raw pair */
    int saw_escape = 0;
    for (size_t i = 0; i + 4 <= l1; i++) {
        if (memcmp(s1 + i, "\\x99", 4) == 0) {
            saw_escape = 1;
            break;
        }
    }
    EXPECT_TRUE(saw_escape) << s1;
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, BlockScalarsNeverRideFlowKeys) {
    /* the nightly's run 38154662070 ("? !\n- name: ...yaml: |..."):
     * a flow-rendered explicit key's subtree carried LITERAL members —
     * `yaml: |` + content lines inside what reads as a flow context,
     * and the emission could never re-parse. BLOCK SCALARS ARE
     * ILLEGAL IN FLOW: in flow context the style chooser answers DQ
     * for literal/folded styles. */
    const char* y = "? !\n"
                    "- name: Trailing whhtespace in streams\n"
                    "  from: '@ingydotnet'\n"
                    "  yaml: |\n"
                    "    - |+\n"
                    "  tree: |\n"
                    "    +STR\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    /* no literal indicator may ride inside the emission's flow spans */
    int lit_in_flow = 0;
    for (size_t i = 0; i + 4 <= l1; i++) {
        if (memcmp(s1 + i, ": |\n", 4) == 0) {
            lit_in_flow = 1;
            break;
        }
    }
    EXPECT_FALSE(lit_in_flow) << s1;
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, SeqItemPropsEmitOnce) {
    /* the nightly's run 38161738783 ("- &a\n  ? &e\n  ! &e..."): the
     * dash's props arm wrote "- " + " &a" (a DOUBLE space) and the
     * empty collection re-emitted the props emit_node had already
     * written — every round duplicated ("- &a\n  &a {}") until the
     * shape stabilized wrong. The dash carries no trailing space in
     * the props arm; an empty propertied collection rides compact. */
    /* the original crasher's "? &e\n! &e" continuation now REJECTS
     * (catch 71: a props-only line under an explicit '?' key — libyaml
     * "could not find expected ':' while scanning a simple key"); the
     * props-once subject rides a parsing shape */
    const char* y = "- &a\n  x: y\n- &e \"s\": t\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << y;
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    /* the dash's props arm carries no trailing space (no "- &a"
     * double-dash spacing), and the anchor emits once; nested
     * indentation legitimately carries double spaces */
    for (size_t i = 0; i + 2 < l1; i++) {
        EXPECT_FALSE(s1[i] == '-' && s1[i + 1] == ' ' && s1[i + 2] == ' ') << s1;
    }
    int anchors = 0;
    for (size_t i = 0; i + 2 < l1; i++) {
        if (memcmp(s1 + i, "&a", 2) == 0) {
            anchors++;
        }
    }
    EXPECT_EQ(anchors, 1) << s1;
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, LeadingBlankFloorsTheLiteralBlock) {
    /* the nightly's run 38163867815 ("foar: 2\n    \ntext: |\n    \n
     *  b..."): a LEADING blank line with MORE indent than the first
     * content line floors the block — libyaml's auto-detect takes the
     * blanks' max, so the shallower content breaks the mapping
     * (Psych: "did not find expected key"). The runner accepted what
     * the engine rejected; it now bails to the engine. Deeper blanks
     * AFTER content stay legal content (Psych-verified). */
    const char* bad[] = {"a: |\n    \n  b\n\nc: 2\n",
                         "foar: 2\n    \ntext: |\n    \n  b\n\n \ntext: |\n    \n  b\n\nt: |\n  "
                         "a\n  \nbar: 2\n    \next: |\n  a\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y;
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    /* a deeper blank AFTER content is content; a dedent then ends the
     * block and the sibling parses (Psych: {"a"=>"b\n  \n","c"=>2}) */
    const char* y = "a: |\n  b\n    \nc: 2\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, LocalShorthandSuffixMustRescan) {
    /* the nightly's run 38165476712 ("!<!<<b,,,,,-,,,,ar> :yam"): the
     * LOCAL shorthand emission wrote a comma-carrying suffix raw —
     * the parse side's tag charset terminates on ',' (our lenience),
     * so the tag re-parsed truncated and the round-trip died. The
     * catch-47 re-scan law now covers the local arm too: the verbatim
     * !<...> wrap carries non-scannable suffixes. */
    const char* y = "!<!<<b,,,,,-,,,,ar> :yam";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    /* the verbatim wrap appears, not the broken local shorthand */
    int saw_verbatim = 0;
    for (size_t i = 0; i + 2 <= l1; i++) {
        if (memcmp(s1 + i, "!<", 2) == 0) {
            saw_verbatim = 1;
            break;
        }
    }
    EXPECT_TRUE(saw_verbatim) << s1;
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, FlowTokenStartQuotes) {
    /* the nightly's run 38171171360 ("? !'str a\"\\u1: -,,,,tr!st"):
     * the flow PRE-SCAN read every quote byte as a quote-open, so a
     * tag suffix or a plain carrying one ("[!'str b]", "[a'b]") died
     * as an unterminated scalar before the real scan ran — and the
     * emitted form never re-parsed. Quotes open only at token starts
     * (libyaml accepts both shapes). Anchors/aliases have NO quoting:
     * a quote in a FLOW name rejects (Psych agrees); block names ride
     * quotes (the suite's W5VH). */
    const char* ok[] = {"[a'b]",      "[a\"b]",       "[!'str b]",
                        "[a, !'x b]", "{a: !'str b}", "[!<'%27x> b]"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
    const char* rejects[] = {"[&a'b x]", "[&a\"b c]", "[*'al x]"};
    for (const char* y : rejects) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        yeptris_document_free(doc);
    }
}

TEST(Parse, TaggedExplicitKeyRoundTrips) {
    /* the crasher itself: a tagged explicit key whose plain value
     * carries quote and backslash bytes — libyaml's one-line
     * "? k : v" double-wrap shape, which must emit a form that
     * re-parses to the same tree. */
    const char* y = "? !'str a\"\\u1: -,,,,,,,,,tr!st\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, PendingResolveDedentPairRejects) {
    /* the nightly's run 38179552341 ("foooo:\n  _: 1\nname: 2\n
     * _=b: 2"): after a pending key resolved into its child map, the
     * frame kept the CHILD's content column — a dedented root pair
     * closed the child's phantom map but the next MORE-indented line
     * rode the stale frame as a phantom sibling, and the runner
     * accepted what the engine (and libyaml: "mapping values are not
     * allowed in this context") rejects. Parse and marshal must agree
     * with the engine; the legit dedent and continuation shapes ride. */
    const char* bad[] = {"a:\n  b: 1\nc: 2\n  d: 3\n", "foooo:\n  _: 1\nname: 2\n  _=b: 2\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        yeptris_document_free(doc);
        char* out = NULL;
        size_t olen = 0;
        YeptrisStatus ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT,
                                           YEPTRIS_MARSHAL_ALL_DOCS, &out, &olen);
        EXPECT_NE(ms, YEPTRIS_OK) << y;
        yeptris_marshal_free(out);
    }
    const char* good[] = {"foo:\n  bar: 1\nbaz: 2\n", "a:\n  b: 1\n  c: 2\n", "k:\n  nested: v\n"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        yeptris_document_free(doc);
    }
}

TEST(Parse, AliasMemoResetPerDocument) {
    /* the nightly's run 38266487549 ("  : &ORIGIN{}\n  *ORIGIN\n--- t:
     * *ORIGIN\n 89"): the repeat-alias memo (TODO.restructure/71)
     * answered doc 2's "*ORIGIN" from doc 1's memoized ordinal ACROSS
     * the document boundary — the whole-buffer parse emitted the
     * alias (resolving it) while the chunked feed (which resets the
     * engine per cut) correctly failed: transport changed semantics.
     * Anchors are per-document; the memo invalidates with them. */
    const char* y = ": &ORIGIN{}\n*ORIGIN\n--- t: *ORIGIN\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    EXPECT_EQ(doc, nullptr) << y; /* doc 2's alias must NOT resolve */
    yeptris_document_free(doc);
    /* within ONE document the memo still serves (the merge-key law) */
    const char* y2 = "a: &x 1\nb: *x\nc: *x\n";
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(y2, strlen(y2), &st2);
    ASSERT_NE(doc2, nullptr) << y2;
    yeptris_document_free(doc2);
}

TEST(Parse, SiblingPairFlushesBufferedExplicitKey) {
    /* issue #570 (the nightly's run 38261610465, "? &a\nt: &b\n*a"):
     * a same-column PLAIN-START pair line under an explicit key was
     * consumed as the key's CONTENT — the buffered "&a" key stranded
     * inside it, and the trailing "*a" emitted an alias to an anchor
     * that no longer existed in the output. The continuation now
     * stops at such pairs (dashes keep the old path — a block seq is
     * the key's content, the catch-38154662070 pin's shape): the null
     * value flushes, the pair proceeds, and the anchor emits. */
    const char* y = "? &a\nt: &b\n*a";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    int has_anchor = 0;
    for (size_t i = 0; i + 1 < l1; i++) {
        if (memcmp(s1 + i, "&a", 2) == 0) {
            has_anchor = 1;
        }
    }
    EXPECT_EQ(has_anchor, 1) << s1; /* the anchor emits with its key */
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
    /* the unanchored twin keeps its tree */
    const char* y2 = "? a\nb: c\n";
    YeptrisStatus st3 = YEPTRIS_OK;
    YeptrisDocument doc3 = yeptris_parse(y2, strlen(y2), &st3);
    ASSERT_NE(doc3, nullptr);
    YeptrisNode root3 = yeptris_document_root(doc3, 0);
    ASSERT_NE(root3, nullptr);
    EXPECT_EQ(yeptris_node_map_count(root3), 2u);
    yeptris_document_free(doc3);
}

TEST(Parse, PendAnchorDisplacedBySecondAnchorRejects) {
    /* catch 78 (the nightly's run 38271222289, "&a\n&a1\nF: *a" and
     * its '? &a' twin): a second props line carrying its own ANCHOR
     * displaced the pend anchor via same-line-wins — the displaced
     * anchor stayed defined in the nametab, so the trailing alias
     * emitted a reference the emitted document never defined and the
     * re-parse failed. libyaml rejects every nesting variant
     * ("did not find expected <document start>" / "did not find
     * expected key"); a TAG-only continuation stays legal. */
    const char* bad[] = {"&a\n&1\nF: *a\n", "? &a \n &a1\nF: *a\n", "k: &a\n  &b v\n",
                         "&a\n&b v\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_EQ(st, YEPTRIS_ERROR_PARSE) << y;
    }
    const char* good[] = {"&a\n!!str v\n", "k:\n  &a\n  !!str v\n"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_NE(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, PropsOnlyLineAtPairPositionRejects) {
    /* catch 79 (the nightly's run 38275208009): a props-only line at an
     * ESTABLISHED pair position ("k: 1\n!!t: &b\n  n: m\nz: *b") was
     * consumed as node props — the pair's key event vanished, the key's
     * tag leaked onto the value node, and a long-tag value then lost
     * its anchor at emit: the trailing alias emitted a reference the
     * emitted document never defined. libyaml rejects every
     * props-only-line-at-pair-position shape ("could not find expected
     * ':' while scanning a simple key"); node-position props lines
     * stay legal (the root tagged map, the first pair of a fresh map). */
    const char* bad[] = {"k: 1\n!!t: v\n", "k: 1\n!!t: &b\n  n: m\nz: *b\n", "k: 1\n!!t\n  n: m\n",
                         "k: 1\n&a\n  n: m\n", "k: 1\n&a\n- x\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_EQ(st, YEPTRIS_ERROR_PARSE) << y;
    }
    const char* good[] = {"!!t: &b\n  n: m\n", "!!t\n  n: m\n", "!!t x: v\n",
                          "a:\n  !!t: &c\n    n: m\n"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_NE(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, MultiLineFlowFloorAndMarkParity) {
    /* catch 80 (the nightly's run 38280372447): the JSON fast path's
     * floor scan stopped at the flow's close, so the closer's own line
     * escaped the suite-strict 9C9N floor whenever the span took the
     * fast path — while short spans (chunk cuts, small documents) took
     * the general kernel and rejected: whole vs chunked feeds
     * disagreed over the same bytes. The fast scan now covers the
     * closer line; the pre-scan also restores its line state with the
     * cursor, so the slow path's open event marks at the '{' exactly
     * like the fast path's. */
    const char* reject = "k: {\n\n} \n---\nk2: v\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(reject, strlen(reject), &st);
    EXPECT_EQ(doc, nullptr);
    EXPECT_EQ(st, YEPTRIS_ERROR_PARSE);
    /* compat has no floor: libyaml/Psych accept the same shape */
    YeptrisParseOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.schema = YEPTRIS_SCHEMA_11_COMPAT;
    st = YEPTRIS_OK;
    doc = yeptris_parse_ex(reject, strlen(reject), &opts, &st);
    EXPECT_NE(doc, nullptr);
    if (doc != NULL) {
        yeptris_document_free(doc);
    }
    /* the open mark matches across the fast and slow routes: a short
     * key rides the slow kernel, a 30-byte key clears the fast path's
     * span gate — both must mark the flow at the '{' */
    struct {
        const char* y;
        uint32_t open_col; /* the '{' column */
    } marks[] = {
        {"k: {\n  x: 1\n  }\n", 4},
        {"k000000000000000000000000000: {\n  x: 1\n  }\n", 31},
    };
    for (const auto& m : marks) {
        YeptrisRecorder rec = yeptris_recorder_new();
        ASSERT_NE(rec, nullptr);
        ASSERT_EQ(yeptris_recorder_feed(rec, m.y, strlen(m.y), 1), YEPTRIS_OK);
        size_t n = 0;
        const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
        int found = 0;
        for (size_t i = 0; i < n; i++) {
            if (rs[i].type == YEPTRIS_EV_MAPPING_START && (rs[i].flags & YEPTRIS_EF_FLOW)) {
                EXPECT_EQ(rs[i].line, 1u) << m.y;
                EXPECT_EQ(rs[i].col, m.open_col) << m.y;
                found = 1;
            }
        }
        EXPECT_EQ(found, 1) << m.y;
        yeptris_recorder_free(rec);
    }
}

TEST(Parse, QPairStateResetsAtDocBoundary) {
    /* catch 81 (the nightly's run 38287390219): doc 4's flow set
     * q_key_pending and the '---' boundary left it set, so doc 5's
     * bare ':' skipped its empty-key event — the whole-buffer stream
     * lost a record the chunked feed (per-run reset) emitted. The
     * boundary clears the pair state now, and the bare ':' key event
     * carries the ':' mark instead of a zeroed one. */
    const char* y = "---\n{\n a: b, : c\n}\n---\n# c\n:\n";
    YeptrisRecorder rec = yeptris_recorder_new();
    ASSERT_NE(rec, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(rec, y, strlen(y), 1), YEPTRIS_OK);
    size_t n = 0;
    const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
    int bare_map = 0, bare_key = 0, bare_val = 0;
    for (size_t i = 0; i < n; i++) {
        if (rs[i].type == YEPTRIS_EV_MAPPING_START && rs[i].line == 7) {
            bare_map = 1;
        }
        if (rs[i].type == YEPTRIS_EV_SCALAR && rs[i].line == 7 && rs[i].col == 1 &&
            rs[i].value_len == 0) {
            bare_key = 1; /* the empty key, marked at the ':' */
        }
        if (rs[i].type == YEPTRIS_EV_SCALAR && rs[i].line == 7 && rs[i].col == 2 &&
            rs[i].value_len == 0) {
            bare_val = 1; /* the empty value */
        }
    }
    EXPECT_EQ(bare_map, 1);
    EXPECT_EQ(bare_key, 1);
    EXPECT_EQ(bare_val, 1);
    yeptris_recorder_free(rec);
    /* and the whole stream matches the chunked feed record for record */
    YeptrisRecorder whole = yeptris_recorder_new();
    ASSERT_NE(whole, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(whole, y, strlen(y), 1), YEPTRIS_OK);
    size_t nw = 0;
    yeptris_recorder_records(whole, &nw);
    YeptrisRecorder ch = yeptris_recorder_new();
    ASSERT_NE(ch, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(ch, y, 9, 0), YEPTRIS_OK);
    ASSERT_EQ(yeptris_recorder_feed(ch, y + 9, strlen(y) - 9, 1), YEPTRIS_OK);
    size_t nc = 0;
    yeptris_recorder_records(ch, &nc);
    EXPECT_EQ(nw, nc);
    yeptris_recorder_free(whole);
    yeptris_recorder_free(ch);
}

TEST(Feed, BlockScalarFoldIsNeverCut) {
    /* catch 82 (the nightly's run 38291177166): the step scan had no
     * block-scalar state — a cut inside an open fold changed the
     * scalar's value ("@>\n---\n" whole vs chunked), and fold content
     * like "'* bullet" poisoned the scanner's quote state. The scan
     * now consumes a fold's lines wholesale and cuts only after the
     * reader's own fold-end rule: a dedented line that is not blank,
     * not a comment, not a marker. */
    const char* y = ">\n--- folded\n line\n\n next\n   '* bullet\n\n# Comment\n---\n";
    YeptrisRecorder whole = yeptris_recorder_new();
    ASSERT_NE(whole, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(whole, y, strlen(y), 1), YEPTRIS_OK);
    size_t nw = 0;
    const YeptrisEventRecord* rw = yeptris_recorder_records(whole, &nw);
    size_t fold_len = 0;
    for (size_t i = 0; i < nw; i++) {
        if (rw[i].type == YEPTRIS_EV_SCALAR) {
            fold_len = rw[i].value_len; /* doc 1's fold (doc 2 is empty) */
            break;
        }
    }
    EXPECT_GT(fold_len, 0u);
    YeptrisRecorder ch = yeptris_recorder_new();
    ASSERT_NE(ch, nullptr);
    size_t half = strlen(y) / 2;
    ASSERT_EQ(yeptris_recorder_feed(ch, y, half, 0), YEPTRIS_OK);
    ASSERT_EQ(yeptris_recorder_feed(ch, y + half, strlen(y) - half, 1), YEPTRIS_OK);
    size_t nc = 0;
    yeptris_recorder_records(ch, &nc);
    EXPECT_EQ(nw, nc);
    yeptris_recorder_free(whole);
    yeptris_recorder_free(ch);
}

TEST(Feed, DocInlineBlockScalarIsNeverCut) {
    /* catch 83 (the nightly's run 38352718743): the scan's header
     * check only fired at line starts, so "--- |" left the fold
     * untracked — its flow-looking content ("b,w: [|PS-<...") poisoned
     * the quote/flow state and the split diverged. Doc-inline folds
     * follow the root rule (only a later marker ends them), so the
     * marker arm now opens the state and the scan holds everything. */
    const char* y = "--- |\nb,w: [|PS-<dobe-1be-2.0\n\nb,[|PS-<dobe-2be-2.0\n\nb,w: [x]\n";
    YeptrisRecorder whole = yeptris_recorder_new();
    ASSERT_NE(whole, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(whole, y, strlen(y), 1), YEPTRIS_OK);
    size_t nw = 0;
    const YeptrisEventRecord* rw = yeptris_recorder_records(whole, &nw);
    size_t fold_len = 0;
    for (size_t i = 0; i < nw; i++) {
        if (rw[i].type == YEPTRIS_EV_SCALAR) {
            fold_len = rw[i].value_len;
            break;
        }
    }
    EXPECT_GT(fold_len, 0u);
    YeptrisRecorder ch = yeptris_recorder_new();
    ASSERT_NE(ch, nullptr);
    size_t third = strlen(y) / 3;
    ASSERT_EQ(yeptris_recorder_feed(ch, y, third, 0), YEPTRIS_OK);
    ASSERT_EQ(yeptris_recorder_feed(ch, y + third, strlen(y) - 2 * third, 0), YEPTRIS_OK);
    ASSERT_EQ(yeptris_recorder_feed(ch, y + 2 * third, third + 2, 1), YEPTRIS_OK);
    size_t nc = 0;
    yeptris_recorder_records(ch, &nc);
    EXPECT_EQ(nw, nc);
    yeptris_recorder_free(whole);
    yeptris_recorder_free(ch);
}

TEST(Parse, PropsThenKeyIndicatorOnLineRejects) {
    /* catch 84 (the nightly's run 38355778230, "? &a ? &a&a ? &a ?.
     * &a ? a\n: ?. &a ? a\n:-: *a"): node properties followed by a
     * second '?' key indicator on the SAME line opened the nested key
     * map with the props dropped — the displaced anchor stayed defined
     * in the nametab while never emitting, so the trailing alias
     * emitted a dangling reference and the round-trip failed. libyaml:
     * "mapping keys are not allowed in this context"; the cross-line
     * twin composes and the props-less "? ? b" is a legal nested key. */
    const char* bad[] = {"? &a ? b\n: v\n", "? &a ? b\n", "k: &a ? b\n", "? &a ? b : v\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_EQ(st, YEPTRIS_ERROR_PARSE) << y;
    }
    const char* good[] = {"? ? b\n: v\n", "? &a\n  ? b\n  : v\n"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_NE(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Feed, PlainContentQuoteIsNeverTracked) {
    /* catch 85 (the nightly's run 38359507422): the scan opened quote
     * state on a quote byte after any indicator char — "A!'- %YAML" is
     * a PLAIN whose apostrophe is content, but the phantom quote state
     * suppressed the "--- |" fold and the later cut orphaned the
     * "%YAML" directive from its "---", shifting the document-start
     * mark by a line. Quotes now open only at a WORD start; a word
     * that began with an indicator still admits a quoted node after
     * it ("&a 'x'", "- 'x'"). */
    const char* y = "A!'- %YAML 0.2\n--- |\n!e-2.0\n...\n%Y'AML 1.2\n---\n# Empty\n...\n";
    YeptrisRecorder whole = yeptris_recorder_new();
    ASSERT_NE(whole, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(whole, y, strlen(y), 1), YEPTRIS_OK);
    size_t nw = 0;
    yeptris_recorder_records(whole, &nw);
    YeptrisRecorder ch = yeptris_recorder_new();
    ASSERT_NE(ch, nullptr);
    size_t half = strlen(y) / 2;
    ASSERT_EQ(yeptris_recorder_feed(ch, y, half, 0), YEPTRIS_OK);
    ASSERT_EQ(yeptris_recorder_feed(ch, y + half, strlen(y) - half, 1), YEPTRIS_OK);
    size_t nc = 0;
    yeptris_recorder_records(ch, &nc);
    EXPECT_EQ(nw, nc);
    yeptris_recorder_free(whole);
    yeptris_recorder_free(ch);
    /* a quote after an indicator-led word is still tracked */
    const char* q = "&a 'x\n y'\n---\nz: 1\n";
    YeptrisRecorder w2 = yeptris_recorder_new();
    ASSERT_NE(w2, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(w2, q, strlen(q), 1), YEPTRIS_OK);
    size_t n2 = 0;
    const YeptrisEventRecord* r2 = yeptris_recorder_records(w2, &n2);
    int folded = 0;
    for (size_t i = 0; i < n2; i++) {
        if (r2[i].type == YEPTRIS_EV_SCALAR && r2[i].style == 2 /* single-quoted */) {
            folded = 1;
        }
    }
    EXPECT_EQ(folded, 1);
    yeptris_recorder_free(w2);
}

TEST(Feed, FlowAdjacentQuoteIsTracked) {
    /* catch 86 (the nightly's run 38366643713): the word-start quote
     * rule armed tok_start only across spaces, so the quote in
     * "{\"O\" ..." — adjacent to the '{' with no space — went
     * untracked: the scan counted the ']' inside the quoted scalar as
     * closing the flow, cut at a '---' that the engine never saw
     * outside the quote, and the chunked feed died on an unterminated
     * quoted scalar the whole parse had closed. Flow separators now
     * re-arm tok_start immediately. */
    const char* y = "{\"a ]\": 1}\n---\nk: v\n";
    YeptrisRecorder whole = yeptris_recorder_new();
    ASSERT_NE(whole, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(whole, y, strlen(y), 1), YEPTRIS_OK);
    size_t nw = 0;
    yeptris_recorder_records(whole, &nw);
    EXPECT_GT(nw, 4u); /* both docs parsed */
    YeptrisRecorder ch = yeptris_recorder_new();
    ASSERT_NE(ch, nullptr);
    size_t half = strlen(y) / 2;
    ASSERT_EQ(yeptris_recorder_feed(ch, y, half, 0), YEPTRIS_OK);
    ASSERT_EQ(yeptris_recorder_feed(ch, y + half, strlen(y) - half, 1), YEPTRIS_OK);
    size_t nc = 0;
    yeptris_recorder_records(ch, &nc);
    EXPECT_EQ(nw, nc);
    yeptris_recorder_free(whole);
    yeptris_recorder_free(ch);
}

TEST(Emit, ExplicitPairValueKeepsItsAnchor) {
    /* catch 87 (the nightly's run 38371134997): the block map's
     * explicit-key arm wrote "? key" then ": " and emitted the value
     * directly — skipping the simple arm's emit_props_tail + fresh
     * line for a nested block collection. An anchored map value lost
     * its anchor (or, once the props call went in, glued it to the
     * first key: "&basename"), and a later alias emitted a dangling
     * reference the re-parse rejected. */
    const char* y = "? k: &base\n   n: m\nz: *base\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    yeptris_document_free(doc);
    st = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st);
    EXPECT_NE(doc2, nullptr) << s1;
    if (doc2 != NULL) {
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
        free(s2);
        yeptris_document_free(doc2);
    }
    /* the anchor must appear in the output */
    EXPECT_NE(strstr(s1, "&base"), nullptr) << s1;
    free(s1);
}

TEST(Parse, LeadingBomContentRidesQuoted) {
    /* the nightly's run 38258674337 ("\xff\xfe\xff\xfe:\x00" — a
     * UTF-16LE double-BOM transcodes to a CONTENT U+FEFF as a plain
     * key): the plain-safety predicate let a LEADING U+FEFF ride raw —
     * the re-parse consumed it as a document BOM and the key became
     * empty, every round shedding it. Only its escape ("\uFEFF") is
     * byte-stable; mid-token BOMs ride plain. */
    const char* y = "\xef\xbb\xbf: v\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, FlowPlainsQuoteSeparators) {
    /* the nightly's run 38255134508 ("! &a\n? ~2: &)J  :(((( 6e"):
     * the flow-rendered pair's VALUE carried ": " — a second mapping
     * separator inside one flow entry ("{a: b : c}" dies at the
     * second ':' per the completed-pair law, catch 60). The gate
     * gains ':' and its explicit-key twin '?'. */
    const char* y = "! &a\n? ~2: &)J  :(((( 6e";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, FlowPlainsQuoteAnchorIndicators) {
    /* the nightly's run 38250474732 ("! &a\n? ~2: &)J  :(((( &e"):
     * the explicit key's nested pair rendered FLOW and its value
     * "&)J :(((( &e" rode PLAIN — the '&' opened an anchor scan on
     * ')' and the re-parse died ("did not find expected alphabetic or
     * numeric character while scanning an anchor"). The flow-context
     * DQ gate gains '&' and its alias twin '*'. The suite's M2N8
     * ("- ? : x" must parse) is untouched — the PARSE side keeps the
     * form (the suite outranks Psych here). */
    const char* y = "! &a\n? ~2: &)J  :(((( &e";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, PropsLineUnderExplicitKeyRejects) {
    /* the nightly's run 38207965687 ("? &a\n&F\n: *a"): a props-only
     * line continuing an explicit '?' key — the key would span lines
     * waiting for a ':' that its own props absorb, and the absorption
     * DROPPED the key's anchor so the emission wrote an alias to an
     * anchor that no longer existed. libyaml rejects the shape
     * ("could not find expected ':' while scanning a simple key");
     * we match. */
    const char* bad[] = {"? &a\n&F\n: *a", "? &e\n! &e\n: v\n", "? &e\n!!t\n: v\n",
                         "- &a\n  ? &e\n  ! &e\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        yeptris_document_free(doc);
    }
    /* the legal explicit-key shapes ride */
    const char* ok[] = {"? &a\n: *a\n", "? &a\n  b\n: v\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        yeptris_document_free(doc);
    }
}

TEST(Parse, AnchoredKeyCountsItsAnchorName) {
    /* the nightly's run 38206305189: the written-token estimate
     * counted the key's tag (catch 63) but NOT its anchor — and
     * anchor names carry interior colons (Y2GN), so a ~1700-byte
     * name rode a 2-byte key past the re-parse's 1024 law and the
     * emission never re-parsed. The anchor's written length now
     * counts; over-limit anchored keys ride the explicit form. */
    char y[1200];
    {
        size_t o = 0;
        y[o++] = '&';
        y[o++] = 'a';
        for (int i = 0; i < 1100; i++) {
            y[o++] = ':';
        }
        y[o++] = ' ';
        y[o++] = 'k';
        y[o++] = ':';
        y[o++] = ' ';
        y[o++] = 'v';
        y[o] = '\0';
    }
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(s1[0], '?') << s1; /* the explicit form */
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, MidTokenHashIsContentInPropLines) {
    /* the nightly's run 38204470906 ("  ? e\n  ? &e ?#"): the props-
     * line classifier treated a MID-TOKEN '#' as a comment — "&a ?#:"
     * scanned as props+comment (only_props) and failed, while the
     * same plain parses fine unanchored ("?# : v"). '#' opens a
     * comment only after a blank; mid-token it is content (libyaml
     * accepts "&a ?#:" as {a: "?#"} shapes). The emit re-spelled the
     * explicit key as "&a ?#:" — a form our own parser then rejected.
     */
    const char* ok[] = {"&a ?#: v\n", "e:\n&a ?#x:\n", "e:\n&a x#:\n", "?#:\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
    /* a '#' after a blank is still a comment: props-only lines keep
     * their law (the S2900 family regression guard) */
    const char* bad[] = {"k: &x\n!!map\na: b\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        yeptris_document_free(doc);
    }
}

TEST(Parse, DeepAnchoredPendingRidesTheEngine) {
    /* the nightly's run 38202382251 ("4:\n4: r\n5:\n &5\n 5: e"): an
     * anchored pending BELOW the root frame dropped its pairs at EOF —
     * the tree lost the anchored map and every round shed bytes. The
     * deferral only flushes at depth 1; deeper anchored pendings now
     * bail to the engine, which resolves them. The depth-1 anchored
     * shape keeps the fused path. */
    const char* y = "4:\n4: r\n5:\n &5\n 5: e";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_map_count(root), 3u) << "the anchored map must survive";
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
    /* the depth-1 anchored pending keeps the fused path */
    const char* y2 = "b: &5\n  c: d\n";
    YeptrisStatus st3 = YEPTRIS_OK;
    YeptrisDocument doc3 = yeptris_parse(y2, strlen(y2), &st3);
    ASSERT_NE(doc3, nullptr);
    YeptrisNode root3 = yeptris_document_root(doc3, 0);
    ASSERT_NE(root3, nullptr);
    YeptrisNode k = NULL, v = NULL;
    ASSERT_EQ(yeptris_node_map_at(root3, 0, &k, &v), 0);
    EXPECT_NE(v, nullptr);
    yeptris_document_free(doc3);
}

TEST(Parse, TaggedEmptyKeyRidesTheExplicitForm) {
    /* the nightly's run 38200627503: the written-token estimate
     * EARLY-RETURNED on an empty key value, before the tag arm — a
     * TAGGED EMPTY key wrote ~2300 bytes of escaped tag as a simple
     * key and the emission died at the re-parse's 1024 law. The tag
     * arm must run for empty keys too. */
    char y[512]; /* "!<400 underscores> : v" — a TAGGED EMPTY key */
    {
        size_t o = 0;
        y[o++] = '!';
        y[o++] = '!';
        y[o++] = 'm';
        y[o++] = 'a';
        y[o++] = 'p';
        for (int i = 0; i < 400; i++) {
            y[o++] = '_';
        }
        y[o++] = ' ';
        y[o++] = ':';
        y[o++] = ' ';
        y[o++] = 'v';
        y[o] = '\0';
    }
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(s1[0], '?') << s1; /* the explicit form */
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, MultilineTaggedBlockKeyRidesSimple) {
    /* the nightly's run 38181819580 (":\n! |\n r\n:"): the clip-chomp
     * twin of catch 64's key — its content keeps the trailing break,
     * so the style chooser demotes it to QUOTED even in the explicit
     * path (as_key), and promoting it would UNSTABILIZE the emission
     * (round two parses a quoted style and rides simple). Only
     * SINGLE-LINE tagged block keys keep the block form; the
     * multiline ones ride simple in both rounds, stably. */
    const char* y = ":\n! |\n r\n:";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(strstr(s1, "? "), (char*)NULL) << s1; /* the simple form */
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, TaggedBlockKeyRidesTheExplicitForm) {
    /* the nightly's run 38177622733 (":\n! |-\n r\n:"): a TAGGED
     * literal block keeps the block form in sc_route even single-line
     * (the #290 law), so as a map KEY its value separator glued onto
     * the block's CONTENT line (colons are data there) — the re-parse
     * read a different tree and every round shed bytes. Such keys ride
     * the explicit form, which keeps the block form too — both rounds
     * agree. Untagged/empty block styles demote to quoted/plain and
     * ride simple stably (the suite's 5WE3). */
    const char* y = ":\n! |-\n r\n:";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
    /* the props-emit-once law, explicit-key side: the key's tag rode
     * "? " AND the node's own header — "? !\n  ! |-..." */
    const char* y2 = "a: !x |\n  k\n: v\n";
    YeptrisStatus st3 = YEPTRIS_OK;
    YeptrisDocument doc3 = yeptris_parse(y2, strlen(y2), &st3);
    ASSERT_NE(doc3, nullptr);
    size_t l3 = 0;
    char* s3 = yeptris_serialize(doc3, &l3);
    ASSERT_NE(s3, nullptr);
    int tags = 0;
    for (size_t i = 0; i + 2 < l3; i++) {
        if (memcmp(s3 + i, "!x", 2) == 0) {
            tags++;
        }
    }
    EXPECT_EQ(tags, 1) << s3;
    free(s3);
    yeptris_document_free(doc3);
}

TEST(Parse, TaggedKeyRidesTheExplicitFormWhenEscapedOverLimit) {
    /* the nightly's run 38176016091: a key carrying a ~975-byte tag
     * whose raw bytes escape as %XX (wr_tag_uri escapes even '_') —
     * the written token inflated past 1024 and the emission never
     * re-parsed ("simple key exceeds 1024"). The written-token law
     * counts the TAG too; an over-limit tagged key rides the explicit
     * form. A short-tagged key stays simple. */
    {
        std::string y = "!!map";
        y.append(340, '_');  /* escapes to 1020 tag bytes written */
        y.append(" k: v\n"); /* the tag rides the KEY, as in the crasher */
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y.data(), y.size(), &st);
        ASSERT_NE(doc, nullptr);
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr);
        EXPECT_EQ(s1[0], '?') << s1; /* the explicit form */
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
    {
        /* props BEFORE the key line-start tag the key itself */
        const char* y = "!!map k: v\n";
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr);
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr);
        EXPECT_EQ(s1[0], '!') << s1; /* still the simple form */
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, FlowPairValueColonRejects) {
    /* the nightly's run 38168927841 (":\n!\n? [s: 2:]\n: -"): the
     * engine accepted "[s: 2:]" and nested a second single-pair
     * wrapper inside the first pair's VALUE slot — unbalanced events,
     * the DOM silently dropped the "2". libyaml rejects every
     * completed-pair-plus-colon shape; the legal multi-pair and
     * trailing-comma shapes must keep parsing. */
    const char* bad[] = {"[s: 2:]",     "[a: b: c]", "[s: : v]",
                         "[s: [b]: c]", "[s: 2\n:]", "[a\n: b: c]"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        yeptris_document_free(doc);
    }
    const char* good[] = {"[a: b, c: d]", "[s: , x]", "[s:]", "{a: b}", "[a: b]"};
    for (const char* y : good) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        yeptris_document_free(doc);
    }
}

TEST(Parse, NestedBlockCollectionRidesFlowEmit) {
    /* a block-styled collection placed inside a flow member (the
     * mutation API builds this freely) must render FLOW — its block
     * dash is not node content inside brackets. The pre-fix emit
     * wrote "- " under "[" and the output never re-parsed. */
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse("[a]", 3, &st);
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    YeptrisNode child = yeptris_node_new_sequence(doc);
    ASSERT_NE(child, nullptr);
    YeptrisNode scalar = yeptris_node_new_scalar(doc, "b", 1, YEPTRIS_STYLE_PLAIN);
    ASSERT_NE(scalar, nullptr);
    ASSERT_EQ(yeptris_node_seq_add(child, scalar), 0);
    ASSERT_EQ(yeptris_node_seq_add(root, child), 0);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    int has_member = 0;
    for (size_t i = 0; i + 3 < l1; i++) {
        if (memcmp(s1 + i, "[b]", 3) == 0) {
            has_member = 1;
        }
    }
    EXPECT_EQ(has_member, 1) << s1;
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, RootPropsEmitOnce) {
    /* the nightly's run 38167237445 ("! &a\n  ? &e)J  !((( &e..."):
     * the root-collection props rode their own leading line AND the
     * empty collection re-emitted them inline — "! &a\n! &a {}" —
     * and every round shed a line. The catch-56 law, root side: an
     * empty propertied collection takes the inline empty-form. */
    const char* y = "! &a\n  ? &e)J  !((( &e\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    int anchors = 0;
    for (size_t i = 0; i + 2 < l1; i++) {
        if (memcmp(s1 + i, "&a", 2) == 0) {
            anchors++;
        }
    }
    EXPECT_EQ(anchors, 1) << s1;
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, RootDedentBelowRootIndentIsSecondRoot) {
    /* corrected law (catch 39, run 38115641999, " k2: v2\nk1: v\n
     * k2: v2"): a line that DEDENTS below a root map's own column is a
     * SECOND ROOT without "---" — an error in libyaml ("did not find
     * expected <document start>"), Psych's load_stream, and the 1.2
     * grammar; Psych.load only "accepts" because it stops reading at
     * the first document end. The earlier catch-31 tolerance (flat
     * siblings) diverged from every reference and made the engine and
     * runner disagree (the runner flat, the engine nested). Both
     * routes now reject; the suite has no case pinning the shape. */
    const char* ins[] = {" k2: v2\nk1: v\n k2: v2", /* the catch */
                         " k2: v2\nk1: v\n k2: v2\n",
                         " i1ak: ?o\nfr: ?ff\n", /* catch 31 */
                         " a: x\nb: y\n",
                         "  k: v\nj: w\n",
                         " a:\nb: 2\n",  /* pending key */
                         " a: 1\n- b\n", /* dash line */
                         " a: 1\nb",     /* bare scalar */
                         " - a\n- b\n"}; /* seq variant */
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        int ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT, YEPTRIS_MARSHAL_ALL_DOCS,
                                 &out, &olen);
        EXPECT_NE(ms, 0) << y; /* parseable-implies-marshalable, both ways */
        free(out);
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    /* the LEGAL neighbors stay accepted: same-column siblings (with a
     * comment between), markers after root content, nesting */
    const char* ok[] = {" a: 1\n b: 2\n", " a: 1\n# c\n b: 2\n", " a: 1\n--- b\n",
                        " a: 1\n...\n",   " - a\n - b\n",        "a:\n  b: 1\nc: 2\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, NullFlowSeqItemEmitsTilde) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-11, run
     * 38106246351, "? - \n  - 2002-07-*2Yon"): a null first item in a
     * FLOW sequence emitted as nothing — "[, x]", which libyaml and
     * Psych alike reject ("did not find expected node content"). The
     * flow-seq item position now rides '~'. */
    const char* y = "? - \n  - 2002-07-*2Yon\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    int tilde_first = 0;
    for (size_t i = 0; i + 2 <= l1; i++) {
        if (memcmp(s1 + i, "[~", 2) == 0) {
            tilde_first = 1;
            break;
        }
    }
    EXPECT_TRUE(tilde_first) << s1;
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0);
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, FlowPlainQuotesAndEscapesTakeTheQuotedRoute) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-11, run
     * 38107485738, "? !str a\"\u1: -,,..."): a plain scalar carrying
     * '"' and '\' rode plain INSIDE a flow map, where the bytes flip
     * the scanner into quoted/escape mode ("unterminated quoted
     * scalar"). The catch-25 flow gate now covers them. */
    const char* y = "? !str a\"\\u1: -,,,,,,,,,:0tr!st\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
    ASSERT_NE(doc2, nullptr) << s1;
    size_t l2 = 0;
    char* s2 = yeptris_serialize(doc2, &l2);
    EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0);
    free(s2);
    yeptris_document_free(doc2);
    free(s1);
    yeptris_document_free(doc);
}

TEST(Parse, PlainPrefixDocumentMarkersQuote) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-11, run
     * 38109609214, "--- --- >\nlfoo ]..."): a multi-line plain whose
     * folded value STARTS with "--- " emitted plain — at line start it
     * re-reads as a document marker. The bare-marker check now covers
     * the prefix forms ("--- x", "... x"); libyaml quotes both. */
    const char* ins[] = {"--- --- >\nlfoo ]\nin>\na  ]\nine3\n", "--- a: --- x\n", "k: ... x\n"};
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, ChainedSameColumnPendingsKeepTheTree) {
    /* the nightly fuzz_roundtrip byte-instability trap (2026-10-11,
     * run 38110714729, "a:\na:\n  b:\n  f: g\nh: i:Q\n i"): a chained
     * same-column pending ("a:" then "a:") whose container carries
     * valued pairs corrupted the runner's frame mapping — the OPEN'd
     * map swallowed the second key as the first pair's value
     * ("a: a"). The runner now bails the shape to the engine, whose
     * tree is byte-identical to the input. */
    const char* ins[] = {"a:\na:\n  b: c\nh: v\n", "x:\na:\n  b: c\nh: v\n",
                         "a:\na:\n  b:\n  f: g\nh: i:Q\n i\n"};
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        /* the tree round-trips: the second serialization is byte-equal
         * after the first normalizes the multiline plain */
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, EmptyKeyLineIsNotAValue) {
    /* the nightly fuzz_roundtrip byte-instability trap (2026-10-09,
     * run 38001436710, input "- &a\n :"): a ':'-led (empty-key) line
     * arriving as following-lines VALUE content was accepted and each
     * serialize round drifted. libyaml errors on every form of this
     * family ("did not find expected key"); the legitimate empty-key
     * document forms (root ': v', '? :') do NOT pass through the
     * following-lines arm and are unchanged. */
    const char* bad[] = {"- &a\n :", "&a\n :", "- &a\n  : ", "- &b &a\n :", "a:\n  : v"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_NE(st, YEPTRIS_OK) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"? a\n: b"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_NE(doc, nullptr) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, RunnerSeqItemPlainFirst) {
    /* the nightly fuzz_roundtrip marshal trap (2026-10-09, run
     * 38004241214, input "x:\n- ]\"#"): the fused runner's sequence
     * item capture accepted a plain item starting with ']' — parse
     * OK, marshal reject. Psych rejects every form. The item bail now
     * requires yep_plain_first_ok like the key/value sites. */
    const char* bad[] = {"x:\n- ]\"#", "- ]", "- ,x", "- }x", "- @x", "- `x"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_NE(st, YEPTRIS_OK) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    /* a plain item with an INTERIOR quote stays legal */
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse("a: b\"c", 6, &st2);
    EXPECT_NE(doc2, nullptr);
    if (doc2 != NULL) {
        yeptris_document_free(doc2);
    }
}

TEST(Parse, NoncharactersEscapeInOutput) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-10, run
     * 38012213661, input "[\"\\uFFFE\"]"): the printable validator
     * rejects raw U+FFFE/U+FFFF, but their \\u ESCAPES pass as input;
     * emitted raw, the document became un-re-parseable. The emitter
     * now re-escapes the pair (libyaml's own form). */
    const char* ins[] = {"[\"\\uFFFE\"]", "[\"\\uFFFF\"]", "\"\\uFFFE\""};
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, MarshalHonorsTheEncodingGate) {
    /* the nightly fuzz_roundtrip marshal trap (2026-10-10, run
     * 38013837979, a UTF-16BE doc whose raw bytes contain "-s"):
     * the value-drain entry (marshal/visit/drain) fed RAW UTF-16/32
     * bytes to the engine while yeptris_parse accepted them via the
     * transcode front-end — parseable-implies-marshalable broken.
     * The drains now share the parse entry's encoding gate. */
    static const unsigned char bytes[] = {0xfe, 0xff, 0x36, 0x01, 0x75, 0x73, 0x3a,
                                          0x0a, 0x2d, 0x73, 0xff, 0xf6, 0x2c, 0x61};
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse((const char*)bytes, sizeof bytes, &st);
    ASSERT_NE(doc, nullptr);
    char* out = NULL;
    size_t olen = 0;
    YeptrisStatus ms = yeptris_marshal((const char*)bytes, sizeof bytes, YEPTRIS_SCHEMA_11_COMPAT,
                                       YEPTRIS_MARSHAL_ALL_DOCS, &out, &olen);
    EXPECT_EQ(ms, YEPTRIS_OK) << "parseable input must be marshalable";
    EXPECT_GT(olen, 0u);
    yeptris_marshal_free(out);
    yeptris_document_free(doc);
}

TEST(Parse, LineEndDashIsAnIndicator) {
    /* the nightly fuzz_roundtrip byte-instability trap (2026-10-10,
     * run 38015183615, input "n:\n- -\t"): the runner's nested-dash
     * detection required a FOLLOWING blank byte, so a line-ending '-'
     * was captured as the plain scalar "-" — parse of the emitted
     * form then disagreed with parse of the input (tab-form [[nil]]
     * vs space-form [["-"]]). A dash at line end is an indicator;
     * both forms now parse [[nil]] like Psych and round-trip stable. */
    const char* ins[] = {"n:\n- -\t", "n:\n- -", "a:\n- -\t", "- -"};
    for (const char* y : ins) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, DocEndMarkerOwnsItsLine) {
    /* the nightly fuzz_roundtrip marshal trap (2026-10-10, run
     * 38016589629, "a: b\n... vi an :  }"): the fused runners had no
     * document-marker law — a '...' line carrying content was
     * swallowed as a plain pair while the engine (and Psych) reject.
     * Both runners now hand marker lines to the engine. */
    const char* bad[] = {"a: b\n... vi an :  }\n", "Map&nopine: Doals\n... vi an :  }\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_NE(st, YEPTRIS_OK) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"a: b\n...\n", "a: b\n... \n", "Map&nopine: Doals\n"};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        char* out = NULL;
        size_t olen = 0;
        YeptrisStatus ms = yeptris_marshal(y, strlen(y), YEPTRIS_SCHEMA_11_COMPAT,
                                           YEPTRIS_MARSHAL_ALL_DOCS, &out, &olen);
        EXPECT_EQ(ms, YEPTRIS_OK) << y;
        yeptris_marshal_free(out);
        yeptris_document_free(doc);
    }
}

TEST(Parse, FlatRunKeyPlainFirst) {
    /* the nightly fuzz_roundtrip marshal trap (2026-10-10, run
     * 38018116500, "<: .ba\n]ge: 21"): the flat-run's line-head bail
     * listed '[' but never enforced plain-first — a ']' sibling key
     * was captured (parse OK) while the marshal rejected. Psych
     * rejects the family. */
    const char* bad[] = {"<: .ba\n]ge: 21\n", "]ge: 21\n", "a:\n  ]ge: 21\n", "k: ,v\n"};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_NE(st, YEPTRIS_OK) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
}

TEST(Parse, UEscapesStayUnder10FFFF) {
    /* the nightly fuzz_roundtrip re-parse trap (2026-10-10, run
     * 38051533028, "\"\\UA66DA66D\""): an 8-digit \U escape can name
     * code points far past U+10FFFF — parse accepted an unencodable
     * one and the emitted bytes never re-parsed. Psych rejects above
     * the boundary; U+10FFFF itself accepts. */
    const char* bad[] = {"\"\\UA66DA66D\"", "\"\\U00110000\""};
    for (const char* y : bad) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << y;
        EXPECT_NE(st, YEPTRIS_OK) << y;
        if (doc != NULL) {
            yeptris_document_free(doc);
        }
    }
    const char* ok[] = {"\"\\U0010FFFF\"", "\"\\U0001F600\""};
    for (const char* y : ok) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << y;
        size_t l1 = 0;
        char* s1 = yeptris_serialize(doc, &l1);
        ASSERT_NE(s1, nullptr) << y;
        YeptrisStatus st2 = YEPTRIS_OK;
        YeptrisDocument doc2 = yeptris_parse(s1, l1, &st2);
        ASSERT_NE(doc2, nullptr) << y << " -> " << s1;
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << y;
        free(s2);
        yeptris_document_free(doc2);
        free(s1);
        yeptris_document_free(doc);
    }
}

TEST(Parse, ScalarRoot) {
    const char* y = "hello";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    ASSERT_EQ(st, YEPTRIS_OK);
    EXPECT_EQ(yeptris_document_count(doc), 1u);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_kind(root), YEPTRIS_NODE_SCALAR);
    EXPECT_EQ(val(root), "hello");
    EXPECT_EQ(yeptris_node_style(root), YEPTRIS_STYLE_PLAIN);
    yeptris_document_free(doc);
}

TEST(Parse, BlockMapping) {
    const char* y = "name: yeptris\nlang: c\nfast: true";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_kind(root), YEPTRIS_NODE_MAPPING);
    EXPECT_EQ(yeptris_node_map_count(root), 3u);
    EXPECT_EQ(map_str(root, "name"), "yeptris");
    EXPECT_EQ(map_str(root, "lang"), "c");
    EXPECT_EQ(map_str(root, "fast"), "true");
    yeptris_document_free(doc);
}

TEST(Parse, NestedBlock) {
    const char* y = "outer:\n"
                    "  middle:\n"
                    "    inner: 1\n"
                    "  sibling: 2\n"
                    "after: 3\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_map_count(root), 2u);
    YeptrisNode outer = yeptris_node_map_get(root, "outer", 5);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(yeptris_node_kind(outer), YEPTRIS_NODE_MAPPING);
    EXPECT_EQ(map_str(root, "after"), "3");
    YeptrisNode middle = yeptris_node_map_get(outer, "middle", 6);
    ASSERT_NE(middle, nullptr);
    EXPECT_EQ(map_str(middle, "inner"), "1");
    EXPECT_EQ(map_str(outer, "sibling"), "2");
    yeptris_document_free(doc);
}

TEST(Parse, SequencesAndCompact) {
    const char* y = "- one\n"
                    "- two\n"
                    "-\n"
                    "  - nested\n"
                    "  - items\n"
                    "- key: value\n"
                    "  k2: v2\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_kind(root), YEPTRIS_NODE_SEQUENCE);
    EXPECT_EQ(yeptris_node_seq_count(root), 4u);
    EXPECT_EQ(val(yeptris_node_seq_at(root, 0)), "one");
    EXPECT_EQ(val(yeptris_node_seq_at(root, 1)), "two");
    YeptrisNode nested = yeptris_node_seq_at(root, 2);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(yeptris_node_kind(nested), YEPTRIS_NODE_SEQUENCE);
    EXPECT_EQ(yeptris_node_seq_count(nested), 2u);
    EXPECT_EQ(val(yeptris_node_seq_at(nested, 1)), "items");
    YeptrisNode compact = yeptris_node_seq_at(root, 3);
    ASSERT_NE(compact, nullptr);
    EXPECT_EQ(yeptris_node_kind(compact), YEPTRIS_NODE_MAPPING);
    EXPECT_EQ(map_str(compact, "key"), "value");
    EXPECT_EQ(map_str(compact, "k2"), "v2");
    yeptris_document_free(doc);
}

TEST(Parse, IndentlessSequenceValue) {
    const char* y = "key:\n"
                    "- a\n"
                    "- b\n"
                    "other: 1\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_map_count(root), 2u);
    YeptrisNode seq = yeptris_node_map_get(root, "key", 3);
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(yeptris_node_kind(seq), YEPTRIS_NODE_SEQUENCE);
    EXPECT_EQ(yeptris_node_seq_count(seq), 2u);
    EXPECT_EQ(val(yeptris_node_seq_at(seq, 0)), "a");
    EXPECT_EQ(map_str(root, "other"), "1");
    yeptris_document_free(doc);
}

TEST(Parse, FlowCollections) {
    const char* y = "list: [1, two, \"three\", [4, 5]]\n"
                    "map: {a: 1, b: [x, y], c: {d: 2}}\n"
                    "empty_list: []\n"
                    "empty_map: {}\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_map_count(root), 4u);

    YeptrisNode list = yeptris_node_map_get(root, "list", 4);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(yeptris_node_kind(list), YEPTRIS_NODE_SEQUENCE);
    EXPECT_EQ(yeptris_node_seq_count(list), 4u);
    EXPECT_EQ(val(yeptris_node_seq_at(list, 0)), "1");
    EXPECT_EQ(val(yeptris_node_seq_at(list, 1)), "two");
    EXPECT_EQ(val(yeptris_node_seq_at(list, 2)), "three");
    YeptrisNode inner = yeptris_node_seq_at(list, 3);
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(yeptris_node_seq_count(inner), 2u);
    EXPECT_EQ(val(yeptris_node_seq_at(inner, 0)), "4");

    YeptrisNode map = yeptris_node_map_get(root, "map", 3);
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(yeptris_node_kind(map), YEPTRIS_NODE_MAPPING);
    EXPECT_EQ(yeptris_node_map_count(map), 3u);
    EXPECT_EQ(map_str(map, "a"), "1");
    YeptrisNode bseq = yeptris_node_map_get(map, "b", 1);
    ASSERT_NE(bseq, nullptr);
    EXPECT_EQ(yeptris_node_seq_count(bseq), 2u);
    EXPECT_EQ(val(yeptris_node_seq_at(bseq, 1)), "y");
    YeptrisNode cmap = yeptris_node_map_get(map, "c", 1);
    ASSERT_NE(cmap, nullptr);
    EXPECT_EQ(map_str(cmap, "d"), "2");

    YeptrisNode el = yeptris_node_map_get(root, "empty_list", 10);
    ASSERT_NE(el, nullptr);
    EXPECT_EQ(yeptris_node_seq_count(el), 0u);
    YeptrisNode em = yeptris_node_map_get(root, "empty_map", 9);
    ASSERT_NE(em, nullptr);
    EXPECT_EQ(yeptris_node_map_count(em), 0u);
    yeptris_document_free(doc);
}

TEST(Parse, QuotedScalars) {
    const char* y = "single: 'it''s here'\n"
                    "double: \"line\\nbreak\\ttab\"\n"
                    "unicode: \"\\x41\\u00e9\"\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(map_str(root, "single"), "it's here");
    std::string d = map_str(root, "double");
    EXPECT_EQ(d, std::string("line\nbreak\ttab"));
    EXPECT_EQ(map_str(root, "unicode"), "Aé");
    yeptris_document_free(doc);
}

TEST(Parse, PlainMultilineFold) {
    const char* y = "key: this is\n"
                    "  a folded\n"
                    "  plain scalar\n"
                    "other: 1\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(map_str(root, "key"), "this is a folded plain scalar");
    EXPECT_EQ(map_str(root, "other"), "1");
    yeptris_document_free(doc);
}

TEST(Parse, BlockScalars) {
    const char* y = "lit: |\n"
                    "  line one\n"
                    "  line two\n"
                    "folded: >\n"
                    "  folds\n"
                    "  into one\n"
                    "strip: |-\n"
                    "  no newline\n"
                    "keep: |+\n"
                    "  keep\n"
                    "\n"
                    "\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(map_str(root, "lit"), "line one\nline two\n");
    EXPECT_EQ(map_str(root, "folded"), "folds into one\n");
    EXPECT_EQ(map_str(root, "strip"), "no newline");
    EXPECT_EQ(map_str(root, "keep"), "keep\n\n\n");
    yeptris_document_free(doc);
}

TEST(Parse, PlainStopSetsMatchRuntimeBuild) {
    /* the constants must equal what the old per-call build produced */
    unsigned char b[32], f[32];
    yep_stopset_clear(b);
    yep_stopset_add(b, '\n');
    yep_stopset_add(b, '\r');
    yep_stopset_add(b, ':');
    yep_stopset_add(b, '#');
    yep_stopset_clear(f);
    memcpy(f, b, 32);
    const unsigned char fs[5] = {',', '[', ']', '{', '}'};
    for (int i = 0; i < 5; i++) {
        yep_stopset_add(f, fs[i]);
    }
    yep_stopset ss;
    yep_stopset_init(&ss, b);
    EXPECT_EQ(0, memcmp(&yep_plain_stop_block, &ss, sizeof(ss)));
    yep_stopset_init(&ss, f);
    EXPECT_EQ(0, memcmp(&yep_plain_stop_flow, &ss, sizeof(ss)));

    /* the break set (\n/\r) — the engine's quote path shares it */
    unsigned char brk[32];
    yep_stopset_clear(brk);
    yep_stopset_add(brk, '\n');
    yep_stopset_add(brk, '\r');
    yep_stopset_init(&ss, brk);
    EXPECT_EQ(0, memcmp(&yep_break_stopset, &ss, sizeof(ss)));
}

namespace {
int ref_colon_terminates(const char* p, size_t len, size_t colon, int flow) {
    size_t next = colon + 1;
    if (next >= len) {
        return 1;
    }
    unsigned char c = (unsigned char)p[next];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        return 1;
    }
    if (flow && yep_ct_is(c, YEP_CT_FLOW_IND)) {
        return 1;
    }
    return 0;
}

/* The byte-loop scan_plain the SWAR tiny path must match span for span. */
yep_span scan_plain_reference(const char* p, size_t len, size_t pos, int flow) {
    const yep_stopset* stop = flow ? &yep_plain_stop_flow : &yep_plain_stop_block;
    yep_span s;
    s.start = (uint32_t)pos;
    s.end = (uint32_t)pos;
    s.term = YEP_TERM_EOF;
    size_t i = pos;
    while (i < len) {
        size_t at = i;
        while (at < len && !yep_stopset_test(stop->bitmap, (unsigned char)p[at])) {
            at++;
        }
        unsigned char c = (at < len) ? (unsigned char)p[at] : 0;
        if (at == len) {
            i = len;
            s.term = YEP_TERM_EOF;
            break;
        }
        if (c == '\n' || c == '\r') {
            i = at;
            s.term = YEP_TERM_EOL;
            break;
        }
        if (c == ':') {
            if (ref_colon_terminates(p, len, at, flow)) {
                i = at;
                s.term = YEP_TERM_COLON;
                break;
            }
            i = at + 1;
            continue;
        }
        if (c == '#') {
            if (at == s.start || (at > s.start && (p[at - 1] == ' ' || p[at - 1] == '\t'))) {
                i = at;
                s.term = YEP_TERM_COMMENT;
                break;
            }
            i = at + 1;
            continue;
        }
        i = at;
        s.term = YEP_TERM_FLOW;
        break;
    }
    size_t e = i;
    while (e > s.start && (p[e - 1] == ' ' || p[e - 1] == '\t')) {
        e--;
    }
    s.end = (uint32_t)e;
    return s;
}
} /* namespace */

TEST(Parse, ScanPlainSwarTinyMatchesByteReference) {
    uint64_t seed = 0x9E3779B9ull;
    auto next = [&seed]() {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return (uint32_t)(seed >> 33);
    };
    auto check = [&](const std::string& s, size_t pos, int flow) {
        yep_span got = yep_scan_plain(s.data(), s.size(), pos, flow);
        yep_span want = scan_plain_reference(s.data(), s.size(), pos, flow);
        ASSERT_EQ(got.start, want.start) << "pos=" << pos << " flow=" << flow << " s='" << s << "'";
        ASSERT_EQ(got.end, want.end) << "pos=" << pos << " flow=" << flow << " s='" << s << "'";
        ASSERT_EQ(got.term, want.term) << "pos=" << pos << " flow=" << flow << " s='" << s << "'";
    };
    /* exhaustive over a 4-char interaction core (colon/hash/space/lit),
     * lengths crossing the 8-byte word boundary, both stop sets */
    const std::string core = "a:#,";
    for (int flow = 0; flow <= 1; flow++) {
        for (uint32_t len = 0; len <= 24; len++) {
            uint64_t variants = 1ull << (2 * (len > 8 ? 8 : len));
            for (uint64_t v = 0; v < variants; v++) {
                std::string s;
                uint64_t bits = v;
                for (uint32_t j = 0; j < len; j++) {
                    s += core[bits & 3u];
                    bits >>= 2;
                    if (j == 7 && len > 8) {
                        s += core[(v >> 13) & 3u]; /* word-boundary flavors */
                    }
                }
                check(s, 0, flow);
            }
            /* random strings over the full member + filler alphabet */
            const std::string alphabet = "ab: #\n\t\r,[]{}X\x01\x7f";
            for (int r = 0; r < 300; r++) {
                std::string s;
                uint32_t n = 1 + next() % 100;
                for (uint32_t j = 0; j < n; j++) {
                    s += alphabet[next() % alphabet.size()];
                }
                size_t pos = (r % 3 == 0) ? (next() % s.size()) : 0;
                check(s, pos, flow);
            }
        }
    }
    /* directed spans: multi-hit words, mid-token '#', non-terminating ':' */
    check("aaaa:bbbb:cccc\nd", 0, 0);
    check("aaa#bbb: c", 0, 0);
    check("a:b:c ", 0, 0);
    check("k:  v ", 3, 0);
    check("a,b]c{d}e:f", 0, 1);
    check("x:a[b]c{d}e,f g", 1, 1);
    check("trailing spaces   ", 0, 0);
    check("a{b}c", 0, 0); /* flow members do not stop block scans */
}

TEST(Parse, NormBreakLeadsMatchRuntimeBuild) {
    /* the literal must equal what yep_stopset_init builds for {C2, E2}
     * (the Unicode break lead bytes) — a drift would silently skip the
     * NEL/LS/PS normalization on every parse */
    unsigned char b[32];
    yep_stopset_clear(b);
    yep_stopset_add(b, 0xC2);
    yep_stopset_add(b, 0xE2);
    yep_stopset ss;
    yep_stopset_init(&ss, b);
    EXPECT_EQ(0, memcmp(&yep_norm_break_leads, &ss, sizeof(ss)));
}

TEST(Parse, UnicodeBreaksNormalizeLookalikesStay) {
    /* real NEL/LS/PS normalize to a line break (folded plain picks up
     * a space); the lookalike byte runs must ride through untouched */
    /* NEL/LS/PS normalize to a line break BEFORE parsing, so each one
     * below is exactly a '\n' in libyaml terms: an indented plain
     * continuation folds with a space, and a break between pairs
     * creates a second pair. The lookalike byte runs must ride through
     * untouched (the detector's real predicate). */
    struct {
        const char* y;
        const char* want;
    } cases[] = {
        {"k: a\xC2\x85"
         " b\n",
         "a b"}, /* NEL fold */
        {"k: a\xE2\x80\xA8"
         " b\n",
         "a b"}, /* LS fold */
        {"k: a\xE2\x80\xA9"
         " b\n",
         "a b"}, /* PS fold */
        {"k: 012345678\xC2\x85"
         " x\n",
         "012345678 x"},                           /* past the byte loop's word */
        {"k: \xC2\xA0x\n", "\xC2\xA0x"},           /* NBSP: C2 lead, not a break */
        {"k: v\xE2\x80\x93x\n", "v\xE2\x80\x93x"}, /* en dash: E2 80, not A8/A9 */
    };
    for (const auto& c : cases) {
        YeptrisStatus st;
        YeptrisDocument doc = yeptris_parse(c.y, strlen(c.y), &st);
        ASSERT_NE(doc, nullptr) << "input=" << c.y << " err=" << yeptris_last_error(NULL, NULL);
        YeptrisNode root = yeptris_document_root(doc, 0);
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(map_str(root, "k"), c.want) << c.y;
        yeptris_document_free(doc);
    }
    /* the pair-splitting shape: the NEL line break ends the 'k' pair */
    {
        const char* y = "k: v\xC2\x85"
                        "j: w\n";
        YeptrisStatus st;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
        YeptrisNode root = yeptris_document_root(doc, 0);
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(yeptris_node_map_count(root), 2u);
        EXPECT_EQ(map_str(root, "k"), "v");
        EXPECT_EQ(map_str(root, "j"), "w");
        yeptris_document_free(doc);
    }
}
TEST(Parse, AnchorsAndAliases) {
    const char* y = "base: &b\n"
                    "  x: 1\n"
                    "same: *b\n"
                    "scalar_anchor: &s hello\n"
                    "ref: *s\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    YeptrisNode base = yeptris_node_map_get(root, "base", 4);
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(yeptris_node_kind(base), YEPTRIS_NODE_MAPPING);
    size_t alen = 0;
    const char* a = yeptris_node_anchor(base, &alen);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(std::string(a, alen), "b");
    YeptrisNode same = yeptris_node_map_get(root, "same", 4);
    ASSERT_NE(same, nullptr);
    EXPECT_EQ(yeptris_node_kind(same), YEPTRIS_NODE_ALIAS);
    YeptrisNode target = yeptris_node_alias_target(same);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(yeptris_node_kind(target), YEPTRIS_NODE_MAPPING);
    /* *s aliases a scalar: the value comes from the target. */
    YeptrisNode ref = yeptris_node_map_get(root, "ref", 3);
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(yeptris_node_kind(ref), YEPTRIS_NODE_ALIAS);
    YeptrisNode starget = yeptris_node_alias_target(ref);
    ASSERT_NE(starget, nullptr);
    EXPECT_EQ(val(starget), "hello");
    yeptris_document_free(doc);
}

/* A props-only line followed by its node: the pend anchor VIEW and its
 * ordinal id must ride together (same-line-wins) into block sequences,
 * flow collections, and scalars — the id alone reaching the DOM was the
 * self-referencing-structures bug (empty error, found by the Ruby port
 * spec: "--- &id001\n- *id001"). */
TEST(Parse, PendAnchorIdRidesTheView) {
    struct {
        const char* yaml;
        const char* key;
        YeptrisNodeKind kind;
    } cases[] = {
        {"--- &id001\n- *id001\n", NULL, YEPTRIS_NODE_SEQUENCE},
        {"k: &a\n  [b, 1]\nr: *a\n", "k", YEPTRIS_NODE_SEQUENCE},
        {"k: &a\n  {b: 1}\nr: *a\n", "k", YEPTRIS_NODE_MAPPING},
        {"k: &a\n  word\nr: *a\n", "k", YEPTRIS_NODE_SCALAR},
    };
    for (const auto& c : cases) {
        YeptrisStatus st;
        YeptrisDocument doc = yeptris_parse(c.yaml, strlen(c.yaml), &st);
        ASSERT_NE(doc, nullptr) << c.yaml << " -> " << yeptris_last_error(NULL, NULL);
        YeptrisNode root = yeptris_document_root(doc, 0);
        ASSERT_NE(root, nullptr);
        YeptrisNode node = c.key ? yeptris_node_map_get(root, c.key, strlen(c.key)) : root;
        ASSERT_NE(node, nullptr) << c.yaml;
        EXPECT_EQ(yeptris_node_kind(node), c.kind) << c.yaml;
        size_t alen = 0;
        const char* an = yeptris_node_anchor(node, &alen);
        ASSERT_NE(an, nullptr) << c.yaml;
        EXPECT_EQ(std::string(an, alen), c.key ? "a" : "id001") << c.yaml;
        const char* ref_name = c.key ? "r" : NULL;
        YeptrisNode ref = ref_name ? yeptris_node_map_get(root, ref_name, 1) : nullptr;
        if (ref_name) {
            ASSERT_NE(ref, nullptr) << c.yaml;
            EXPECT_EQ(yeptris_node_kind(ref), YEPTRIS_NODE_ALIAS) << c.yaml;
            YeptrisNode target = yeptris_node_alias_target(ref);
            ASSERT_NE(target, nullptr) << c.yaml;
            EXPECT_EQ(yeptris_node_id(target), yeptris_node_id(node)) << c.yaml;
        } else {
            /* the self-referencing sequence: element 0 aliases the root */
            YeptrisNode el = yeptris_node_seq_at(node, 0);
            ASSERT_NE(el, nullptr) << c.yaml;
            EXPECT_EQ(yeptris_node_kind(el), YEPTRIS_NODE_ALIAS) << c.yaml;
            EXPECT_EQ(yeptris_node_id(yeptris_node_alias_target(el)), yeptris_node_id(node))
                << c.yaml;
        }
        yeptris_document_free(doc);
    }
}

TEST(Parse, Tags) {
    const char* y = "a: !!str 123\nb: !custom v\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    YeptrisNode a = yeptris_node_map_get(root, "a", 1);
    ASSERT_NE(a, nullptr);
    size_t tlen = 0;
    const char* t = yeptris_node_tag(a, &tlen);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(std::string(t, tlen), "tag:yaml.org,2002:str");
    EXPECT_EQ(val(a), "123");
    YeptrisNode b = yeptris_node_map_get(root, "b", 1);
    t = yeptris_node_tag(b, &tlen);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(std::string(t, tlen), "!custom");
    yeptris_document_free(doc);
}

TEST(Parse, MultipleDocuments) {
    const char* y = "---\nfirst: 1\n---\nsecond: 2\n...\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    EXPECT_EQ(yeptris_document_count(doc), 2u);
    YeptrisNode r0 = yeptris_document_root(doc, 0);
    ASSERT_NE(r0, nullptr);
    EXPECT_EQ(map_str(r0, "first"), "1");
    YeptrisNode r1 = yeptris_document_root(doc, 1);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(map_str(r1, "second"), "2");
    yeptris_document_free(doc);
}

TEST(Parse, CommentsAndBlanks) {
    const char* y = "# leading comment\n"
                    "\n"
                    "key: value # trailing\n"
                    "\n"
                    "# another\n"
                    "next: 2\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_map_count(root), 2u);
    EXPECT_EQ(map_str(root, "key"), "value");
    EXPECT_EQ(map_str(root, "next"), "2");
    yeptris_document_free(doc);
}

TEST(Parse, NullValues) {
    const char* y = "empty:\nother: ~\nnul: null\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(map_str(root, "empty"), "");
    EXPECT_EQ(map_str(root, "other"), "~");
    EXPECT_EQ(map_str(root, "nul"), "null");
    yeptris_document_free(doc);
}

/* The block-level flow indent floor (9C9N/VJP3) is a yaml-test-suite
 * rule libyaml never enforced: real-world locale files close multi-line
 * flow collections at their parent's column. The strict 1.2 engine
 * keeps the floor (the conformance suite demands the rejection); the
 * 11-COMPAT surface (the psych/libyaml parity entry every Ruby adapter
 * rides) must accept what libyaml accepts — isodoc's i18n-en.yaml is
 * the real-world breaker (metanorma-standoc CI, 2026-09-26). */
TEST(Parse, CompatGrammarFlowFloorParity) {
    struct {
        const char* y;
        int core_ok;     /* the strict engine's verdict */
        int root_is_map; /* compat: the root node's kind */
    } cases[] = {
        /* close at the parent's column — libyaml accepts, suite rejects */
        {"a: {\n  x: 1\n}\nb: 3\n", 0, 1},
        /* 9C9N: continuation line at the parent's column */
        {"flow: [a,\nb,\nc]", 0, 1},
        /* VJP3#1: every line at column 0 */
        {"k: {\nk\n:\nv\n}", 0, 1},
        /* shapes both engines accept */
        {"a: {x: 1}\n", 1, 1},
        {"a: {x: 1,\n y: 2}\n", 1, 1},
        {"[\n1,\n2\n]", 1, 0},
    };
    for (const auto& c : cases) {
        YeptrisParseOptions opts;
        memset(&opts, 0, sizeof(opts));

        opts.schema = YEPTRIS_SCHEMA_12_CORE;
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse_ex(c.y, strlen(c.y), &opts, &st);
        EXPECT_EQ(st == YEPTRIS_OK, c.core_ok != 0) << "core input: " << c.y;
        yeptris_document_free(doc);

        opts.schema = YEPTRIS_SCHEMA_11_COMPAT;
        st = YEPTRIS_OK;
        doc = yeptris_parse_ex(c.y, strlen(c.y), &opts, &st);
        ASSERT_EQ(st, YEPTRIS_OK) << "compat input: " << c.y;
        YeptrisNode root = yeptris_document_root(doc, 0);
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(yeptris_node_kind(root) == YEPTRIS_NODE_MAPPING, c.root_is_map != 0) << c.y;
        yeptris_document_free(doc);
    }

    /* the compat parse is libyaml's, not just accepted: the outdented
     * continuation is flow content ("a", "b", "c" — not block keys) */
    const char* y = "flow: [a,\nb,\nc]";
    YeptrisParseOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.schema = YEPTRIS_SCHEMA_11_COMPAT;
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse_ex(y, strlen(y), &opts, &st);
    ASSERT_EQ(st, YEPTRIS_OK);
    YeptrisNode root = yeptris_document_root(doc, 0);
    YeptrisNode seq = yeptris_node_map_get(root, "flow", 4);
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(yeptris_node_kind(seq), YEPTRIS_NODE_SEQUENCE);
    EXPECT_EQ(yeptris_node_seq_count(seq), 3u);
    EXPECT_EQ(val(yeptris_node_seq_at(seq, 2)), "c");
    yeptris_document_free(doc);
}

TEST(Parse, Errors) {
    struct {
        const char* y;
        YeptrisStatus want;
    } cases[] = {
        {"key: [1, 2", YEPTRIS_ERROR_PARSE},          /* unterminated flow */
        {"key: \"unterminated", YEPTRIS_ERROR_PARSE}, /* unterminated quote */
        {"bad: *missing", YEPTRIS_ERROR_PARSE},       /* undefined alias */
        {"a: 1\n\tb: 2\n", YEPTRIS_ERROR_PARSE},      /* tab indent */
        {"[1,,2]", YEPTRIS_ERROR_PARSE},              /* doubled comma */
        {"a: b: c", YEPTRIS_ERROR_PARSE},             /* mapping values not allowed */
    };
    for (const auto& c : cases) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(c.y, strlen(c.y), &st);
        EXPECT_EQ(doc, nullptr) << "input: " << c.y;
        EXPECT_EQ(st, c.want) << "input: " << c.y;
        if (doc == nullptr) {
            uint32_t line = 0, col = 0;
            const char* msg = yeptris_last_error(&line, &col);
            EXPECT_STRNE(msg, "") << "input: " << c.y;
            EXPECT_GE(line, 1u) << "error position expected: " << c.y;
        }
        yeptris_document_free(doc);
    }
}

TEST(Parse, EmptyAndWhitespace) {
    for (const char* y : {"", "\n", "   \n", "# only a comment\n"}) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
        EXPECT_EQ(doc, nullptr) << "input: [" << y << "]";
        EXPECT_EQ(st, YEPTRIS_OK) << "empty input is not an error: [" << y << "]";
    }
}

/* A properties-only document ("---\n!\n") ends at the boundary: the
 * props belong to its empty scalar and must never swallow the next
 * document as their value (found by the realworld bench corpus). */
TEST(Parse, PropsOnlyDocumentThenBoundary) {
    struct {
        const char* y;
        const char* tag;
        const char* anchor;
    } cases[] = {
        {"---\n!\n---\nfoo: bar\n", "!", ""},
        {"---\n!tag\n---\nfoo: bar\n", "!tag", ""},
        {"---\n&a\n---\nfoo: bar\n", "", "a"},
        {"---\n!\n...\nfoo: bar\n", "!", ""},
        {"---\n!\n# comment\n---\nfoo: bar\n", "!", ""},
    };
    for (const auto& c : cases) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse(c.y, strlen(c.y), &st);
        ASSERT_NE(doc, nullptr) << c.y << ": " << yeptris_last_error(NULL, NULL);
        ASSERT_EQ(yeptris_document_count(doc), 2u) << c.y;
        YeptrisNode first = yeptris_document_root(doc, 0);
        ASSERT_NE(first, nullptr) << c.y;
        EXPECT_EQ(yeptris_node_kind(first), YEPTRIS_NODE_SCALAR) << c.y;
        EXPECT_EQ(val(first), "") << c.y;
        size_t tl = 0;
        const char* tp = yeptris_node_tag(first, &tl);
        std::string tag = tp ? std::string(tp, tl) : std::string();
        EXPECT_EQ(tag, c.tag) << c.y;
        YeptrisNode second = yeptris_document_root(doc, 1);
        ASSERT_NE(second, nullptr) << c.y;
        EXPECT_EQ(yeptris_node_kind(second), YEPTRIS_NODE_MAPPING) << c.y;
        EXPECT_EQ(map_str(second, "foo"), "bar") << c.y;
        yeptris_document_free(doc);
    }
}

/* 2JQS: two empty-key pairs — suite-valid, libyaml-rejected; the bench
 * corpus keeps it out of the shared race but parse behavior is pinned. */
TEST(Parse, EmptyKeyPairs) {
    const char* y = ": a\n: b\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr) << yeptris_last_error(NULL, NULL);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_kind(root), YEPTRIS_NODE_MAPPING);
    EXPECT_EQ(yeptris_node_map_count(root), 2u);
    EXPECT_EQ(map_str(root, ""), "a");
    yeptris_document_free(doc);
}

TEST(Parse, DeepNestingGuard) {
    std::string y;
    for (int i = 0; i < 1200; i++) {
        y += "a:\n";
        for (int j = 0; j <= i; j++) {
            y += " ";
        }
    }
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y.c_str(), y.size(), &st);
    EXPECT_EQ(doc, nullptr);
    EXPECT_EQ(st, YEPTRIS_ERROR_DEPTH);
}

/* ---- strict JSON mode (TODO.impl/08C) -------------------------------- */

TEST(JsonMode, StrictAcceptsAndRejects) {
    struct {
        const char* j;
        int accept;
    } cases[] = {
        {"{\"a\": 1}", 1}, {"[1, 2, 3]", 1}, {"42", 1},      {"\"x\"", 1},       {"true", 1},
        {"null", 1},       {"-0.5e+10", 1},  {" [ 1 ] ", 1}, {"{\"a\": 1,}", 0}, {"[1,,2]", 0},
        {"01", 0},         {"+1", 0},        {".5", 0},      {"{'a': 1}", 0},    {"[unquoted]", 0},
        {"{\"a\" 1}", 0},  {"[1 2]", 0},     {"", 0},        {"[1] x", 0},       {"[", 0},
        {"\"\\uDd\"", 0},  {"\"\\a\"", 0},   {"nan", 0},     {"[1.0e]", 0},
    };
    for (const auto& c : cases) {
        YeptrisStatus st = YEPTRIS_OK;
        YeptrisDocument doc = yeptris_parse_json(c.j, strlen(c.j), &st);
        EXPECT_EQ(doc != nullptr, c.accept != 0) << c.j;
        yeptris_document_free(doc);
    }
}

TEST(JsonMode, DomQueries) {
    const char* j = "{\"name\": \"yeptris\", \"n\": 42, \"pi\": 3.5, \"ok\": true, \"no\": null}";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse_json(j, strlen(j), &st);
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_kind(root), YEPTRIS_NODE_MAPPING);
    EXPECT_EQ(map_str(root, "name"), "yeptris");
    int64_t n = 0;
    EXPECT_EQ(yeptris_node_int(yeptris_node_map_get(root, "n", 1), &n), YEPTRIS_OK);
    EXPECT_EQ(n, 42);
    double pi = 0;
    EXPECT_EQ(yeptris_node_float(yeptris_node_map_get(root, "pi", 2), &pi), YEPTRIS_OK);
    EXPECT_DOUBLE_EQ(pi, 3.5);
    int b = 0;
    EXPECT_EQ(yeptris_node_bool(yeptris_node_map_get(root, "ok", 2), &b), YEPTRIS_OK);
    EXPECT_EQ(b, 1);
    /* JSON null: the literal text with the null tag (Psych: nil) */
    YeptrisNode no = yeptris_node_map_get(root, "no", 2);
    EXPECT_EQ(val(no), "null");
    yeptris_document_free(doc);
}

TEST(JsonMode, EscapesAndDeep) {
    const char* j = "{\"k\": \"a\\nb\\u00e9\\ud834\\udd1e\", \"arr\": [[[[1]]]]}";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse_json(j, strlen(j), &st);
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_document_root(doc, 0);
    EXPECT_EQ(map_str(root, "k"), "a\nb\xC3\xA9\xF0\x9D\x84\x9E");
    yeptris_document_free(doc);
    std::string deep = "[[[[[[[[[[10]]]]]]]]]]";
    YeptrisStatus st2 = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse_json(deep.c_str(), deep.size(), &st2);
    EXPECT_NE(doc2, nullptr);
    yeptris_document_free(doc2);
}

/* ruby #168: the bulk children drain — one walk instead of the
 * per-index O(i) seq_at/map_at loops (the 80k-row relaton index paid
 * n^2/2 sibling steps). */
TEST(NodeChildren, BulkDrainMatchesIndexedAccess) {
    const char* y = "top:\n"
                    "  - one\n"
                    "  - [a, b]\n"
                    "  - k: v\n"
                    "    k2: v2\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);

    /* count-only: no handles allocated, same count as seq_count */
    EXPECT_EQ(yeptris_node_children(root, NULL, 0), 2u);

    /* mapping: key,value interleaved — one walk equals map_at pairs */
    YeptrisNode kids[2];
    EXPECT_EQ(yeptris_node_children(root, kids, 2), 2u);
    EXPECT_EQ(val(kids[0]), "top");
    YeptrisNode seq = kids[1];
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(yeptris_node_kind(seq), YEPTRIS_NODE_SEQUENCE);

    EXPECT_EQ(yeptris_node_children(seq, NULL, 0), 3u);
    YeptrisNode items[3];
    EXPECT_EQ(yeptris_node_children(seq, items, 3), 3u);
    EXPECT_EQ(val(items[0]), "one");
    EXPECT_EQ(yeptris_node_kind(items[1]), YEPTRIS_NODE_SEQUENCE);
    EXPECT_EQ(yeptris_node_kind(items[2]), YEPTRIS_NODE_MAPPING);

    /* differential: the drain's values equal seq_at's, in order */
    for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(yeptris_node_id(items[i]), yeptris_node_id(yeptris_node_seq_at(seq, i)));
    }

    /* the nested mapping: pairs interleaved — the map_at equivalence */
    YeptrisNode m = items[2];
    YeptrisNode pairs[4];
    EXPECT_EQ(yeptris_node_children(m, pairs, 4), 4u);
    for (size_t i = 0; i < 2; i++) {
        YeptrisNode k, v;
        ASSERT_EQ(yeptris_node_map_at(m, i, &k, &v), 0);
        EXPECT_EQ(yeptris_node_id(pairs[2 * i]), yeptris_node_id(k));
        EXPECT_EQ(yeptris_node_id(pairs[2 * i + 1]), yeptris_node_id(v));
    }
    yeptris_document_free(doc);
}

TEST(NodeChildren, ScalarsAndShortBuffers) {
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse_json("[10, 20, 30]", strlen("[10, 20, 30]"), &st);
    ASSERT_EQ(st, YEPTRIS_OK);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);

    /* a scalar has no children */
    YeptrisNode ten = yeptris_node_seq_at(root, 0);
    ASSERT_NE(ten, nullptr);
    EXPECT_EQ(yeptris_node_children(ten, NULL, 0), 0u);

    /* a short buffer fills what fits and still returns the total */
    YeptrisNode one[1];
    EXPECT_EQ(yeptris_node_children(root, one, 1), 3u);
    EXPECT_EQ(val(one[0]), "10");

    /* NULL handle */
    EXPECT_EQ(yeptris_node_children(NULL, NULL, 0), 0u);
    yeptris_document_free(doc);
}

TEST(NodeChildren, LargeSequenceDrainsOnce) {
    /* the repro shape from ruby #168: 80k top-level rows */
    std::string y = "- k: 1\n";
    y.reserve(y.size() * 80000);
    for (int i = 1; i < 80000; i++) {
        y += "- k: 1\n";
    }
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y.c_str(), y.size(), &st);
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(yeptris_node_children(root, NULL, 0), 80000u);
    std::vector<YeptrisNode> kids(80000);
    EXPECT_EQ(yeptris_node_children(root, kids.data(), kids.size()), 80000u);
    EXPECT_EQ(yeptris_node_kind(kids[79999]), YEPTRIS_NODE_MAPPING);
    yeptris_document_free(doc);
}

/* #377: the lazy child-index — indexed access must reflect mutations
 * (every link/unlink invalidates the cache) and stay differential
 * with the drain. */
TEST(NodeChildren, IndexedAccessSurvivesMutation) {
    const char* y = "- one\n- two\n- three\n";
    YeptrisStatus st;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    YeptrisNode root = yeptris_document_root(doc, 0);
    ASSERT_NE(root, nullptr);

    /* warm the cache */
    EXPECT_EQ(val(yeptris_node_seq_at(root, 2)), "three");

    /* mutate: append + delete, then re-index */
    YeptrisNode four = yeptris_node_new_scalar(doc, "four", 4, YEPTRIS_STYLE_PLAIN);
    ASSERT_NE(four, nullptr);
    ASSERT_EQ(yeptris_node_seq_add(root, four), YEPTRIS_OK);
    EXPECT_EQ(yeptris_node_seq_count(root), 4u);
    EXPECT_EQ(val(yeptris_node_seq_at(root, 3)), "four");
    EXPECT_EQ(val(yeptris_node_seq_at(root, 2)), "three");

    ASSERT_EQ(yeptris_node_seq_del(root, 0), YEPTRIS_OK);
    EXPECT_EQ(val(yeptris_node_seq_at(root, 0)), "two");
    EXPECT_EQ(val(yeptris_node_seq_at(root, 2)), "four");

    /* differential: the drain agrees with the index after mutation */
    YeptrisNode kids[3];
    ASSERT_EQ(yeptris_node_children(root, kids, 3), 3u);
    for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(yeptris_node_id(kids[i]), yeptris_node_id(yeptris_node_seq_at(root, i)));
    }
    yeptris_document_free(doc);
}

TEST(Parse, NestedExplicitPairNullsAtOwnLevels) {
    /* catch 88 (the nightly's run 38387614146): nested '? ?' pairs
     * stacked two pending nulls over a single depth slot — the inner
     * clobbered the outer's level, the outer null flushed inside the
     * wrong map, and the DOM alternation desynced. The pending levels
     * are a stack now: each '?' pair's null flushes directly inside
     * its own map (the libyaml oracle: {{{"k" => "a"} => nil} =>
     * nil}). */
    const char* y = "? ? k : a\n";
    YeptrisRecorder rec = yeptris_recorder_new();
    ASSERT_NE(rec, nullptr);
    ASSERT_EQ(yeptris_recorder_feed(rec, y, strlen(y), 1), YEPTRIS_OK);
    size_t n = 0;
    const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
    ASSERT_NE(rs, nullptr);
    int map_starts = 0, map_ends = 0, nulls = 0;
    for (size_t i = 0; i < n; i++) {
        if (rs[i].type == YEPTRIS_EV_MAPPING_START) {
            map_starts++;
        }
        if (rs[i].type == YEPTRIS_EV_MAPPING_END) {
            map_ends++;
        }
        if (rs[i].type == YEPTRIS_EV_SCALAR && rs[i].value_len == 0) {
            nulls++;
        }
    }
    EXPECT_EQ(map_starts, 3);
    EXPECT_EQ(map_ends, 3);
    EXPECT_EQ(nulls, 2); /* one per '?' pair, none missing, none doubled */
    yeptris_recorder_free(rec);
}

TEST(Emit, NestedExplicitPairRoundTripsWithAnchors) {
    /* catch 88's full minimum: the nested '?' nulls plus the anchored
     * empty value of the following pair — the dropped anchor left the
     * trailing alias dangling and the emitted document failed to
     * re-parse. */
    const char* y = ":\n  ? ? &? &a : a\n  ?\"&d!se: &base\nU name <: *base\n";
    YeptrisStatus st = YEPTRIS_OK;
    YeptrisDocument doc = yeptris_parse(y, strlen(y), &st);
    ASSERT_NE(doc, nullptr);
    size_t l1 = 0;
    char* s1 = yeptris_serialize(doc, &l1);
    ASSERT_NE(s1, nullptr);
    yeptris_document_free(doc);
    /* the anchor must survive the emit for the alias to resolve */
    EXPECT_NE(strstr(s1, "&base"), nullptr) << s1;
    st = YEPTRIS_OK;
    YeptrisDocument doc2 = yeptris_parse(s1, l1, &st);
    EXPECT_NE(doc2, nullptr) << s1;
    if (doc2 != NULL) {
        size_t l2 = 0;
        char* s2 = yeptris_serialize(doc2, &l2);
        EXPECT_TRUE(s2 != NULL && l1 == l2 && memcmp(s1, s2, l1) == 0) << s1;
        free(s2);
        yeptris_document_free(doc2);
    }
    free(s1);
}

TEST(Parse, FlowNumberKeyRidesTheGeneralKernel) {
    /* catch 89 (the nightly's run 38513849602): the JSON fast path's
     * lenient walk accepted "{9:1}" — invalid JSON, a number key — and
     * emitted {9 -> 1}, while a chunked feed fell under the <24-byte
     * gate to the general kernel, which reads "9:1" as one plain
     * scalar. Whole and chunked streams diverged on the same bytes.
     * The fast paths are byte-for-byte JSON now; number/literal keys
     * take the general kernel's colon rules in both modes. */
    const char* y = "{9:1}\n---\n  ? ? &?  &a :\n";
    size_t len = strlen(y);
    /* the recorder resets at every feed entry — accumulate per feed,
     * the fuzz_feed harness's discipline */
    typedef struct {
        uint16_t type;
        uint32_t line, col;
        uint32_t vlen;
    } pin_rec;
    pin_rec acc[2][128];
    size_t an[2] = {0, 0};
    int key_len = -1;
    for (int mode = 0; mode < 2; mode++) {
        YeptrisRecorder rec = yeptris_recorder_new();
        ASSERT_NE(rec, nullptr);
        if (mode == 0) {
            ASSERT_EQ(yeptris_recorder_feed(rec, y, len, 0), YEPTRIS_OK);
            size_t n = 0;
            const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
            for (size_t i = 0; i < n && an[0] < 128; i++) {
                acc[0][an[0]].type = rs[i].type;
                acc[0][an[0]].line = rs[i].line;
                acc[0][an[0]].col = rs[i].col;
                acc[0][an[0]].vlen = rs[i].value_len;
                if (rs[i].type == YEPTRIS_EV_SCALAR && rs[i].line == 1 && rs[i].col == 2) {
                    key_len = (int)rs[i].value_len;
                }
                an[0]++;
            }
        } else {
            for (size_t off = 0; off < len; off++) {
                ASSERT_EQ(yeptris_recorder_feed(rec, y + off, 1, 0), YEPTRIS_OK);
                size_t n = 0;
                const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
                for (size_t i = 0; i < n && an[1] < 128; i++) {
                    acc[1][an[1]].type = rs[i].type;
                    acc[1][an[1]].line = rs[i].line;
                    acc[1][an[1]].col = rs[i].col;
                    acc[1][an[1]].vlen = rs[i].value_len;
                    an[1]++;
                }
            }
        }
        ASSERT_EQ(yeptris_recorder_feed(rec, "", 0, 1), YEPTRIS_OK);
        size_t n = 0;
        const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
        for (size_t i = 0; i < n && an[mode] < 128; i++) {
            acc[mode][an[mode]].type = rs[i].type;
            acc[mode][an[mode]].line = rs[i].line;
            acc[mode][an[mode]].col = rs[i].col;
            acc[mode][an[mode]].vlen = rs[i].value_len;
            an[mode]++;
        }
        yeptris_recorder_free(rec);
    }
    /* the flow key is the plain scalar "9:1" (3 bytes), not 9 */
    EXPECT_EQ(key_len, 3);
    /* whole vs chunked: identical streams */
    ASSERT_EQ(an[0], an[1]);
    for (size_t i = 0; i < an[0]; i++) {
        ASSERT_EQ(acc[0][i].type, acc[1][i].type);
        ASSERT_EQ(acc[0][i].line, acc[1][i].line);
        ASSERT_EQ(acc[0][i].col, acc[1][i].col);
        ASSERT_EQ(acc[0][i].vlen, acc[1][i].vlen);
    }
}

TEST(Parse, FlowKeyLookaheadRestoresTheLineState) {
    /* catch 90 (the nightly's run 38518507064): the flow key
     * detection's ws lookahead crossed the newline after a value and
     * the no-colon rollback restored the cursor but not the line
     * state — e->line stayed one high, the multi-line flow's END
     * (and every mark after it) parsed one line low. Whole feeds took
     * the JSON fast path (its own line walk) and never noticed. */
    const char* y = "{\n\"b\":\"\"\n}\n--- #\n...\n%TA\n";
    typedef struct {
        uint16_t type;
        uint32_t line, col;
    } pin_rec;
    pin_rec acc[2][64];
    size_t an[2] = {0, 0};
    for (int mode = 0; mode < 2; mode++) {
        YeptrisRecorder rec = yeptris_recorder_new();
        ASSERT_NE(rec, nullptr);
        /* the recorder resets at every feed entry — accumulate per
         * feed (the fuzz_feed harness's discipline) */
        size_t n = 0;
        const YeptrisEventRecord* rs = NULL;
        if (mode == 0) {
            ASSERT_EQ(yeptris_recorder_feed(rec, y, strlen(y), 0), YEPTRIS_OK);
            rs = yeptris_recorder_records(rec, &n);
            for (size_t i = 0; i < n && an[0] < 64; i++) {
                acc[0][an[0]].type = rs[i].type;
                acc[0][an[0]].line = rs[i].line;
                acc[0][an[0]].col = rs[i].col;
                an[0]++;
            }
        } else {
            for (size_t off = 0; off < strlen(y); off++) {
                ASSERT_EQ(yeptris_recorder_feed(rec, y + off, 1, 0), YEPTRIS_OK);
                rs = yeptris_recorder_records(rec, &n);
                for (size_t i = 0; i < n && an[1] < 64; i++) {
                    acc[1][an[1]].type = rs[i].type;
                    acc[1][an[1]].line = rs[i].line;
                    acc[1][an[1]].col = rs[i].col;
                    an[1]++;
                }
            }
        }
        ASSERT_EQ(yeptris_recorder_feed(rec, "", 0, 1), YEPTRIS_OK);
        rs = yeptris_recorder_records(rec, &n);
        for (size_t i = 0; i < n && an[mode] < 64; i++) {
            acc[mode][an[mode]].type = rs[i].type;
            acc[mode][an[mode]].line = rs[i].line;
            acc[mode][an[mode]].col = rs[i].col;
            an[mode]++;
        }
        yeptris_recorder_free(rec);
    }
    /* the multi-line flow's END marks at the '}' (libyaml: 3:1) */
    int map_end_line = -1;
    for (size_t i = 0; i < an[0]; i++) {
        if (acc[0][i].type == YEPTRIS_EV_MAPPING_END && map_end_line == -1) {
            map_end_line = (int)acc[0][i].line;
        }
    }
    EXPECT_EQ(map_end_line, 3);
    /* whole vs chunked: identical marks everywhere */
    ASSERT_EQ(an[0], an[1]);
    for (size_t i = 0; i < an[0]; i++) {
        ASSERT_EQ(acc[0][i].type, acc[1][i].type);
        ASSERT_EQ(acc[0][i].line, acc[1][i].line);
        ASSERT_EQ(acc[0][i].col, acc[1][i].col);
    }
}

TEST(Parse, BreakNormalizationKeepsTheInputLength) {
    /* catch 91 (the nightly's run 38524928004): the NEL/LS/PS break
     * normalizer copies the input and null-terminates the copy, and
     * the run measured that copy with strlen() — an input carrying a
     * NUL byte of its own was truncated at it. "\0\n---\r<U+2028>"
     * whole-parsed as an empty stream (the LS triggered the copy, the
     * NUL cut it), while a chunked feed cut before the LS and never
     * copied: 2 records vs 8, on the same bytes. The copied length is
     * the normalizer's own count now. */
    const char y[] = "\0\n---\r\xE2\x80\xA8";
    typedef struct {
        uint16_t type;
        uint32_t line, col;
    } pin_rec;
    pin_rec acc[2][64];
    size_t an[2] = {0, 0};
    const size_t len = sizeof(y) - 1;
    for (int mode = 0; mode < 2; mode++) {
        YeptrisRecorder rec = yeptris_recorder_new();
        ASSERT_NE(rec, nullptr);
        size_t n = 0;
        const YeptrisEventRecord* rs = NULL;
        if (mode == 0) {
            ASSERT_EQ(yeptris_recorder_feed(rec, y, len, 1), YEPTRIS_OK);
            rs = yeptris_recorder_records(rec, &n);
            for (size_t i = 0; i < n && an[0] < 64; i++) {
                acc[0][an[0]].type = rs[i].type;
                acc[0][an[0]].line = rs[i].line;
                acc[0][an[0]].col = rs[i].col;
                an[0]++;
            }
        } else {
            for (size_t off = 0; off < len; off++) {
                ASSERT_EQ(yeptris_recorder_feed(rec, y + off, 1, 0), YEPTRIS_OK);
                rs = yeptris_recorder_records(rec, &n);
                for (size_t i = 0; i < n && an[1] < 64; i++) {
                    acc[1][an[1]].type = rs[i].type;
                    acc[1][an[1]].line = rs[i].line;
                    acc[1][an[1]].col = rs[i].col;
                    an[1]++;
                }
            }
            ASSERT_EQ(yeptris_recorder_feed(rec, "", 0, 1), YEPTRIS_OK);
            rs = yeptris_recorder_records(rec, &n);
            for (size_t i = 0; i < n && an[1] < 64; i++) {
                acc[1][an[1]].type = rs[i].type;
                acc[1][an[1]].line = rs[i].line;
                acc[1][an[1]].col = rs[i].col;
                an[1]++;
            }
        }
        yeptris_recorder_free(rec);
    }
    /* whole vs chunked: identical streams — and not the truncated one */
    EXPECT_GT(an[0], 2u);
    ASSERT_EQ(an[0], an[1]);
    for (size_t i = 0; i < an[0]; i++) {
        ASSERT_EQ(acc[0][i].type, acc[1][i].type);
        ASSERT_EQ(acc[0][i].line, acc[1][i].line);
        ASSERT_EQ(acc[0][i].col, acc[1][i].col);
    }
}

TEST(Parse, StepScanTracksFlowValueQuotes) {
    /* catch 93 (the nightly's run 38534965021): step_scan's space
     * handler set tok_start = word_led, so after a plain-led key the
     * quote at the flow VALUE position was not tracked — the cut
     * landed inside the quoted span ("{f '}\n--- '}": whole saw the
     * far close and ate the '---' marker, chunked errored on the
     * unterminated prefix). In a flow a space always ends the token. */
    const char* y = "{f '}\n--- '}\n";
    /* whole and chunked must agree (both reject: the quote is
     * unterminated in the whole buffer too once tracked) */
    typedef struct {
        uint16_t type;
        uint32_t line, col;
    } pin_rec;
    pin_rec a[32] = {}, b[32] = {};
    size_t an = 0, bn = 0;
    {
        YeptrisRecorder rec = yeptris_recorder_new();
        ASSERT_NE(rec, nullptr);
        yeptris_recorder_feed(rec, y, strlen(y), 1);
        size_t n = 0;
        const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
        for (size_t i = 0; i < n && an < 32; i++) {
            a[an].type = rs[i].type;
            a[an].line = rs[i].line;
            a[an].col = rs[i].col;
            an++;
        }
        yeptris_recorder_free(rec);
    }
    {
        YeptrisRecorder rec = yeptris_recorder_new();
        ASSERT_NE(rec, nullptr);
        for (size_t off = 0; off < strlen(y); off++) {
            yeptris_recorder_feed(rec, y + off, 1, 0);
            size_t n = 0;
            const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
            for (size_t i = 0; i < n && bn < 32; i++) {
                b[bn].type = rs[i].type;
                b[bn].line = rs[i].line;
                b[bn].col = rs[i].col;
                bn++;
            }
        }
        yeptris_recorder_feed(rec, "", 0, 1);
        size_t n = 0;
        const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
        for (size_t i = 0; i < n && bn < 32; i++) {
            b[bn].type = rs[i].type;
            b[bn].line = rs[i].line;
            b[bn].col = rs[i].col;
            bn++;
        }
        yeptris_recorder_free(rec);
    }
    ASSERT_GT(an, 0u);
    ASSERT_EQ(an, bn);
    for (size_t i = 0; i < an; i++) {
        ASSERT_EQ(a[i].type, b[i].type);
        ASSERT_EQ(a[i].line, b[i].line);
        ASSERT_EQ(a[i].col, b[i].col);
    }
}

TEST(Parse, FlowPreScanIncompleteDefersOnPartialFeeds) {
    /* catch 92 (the nightly's run 38530395946): e_skip_flow's
     * "unclosed at buffer end" return fired identically for partial
     * step feeds and complete buffers. A cut before the flow's '}'
     * ('{ !< }\n--- }') made the '{' arm bail before MAP_START in the
     * chunked path while whole (with the '}') emitted it — different
     * record counts for the same bytes. The pre-scan now defers when
     * the engine is mid-stream (parsing_final = 0); complete-buffer
     * errors are unchanged. */
    const char* y = "{ !< }\n--- }\n";
    typedef struct {
        uint16_t type;
        uint32_t line, col;
    } pin_rec;
    pin_rec a[32] = {}, b[32] = {};
    size_t an = 0, bn = 0;
    {
        YeptrisRecorder rec = yeptris_recorder_new();
        ASSERT_NE(rec, nullptr);
        yeptris_recorder_feed(rec, y, strlen(y), 1);
        size_t n = 0;
        const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
        for (size_t i = 0; i < n && an < 32; i++) {
            a[an].type = rs[i].type;
            a[an].line = rs[i].line;
            a[an].col = rs[i].col;
            an++;
        }
        yeptris_recorder_free(rec);
    }
    {
        YeptrisRecorder rec = yeptris_recorder_new();
        ASSERT_NE(rec, nullptr);
        for (size_t off = 0; off < strlen(y); off++) {
            yeptris_recorder_feed(rec, y + off, 1, 0);
            size_t n = 0;
            const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
            for (size_t i = 0; i < n && bn < 32; i++) {
                b[bn].type = rs[i].type;
                b[bn].line = rs[i].line;
                b[bn].col = rs[i].col;
                bn++;
            }
        }
        yeptris_recorder_feed(rec, "", 0, 1);
        size_t n = 0;
        const YeptrisEventRecord* rs = yeptris_recorder_records(rec, &n);
        for (size_t i = 0; i < n && bn < 32; i++) {
            b[bn].type = rs[i].type;
            b[bn].line = rs[i].line;
            b[bn].col = rs[i].col;
            bn++;
        }
        yeptris_recorder_free(rec);
    }
    ASSERT_GT(an, 0u);
    ASSERT_EQ(an, bn);
    for (size_t i = 0; i < an; i++) {
        ASSERT_EQ(a[i].type, b[i].type);
        ASSERT_EQ(a[i].line, b[i].line);
        ASSERT_EQ(a[i].col, b[i].col);
    }
}
