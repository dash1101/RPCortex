#!/usr/bin/env python3
"""Ed25519 test vectors for ed25519_test.cpp, taken from the text of RFC 8032.

    python3 gen_ed25519_vectors.py [rfc8032.txt] > ed25519_vectors.inc

The input is the RFC as published (https://www.rfc-editor.org/rfc/rfc8032.txt).

  - Section 7.1: the five Ed25519 test vectors, parsed out of the text. Every
    length is checked against the "(length N bytes)" the RFC states, and every
    vector against the RFC's own sign and verify functions, so a parsing slip
    stops the script instead of becoming a wrong vector.
  - Section 6: the RFC's Python illustration, run as published, computes the
    vectors for the rules the five do not reach: S out of range, encodings that
    must not decode, and a signature that only a cofactored check accepts.

The expected verdict of each extra case comes from the RFC's TEXT (sections
5.1.3 and 5.1.7), not from the illustration's verify: its recover_x accepts an
x of zero with the sign bit set when y = p - 1, which section 5.1.3 forbids.
"""
import re
import sys

PAGE_BREAK = re.compile(r"^(Josefsson & Liusvaara .*|RFC 8032 .*|\f)$")


def lines_between(text, start, end):
    lines = text.splitlines()
    a = next(i for i, l in enumerate(lines) if l.startswith(start))
    b = next(i for i, l in enumerate(lines) if i > a and l.startswith(end))
    return [l for l in lines[a + 1:b] if not PAGE_BREAK.match(l)]


def rfc_vectors(text):
    body = lines_between(text, "7.1.  Test Vectors for Ed25519",
                         "7.2.  Test Vectors for Ed25519ctx")
    hexline = re.compile(r"^   ((?:[0-9a-f]{2})+)$")
    fields = {"SECRET KEY:": "sk", "PUBLIC KEY:": "pk", "SIGNATURE:": "sig"}
    tests, cur, field = [], None, None
    for l in body:
        s = l.strip()
        m = re.match(r"^   -----TEST (.+)$", l)
        if m:
            cur = {"name": m.group(1), "sk": "", "pk": "", "msg": "", "sig": ""}
            tests.append(cur)
            field = None
        elif cur is None or s in ("", "Ed25519", "-----"):
            continue
        elif s == "ALGORITHM:":
            field = None
        elif s in fields:
            field = fields[s]
        elif s.startswith("MESSAGE (length"):
            field = "msg"
            cur["msglen"] = int(re.match(r"MESSAGE \(length (\d+) bytes?\):$", s).group(1))
        else:
            m = hexline.match(l)
            if not m or field is None:
                raise SystemExit("unexpected line in section 7.1: %r" % l)
            cur[field] += m.group(1)
    names = [t["name"] for t in tests]
    if names != ["1", "2", "3", "1024", "SHA(abc)"]:
        raise SystemExit("expected the five tests of section 7.1, found %r" % names)
    for t in tests:
        if len(t["sk"]) != 64 or len(t["pk"]) != 64 or len(t["sig"]) != 128:
            raise SystemExit("TEST %s: a key or signature has the wrong length" % t["name"])
        if len(t["msg"]) != 2 * t["msglen"]:
            raise SystemExit("TEST %s: the message is %d bytes, the RFC says %d"
                             % (t["name"], len(t["msg"]) // 2, t["msglen"]))
    return tests


def rfc_code(text):
    body = lines_between(text, "6.  Ed25519 Python Illustration", "7.  Test Vectors")
    start = next(i for i, l in enumerate(body) if l.startswith("## First, some preliminaries"))
    ns = {}
    exec(compile("\n".join(body[start:]) + "\n", "rfc8032-section-6", "exec"), ns)
    return ns


def le(n):
    return n.to_bytes(32, "little")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "rfc8032.txt"
    with open(path, encoding="ascii") as f:
        text = f.read()
    tests = rfc_vectors(text)
    r = rfc_code(text)
    p, q, G = r["p"], r["q"], r["G"]
    compress, mul, add, equal = r["point_compress"], r["point_mul"], r["point_add"], r["point_equal"]

    for t in tests:
        sk, pk, msg, sig = (bytes.fromhex(t[k]) for k in ("sk", "pk", "msg", "sig"))
        if r["secret_to_public"](sk) != pk:
            raise SystemExit("TEST %s: the public key does not follow from the secret" % t["name"])
        if r["sign"](sk, msg) != sig or not r["verify"](pk, msg, sig):
            raise SystemExit("TEST %s: the RFC's own code disagrees with the vector" % t["name"])

    base = compress(G)
    identity = compress((0, 1, 1, 0))
    signed_identity = identity[:31] + bytes([0x80])
    # A y that is not on the curve: the smallest one recover_x refuses.
    off_y = next(y for y in range(2, 100) if r["recover_x"](y, 0) is None)
    off = le(off_y)

    # Encodings, and whether section 5.1.3 lets them decode.
    decode = [
        ("the base point", base, True),
        ("the identity", identity, True),
        ("the identity with the sign bit set (x = 0, x_0 = 1)", signed_identity, False),
        ("(0, -1), a point of order 2", le(p - 1), True),
        ("(0, -1) with the sign bit set (x = 0, x_0 = 1)", le(p - 1 + (1 << 255)), False),
        ("y = p", le(p), False),
        ("y = p + 1, the identity written the long way", le(p + 1), False),
        ("y = 2^255 - 1", le((1 << 255) - 1), False),
        ("y = %d, not on the curve" % off_y, off, False),
    ]

    # Signature cases; the verdict is the RFC's.
    t1 = tests[0]
    pk1, sig1 = bytes.fromhex(t1["pk"]), bytes.fromhex(t1["sig"])
    s1 = int.from_bytes(sig1[32:], "little")
    cases = [
        # S + L names the same scalar as S, so [S]B is unchanged: only the
        # range check refuses it.
        ("TEST 1 with S + L", pk1, b"", sig1[:32] + le(s1 + q), False),
        ("TEST 1 with S = L", pk1, b"", sig1[:32] + le(q), False),
        ("TEST 1 with S = 2^256 - 1", pk1, b"", sig1[:32] + b"\xff" * 32, False),
        # A key that is the identity verifies (R = [S]B, S) for every message.
        # Written canonically, RFC 8032 decodes it and the signature holds;
        # written any other way, the key does not decode.
        ("identity key, R = B, S = 1", identity, b"any message", base + le(1), True),
        ("identity key as y = p + 1", le(p + 1), b"any message", base + le(1), False),
        ("identity key with the sign bit set", signed_identity, b"any message", base + le(1), False),
        ("a key that is not on the curve", off, b"any message", base + le(1), False),
        # With S = 0 and the identity as the key, R = identity is the answer.
        ("R = identity, S = 0", identity, b"m", identity + le(0), True),
        ("R = identity as y = p + 1", identity, b"m", le(p + 1) + le(0), False),
        ("R = identity with the sign bit set", identity, b"m", signed_identity + le(0), False),
        ("R not on the curve", identity, b"m", off + le(0), False),
    ]

    # Cofactorless, not cofactored: R' = R + T for T = (0, -1), of order 2,
    # signed honestly with TEST 1's key. [8][S]B = [8]R' + [8][k]A holds;
    # [S]B = R' + [k]A does not.
    a, prefix = r["secret_expand"](bytes.fromhex(t1["sk"]))
    msg = b"cofactor"
    rr = r["sha512_modq"](prefix + msg)
    R2s = compress(add(mul(rr, G), (0, p - 1, 1, 0)))
    k = r["sha512_modq"](R2s + pk1 + msg)
    sig = R2s + le((rr + k * a) % q)
    A = r["point_decompress"](pk1)
    R2 = r["point_decompress"](R2s)
    if r["verify"](pk1, msg, sig):
        raise SystemExit("the cofactor case verifies without the cofactor")
    s = int.from_bytes(sig[32:], "little")
    if not equal(mul(8 * s, G), add(mul(8, R2), mul(8 * k, A))):
        raise SystemExit("the cofactor case does not verify with the cofactor either")
    cases.append(("R + (0, -1): only a cofactored check accepts it", pk1, msg, sig, False))

    out = []
    w = out.append
    w("// Generated by gen_ed25519_vectors.py from the text of RFC 8032; do not edit.")
    w("// kRfc8032 is section 7.1 as published. The rest was computed with the")
    w("// RFC's section 6 code, each verdict as sections 5.1.3 and 5.1.7 give it.")
    w("struct Ed25519Vector { const char *name, *pk, *msg, *sig; };")
    w("static const Ed25519Vector kRfc8032[] = {")
    for t in tests:
        w('    { "TEST %s", "%s",' % (t["name"], t["pk"]))
        m = t["msg"]
        if len(m) <= 64:
            w('      "%s",' % m)
        else:
            for i in range(0, len(m), 64):
                w('      "%s"%s' % (m[i:i + 64], "," if i + 64 >= len(m) else ""))
        w('      "%s" },' % t["sig"])
    w("};")
    w("struct Ed25519Decode { const char *name, *enc; bool ok; };")
    w("static const Ed25519Decode kDecode[] = {")
    for name, enc, ok in decode:
        w('    { "%s", "%s", %s },' % (name, enc.hex(), "true" if ok else "false"))
    w("};")
    w("struct Ed25519Case { const char *name, *pk, *msg, *sig; bool ok; };")
    w("static const Ed25519Case kStrict[] = {")
    for name, pk, m, sg, ok in cases:
        w('    { "%s", "%s", "%s",' % (name, pk.hex(), m.hex()))
        w('      "%s", %s },' % (sg.hex(), "true" if ok else "false"))
    w("};")
    print("\n".join(out))


if __name__ == "__main__":
    main()
