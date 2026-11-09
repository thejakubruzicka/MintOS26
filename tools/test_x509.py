#!/usr/bin/env python3
"""Tests MintOS signature verification and X.509 chain validation on the host.

OpenSSL (3.x) generates RSA and EC keys, PKCS#1 v1.5 / PSS / ECDSA
signatures and a three-level test PKI; the MintOS code must accept the valid
ones and reject tampered signatures, wrong hosts, expired or not-yet-valid
certificates, missing intermediates and non-CA issuers. With --live (or
MINT_TEST_LIVE=1), real server chains are fetched and validated against the
bundled Mozilla roots.
"""
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CRYPTO = ROOT / "kernel/user/crypto"
failures = 0
OPENSSL = next((p for p in ("/opt/homebrew/opt/openssl/bin/openssl", "/usr/local/opt/openssl/bin/openssl", shutil.which("openssl") or "")
                if p and Path(p).exists() and "OpenSSL" in subprocess.run([p, "version"], capture_output=True, text=True).stdout), None)


def check(ok, name):
    global failures
    print(("PASS: " if ok else "FAIL: ") + name)
    if not ok:
        failures += 1


def ossl(*args, input_bytes=None):
    return subprocess.run([OPENSSL] + list(args), input=input_bytes, capture_output=True, check=True).stdout


def build(tmp):
    exe = Path(tmp) / "x509-test"
    srcs = [ROOT / "tools/test_x509.c", CRYPTO / "sha2.c", CRYPTO / "pubkey.c", CRYPTO / "x509.c", CRYPTO / "roots_data.c"]
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(exe)] + [str(s) for s in srcs], check=True)
    return exe


def run(exe, *args):
    return subprocess.run([str(exe)] + list(args), capture_output=True, text=True, check=True).stdout.strip()


def der_children(buf, at):
    def tlv(at):
        length = buf[at + 1]; hl = 2
        if length & 0x80:
            n = length & 0x7F; length = int.from_bytes(buf[at + 2:at + 2 + n], "big"); hl += n
        return hl, length
    hl, ln = tlv(at)
    i, end, out = at + hl, at + hl + ln, []
    while i < end:
        h, l = tlv(i); out.append((buf[i], i, h + l)); i += h + l
    return out


def subject_and_spki(der):
    fields = der_children(der, der_children(der, 0)[0][1])
    k = 1 if fields[0][0] == 0xA0 else 0
    s, p = fields[k + 4], fields[k + 5]
    return der[s[1]:s[1] + s[2]], der[p[1]:p[1] + p[2]]


def main():
    if not OPENSSL:
        print("SKIP: OpenSSL 3 command line not found; signature and certificate tests need it")
        return
    live = "--live" in sys.argv or os.environ.get("MINT_TEST_LIVE") == "1"
    rnd = random.Random(7)
    with tempfile.TemporaryDirectory(prefix="mint-x509-") as tmpd:
        tmp = Path(tmpd)
        exe = build(tmpd)

        def key(name, *gen):
            path = tmp / f"{name}.pem"
            ossl("genpkey", *gen, "-out", str(path))
            spki = ossl("pkey", "-in", str(path), "-pubout", "-outform", "DER")
            return path, spki

        keys = {
            "rsa2048": key("rsa2048", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048"),
            "rsa4096": key("rsa4096", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:4096"),
            "p256": key("p256", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256"),
            "p384": key("p384", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-384"),
        }
        msg = bytes(rnd.getrandbits(8) for _ in range(300))
        (tmp / "msg.bin").write_bytes(msg)

        def sign(keyname, digest, *opts):
            return ossl("dgst", f"-{digest}", "-sign", str(keys[keyname][0]), *opts, str(tmp / "msg.bin"))

        cases = [
            ("rsa2048", "sha256", 0x0401, []), ("rsa2048", "sha384", 0x0501, []), ("rsa4096", "sha512", 0x0601, []),
            ("rsa2048", "sha256", 0x0804, ["-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest"]),
            ("rsa4096", "sha384", 0x0805, ["-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest"]),
            ("rsa2048", "sha512", 0x0806, ["-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest"]),
            ("p256", "sha256", 0x0403, []), ("p384", "sha384", 0x0503, []),
            ("p256", "sha384", 0x7F02, []), ("p384", "sha256", 0x7F01, []),
        ]
        for keyname, digest, scheme, opts in cases:
            spki = keys[keyname][1].hex()
            ok_all = True
            for _ in range(3):
                sig = sign(keyname, digest, *opts)
                if run(exe, "verify", str(scheme), spki, msg.hex(), sig.hex()) != "1":
                    ok_all = False
                bad = bytearray(sig); bad[len(bad) // 2] ^= 0x10
                if run(exe, "verify", str(scheme), spki, msg.hex(), bad.hex()) != "0":
                    ok_all = False
                if run(exe, "verify", str(scheme), spki, (msg + b"!").hex(), sig.hex()) != "0":
                    ok_all = False
            check(ok_all, f"{keyname} {digest} scheme 0x{scheme:04x}: valid signatures verify; tampered signature or message rejected")
        sig = sign("rsa2048", "sha256")
        check(run(exe, "verify", str(0x0403), keys["rsa2048"][1].hex(), msg.hex(), sig.hex()) == "0",
              "a signature scheme that does not match the key type is rejected")

        # --- a three-level PKI: P-384 root -> RSA intermediate -> P-256 leaf
        def cfg(text):
            p = tmp / f"cfg{rnd.getrandbits(32)}.cnf"; p.write_text(text); return str(p)
        ext_ca = cfg("[v3]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n")
        ext_leaf = cfg("[v3]\nbasicConstraints=critical,CA:FALSE\nsubjectAltName=DNS:www.example.test,DNS:*.wild.test,IP:10.0.2.15\n")
        ext_noca = cfg("[v3]\nbasicConstraints=critical,CA:FALSE\n")

        def cert(name, subj, keyname, issuer=None, ext=ext_ca, days="30", digest="sha256", start=None):
            out = tmp / f"{name}.der"
            if issuer is None:
                ossl("req", "-x509", "-new", "-key", str(keys[keyname][0]), "-subj", subj, "-days", days, f"-{digest}",
                     "-extensions", "v3", "-config", ext + "", "-outform", "DER", "-out", str(out))
            else:
                csr = tmp / f"{name}.csr"
                ossl("req", "-new", "-key", str(keys[keyname][0]), "-subj", subj, "-out", str(csr), "-config", ext)
                args = ["x509", "-req", "-in", str(csr), "-CA", str(tmp / f"{issuer[0]}.pem"), "-CAkey", str(keys[issuer[1]][0]),
                        "-set_serial", str(rnd.getrandbits(48)), "-days", days, f"-{digest}", "-extfile", ext, "-extensions", "v3",
                        "-outform", "DER", "-out", str(out)]
                if start:
                    args[args.index("-days"):args.index("-days") + 2] = ["-not_before", start, "-not_after", days]
                ossl(*args)
            ossl("x509", "-inform", "DER", "-in", str(out), "-out", str(tmp / f"{name}.pem"))
            return out.read_bytes()

        # minimal config sections the req command needs
        for c in (ext_ca, ext_leaf, ext_noca):
            Path(c).write_text("[req]\ndistinguished_name=dn\n[dn]\n" + Path(c).read_text())
        root = cert("root", "/CN=MintOS Test Root/O=MintOS", "p384", digest="sha384")
        inter = cert("inter", "/CN=MintOS Test Intermediate/O=MintOS", "rsa2048", issuer=("root", "p384"), digest="sha384")
        leaf = cert("leaf", "/CN=www.example.test", "p256", issuer=("inter", "rsa2048"), ext=ext_leaf)
        subj, spki = subject_and_spki(root)
        roots = f"{subj.hex()}:{spki.hex()}"
        now = str(int(time.time()))
        # The clock is read at each check: certificates minted later in the test have later notBefore times.
        chain = lambda host, *certs, when=None: run(exe, "chain", host, when or str(int(time.time()) + 1), roots, *[c.hex() for c in certs])
        check(chain("www.example.test", leaf, inter) == "0", "leaf -> intermediate -> root chain validates (ECDSA P-384 over RSA, RSA over P-256)")
        check(chain("www.example.test", leaf, inter, root) == "0", "a chain that includes the root itself validates")
        check(chain("a.wild.test", leaf, inter) == "0" and chain("10.0.2.15", leaf, inter) == "0",
              "wildcard and IP-address subjectAltNames match")
        check(chain("b.a.wild.test", leaf, inter) == "4" and chain("wild.test", leaf, inter) == "4" and chain("example.test", leaf, inter) == "4",
              "wildcards match exactly one label; other names are rejected as a wrong host")
        check(chain("WWW.Example.TEST", leaf, inter) == "0", "host names compare case-insensitively")
        check(chain("www.example.test", leaf) == "5", "a missing intermediate leaves the chain untrusted")
        check(chain("www.example.test", leaf, inter, when=str(int(time.time()) + 90 * 86400)) == "2", "an expired certificate is rejected")
        check(chain("www.example.test", leaf, inter, when=str(int(time.time()) - 86400 * 400)) == "3", "a not-yet-valid certificate is rejected")
        check(run(exe, "chain", "www.example.test", now, "mozilla", leaf.hex(), inter.hex()) == "5", "the test PKI is not trusted by the Mozilla root list")
        bad_leaf = bytearray(leaf); bad_leaf[-5] ^= 0x01
        check(chain("www.example.test", bytes(bad_leaf), inter) in ("6", "1"), "a corrupted leaf signature is rejected")
        noca = cert("noca", "/CN=Not A CA", "rsa4096", issuer=("root", "p384"), ext=ext_noca)
        leaf2 = cert("leaf2", "/CN=www.example.test", "p256", issuer=("noca", "rsa4096"), ext=ext_leaf)
        rc = chain("www.example.test", leaf2, noca)
        check(rc == "8", "a certificate issued by a non-CA certificate is rejected" + ("" if rc == "8" else f" (got {rc})"))
        check(run(exe, "chain", "x", now, roots, "3003020100") == "1", "malformed DER is rejected")

        if live:
            ok = True
            for host in ("example.com", "www.google.com", "en.wikipedia.org", "github.com", "www.cloudflare.com", "letsencrypt.org"):
                try:
                    out = subprocess.run([OPENSSL, "s_client", "-connect", f"{host}:443", "-servername", host, "-showcerts"],
                                         input=b"", capture_output=True, timeout=20).stdout.decode(errors="replace")
                except subprocess.TimeoutExpired:
                    print(f"  {host}: timed out"); ok = False; continue
                pems = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", out, re.S)
                ders = [ossl("x509", "-outform", "DER", input_bytes=p.encode()) for p in pems]
                rc = run(exe, "chain", host, str(int(time.time())), "mozilla", *[d.hex() for d in ders]) if ders else "none"
                print(f"  {host}: {len(ders)} certificates -> {rc}")
                ok &= rc == "0"
            check(ok, "live server chains validate against the Mozilla roots")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
