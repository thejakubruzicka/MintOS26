#!/usr/bin/env python3
"""Tests the MintOS crypto library (kernel/user/crypto) on the build host.

SHA-2, HMAC and HKDF are compared with Python's hashlib/hmac on random inputs
(including block-boundary lengths). AEADs and X25519 are checked against
published test vectors (RFC 8439, FIPS-197, NIST GCM, RFC 7748), round trips,
and tamper detection; X25519 is also cross-checked against OpenSSL when an
OpenSSL 3 command line is available.
"""
import hashlib
import hmac
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CRYPTO = ROOT / "kernel/user/crypto"
failures = 0


def check(ok, name):
    global failures
    print(("PASS: " if ok else "FAIL: ") + name)
    if not ok:
        failures += 1


def build(tmp):
    exe = Path(tmp) / "crypto-test"
    srcs = [ROOT / "tools/test_crypto.c", CRYPTO / "sha2.c", CRYPTO / "aead.c", CRYPTO / "x25519.c"]
    cc = os.environ.get("CC", "clang")
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(exe)] + [str(s) for s in srcs], check=True)
    return exe


def run(exe, *args):
    out = subprocess.run([str(exe)] + [a if a else "-" for a in args], capture_output=True, text=True, check=True)
    return out.stdout.strip()


def hkdf_ref(name, salt, ikm, info, length):
    hlen = hashlib.new(name).digest_size
    prk = hmac.new(salt or bytes(hlen), ikm, name).digest()
    t, okm, i = b"", b"", 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([i]), name).digest()
        okm += t
        i += 1
    return okm[:length]


def main():
    rnd = random.Random(2026)
    with tempfile.TemporaryDirectory(prefix="mint-crypto-") as tmp:
        exe = build(tmp)
        names = {"256": "sha256", "384": "sha384", "512": "sha512"}
        ok = True
        for alg, name in names.items():
            for n in (0, 1, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 1000, 4097):
                data = bytes(rnd.getrandbits(8) for _ in range(n))
                if run(exe, "hash", alg, data.hex()) != hashlib.new(name, data).hexdigest():
                    ok = False
                    print(f"  hash mismatch {name} len={n}")
        check(ok, "SHA-256/384/512 match hashlib at every padding boundary, with incremental updates")
        big = b"a" * 300000
        check(run(exe, "hash", "512", big.hex()) == hashlib.sha512(big).hexdigest(), "SHA-512 of 300 KB matches hashlib")

        ok = True
        for alg, name in names.items():
            for kn in (0, 16, 64, 128, 200):
                key = bytes(rnd.getrandbits(8) for _ in range(kn))
                data = bytes(rnd.getrandbits(8) for _ in range(rnd.randint(0, 300)))
                if run(exe, "hmac", alg, key.hex(), data.hex()) != hmac.new(key, data, name).hexdigest():
                    ok = False
        check(ok, "HMAC matches Python hmac, including keys longer than the block size")

        ok = True
        for alg, name in names.items():
            for _ in range(6):
                salt = bytes(rnd.getrandbits(8) for _ in range(rnd.choice([0, 13, 32])))
                ikm = bytes(rnd.getrandbits(8) for _ in range(rnd.randint(1, 80)))
                info = bytes(rnd.getrandbits(8) for _ in range(rnd.randint(0, 40)))
                length = rnd.choice([1, 32, 48, 64, 100, 255])
                if run(exe, "hkdf", alg, salt.hex(), ikm.hex(), info.hex(), str(length)) != hkdf_ref(name, salt, ikm, info, length).hex():
                    ok = False
        check(ok, "HKDF extract/expand matches an hmac-based reference (RFC 5869)")

        secret = bytes(range(32))
        label_ref = hkdf_ref("sha256", b"", b"", b"", 1)  # unused, keeps structure obvious
        info = (32).to_bytes(2, "big") + bytes([len(b"tls13 derived")]) + b"tls13 derived" + bytes([32]) + hashlib.sha256(b"").digest()
        t = hmac.new(secret, info + b"\x01", "sha256").digest()
        check(run(exe, "label", "256", secret.hex(), "derived", hashlib.sha256(b"").hexdigest(), "32") == t.hex(),
              "HKDF-Expand-Label builds the TLS 1.3 HkdfLabel (\"tls13 \" prefix, lengths, context)")
        del label_ref

        # RFC 8439 2.3.2 ChaCha20 block function
        blk = run(exe, "chacha", bytes(range(32)).hex(), "1", "000000090000004a00000000")
        check(blk.startswith("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"), "ChaCha20 block matches RFC 8439 2.3.2")
        # RFC 8439 2.8.2 AEAD
        pt = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it."
        key = bytes(range(0x80, 0xA0))
        sealed = run(exe, "seal", "3", key.hex(), "070000004041424344454647", "50515253c0c1c2c3c4c5c6c7", pt.hex())
        check(sealed.startswith("d31a8d34648e60db7b86afbc53ef7ec2") and sealed.endswith("1ae10b594f09e26a7e902ecbd0600691"),
              "ChaCha20-Poly1305 matches RFC 8439 2.8.2 (ciphertext and tag)")
        # FIPS-197 C.1 / C.3
        check(run(exe, "aes", "1", "000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff") == "69c4e0d86a7b0430d8cdb78070b4c55a",
              "AES-128 matches FIPS-197 C.1")
        check(run(exe, "aes", "2", bytes(range(32)).hex(), "00112233445566778899aabbccddeeff") == "8ea2b7ca516745bfeafc49904b496089",
              "AES-256 matches FIPS-197 C.3")
        # NIST GCM test cases 1, 2, 13, 14
        z = "00" * 32
        check(run(exe, "seal", "1", z, "00" * 12, "", "") == "58e2fccefa7e3061367f1d57a4e7455a", "AES-128-GCM matches NIST test case 1")
        check(run(exe, "seal", "1", z, "00" * 12, "", "00" * 16) == "0388dace60b6a392f328c2b971b2fe78ab6e47d42cec13bdf53a67b21257bddf",
              "AES-128-GCM matches NIST test case 2")
        check(run(exe, "seal", "2", z, "00" * 12, "", "") == "530f8afbc74536b9a963b4f1c4cb738b", "AES-256-GCM matches NIST test case 13")
        check(run(exe, "seal", "2", z, "00" * 12, "", "00" * 16) == "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919",
              "AES-256-GCM matches NIST test case 14")

        ok = True
        for kind in ("1", "2", "3"):
            for n in (0, 1, 15, 16, 17, 63, 64, 65, 1000):
                k = bytes(rnd.getrandbits(8) for _ in range(32)).hex()
                nonce = bytes(rnd.getrandbits(8) for _ in range(12)).hex()
                aad = bytes(rnd.getrandbits(8) for _ in range(rnd.randint(0, 40))).hex()
                p = bytes(rnd.getrandbits(8) for _ in range(n)).hex()
                c = run(exe, "seal", kind, k, nonce, aad, p)
                if run(exe, "open", kind, k, nonce, aad, c) != p:
                    ok = False
                bad = bytearray(bytes.fromhex(c)); bad[rnd.randrange(len(bad))] ^= 1
                if run(exe, "open", kind, k, nonce, aad, bad.hex()) != "auth-fail":
                    ok = False
                if aad and run(exe, "open", kind, k, nonce, aad[:-2], c) != "auth-fail":
                    ok = False
        check(ok, "all three AEADs round-trip and reject a flipped bit or altered associated data")

        # RFC 7748 5.2 and 6.1
        check(run(exe, "x25519", "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                  "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c") ==
              "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", "X25519 matches RFC 7748 5.2")
        alice = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"
        bob = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"
        apub = run(exe, "x25519base", alice)
        bpub = run(exe, "x25519base", bob)
        check(apub == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a" and
              bpub == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", "X25519 public keys match RFC 7748 6.1")
        check(run(exe, "x25519", alice, bpub) == run(exe, "x25519", bob, apub) ==
              "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", "X25519 shared secret matches RFC 7748 6.1")
        # 1000 iterations of RFC 7748 5.2 (k = X25519(k, u); u = old k)
        k = u = "0900000000000000000000000000000000000000000000000000000000000000"
        for _ in range(1000):
            k, u = run(exe, "x25519", k, u), k
        check(k == "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", "X25519 1,000-iteration test matches RFC 7748 5.2")

        openssl = next((p for p in ("/opt/homebrew/opt/openssl/bin/openssl", shutil.which("openssl") or "") if p and Path(p).exists()), None)
        if openssl and "OpenSSL 3" in subprocess.run([openssl, "version"], capture_output=True, text=True).stdout + \
                subprocess.run([openssl, "version"], capture_output=True, text=True).stdout.replace("OpenSSL 4", "OpenSSL 3"):
            ok = True
            for _ in range(5):
                a = bytes(rnd.getrandbits(8) for _ in range(32)); b = bytes(rnd.getrandbits(8) for _ in range(32))
                pa = Path(tmp) / "a.der"; pb = Path(tmp) / "b.der"
                pa.write_bytes(bytes.fromhex("302e020100300506032b656e04220420") + a)
                pb.write_bytes(bytes.fromhex("302a300506032b656e032100") + bytes.fromhex(run(exe, "x25519base", b.hex())))
                ref = subprocess.run([openssl, "pkeyutl", "-derive", "-inkey", str(pa), "-keyform", "DER", "-peerkey", str(pb), "-peerform", "DER"],
                                     capture_output=True, check=True).stdout.hex()
                if run(exe, "x25519", a.hex(), run(exe, "x25519base", b.hex())) != ref:
                    ok = False
            check(ok, "X25519 shared secrets match OpenSSL on random keys")
        else:
            print("SKIP: OpenSSL 3 not found; X25519 cross-check against OpenSSL skipped")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
