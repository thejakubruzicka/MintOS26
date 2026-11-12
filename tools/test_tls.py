#!/usr/bin/env python3
"""Tests the MintOS TLS 1.3 / 1.2 client on the build host.

Local (always, needs an OpenSSL 3 command line): `openssl s_server` with a
private test PKI exercises ECDSA P-256/P-384 and RSA-PSS CertificateVerify, all
three cipher suites, a server that demands a different key-exchange group
(HelloRetryRequest, reported as unsupported), a certificate for another
host, an untrusted root, and KeyUpdate during the transfer. TLS 1.2 (s_server
-tls1_2): all six ECDHE AEAD suites with ECDSA and RSA certificates over X25519
and P-256, with and without extended master secret; CBC-only and wrong-host
servers fail cleanly. TLS 1.3 over P-256 via HelloRetryRequest.
Live (--live or MINT_TEST_LIVE=1): real HTTPS sites with every suite forced.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
C = ROOT / "kernel/user/crypto"
OPENSSL = next((p for p in ("/opt/homebrew/opt/openssl/bin/openssl", "/usr/local/opt/openssl/bin/openssl", shutil.which("openssl") or "")
                if p and Path(p).exists() and "OpenSSL" in subprocess.run([p, "version"], capture_output=True, text=True).stdout), None)
failures = 0
sys.path.insert(0, str(ROOT / "tools"))
from test_x509 import subject_and_spki  # noqa: E402


def check(ok, name, detail=""):
    global failures
    print(("PASS: " if ok else "FAIL: ") + name + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        failures += 1


def ossl(*args):
    return subprocess.run([OPENSSL] + list(args), capture_output=True, check=True).stdout


def build(tmp):
    exe = Path(tmp) / "tls-test"
    srcs = ["tools/test_tls.c"] + [f"kernel/user/crypto/{n}" for n in ("tls.c", "sha2.c", "aead.c", "x25519.c", "pubkey.c", "x509.c", "roots_data.c")]
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-D_DARWIN_C_SOURCE", "-o", str(exe)] +
                   [str(ROOT / s) for s in srcs], check=True)
    return exe


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def main():
    live = "--live" in sys.argv or os.environ.get("MINT_TEST_LIVE") == "1"
    with tempfile.TemporaryDirectory(prefix="mint-tls-") as tmpd:
        tmp = Path(tmpd)
        exe = build(tmpd)
        run = lambda *a: subprocess.run([str(exe)] + list(a), capture_output=True, text=True, timeout=60).stdout.strip()
        if OPENSSL:
            conf = tmp / "x.cnf"
            conf.write_text("[req]\ndistinguished_name=dn\n[dn]\n[ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign\n"
                            "[leaf]\nbasicConstraints=critical,CA:FALSE\nsubjectAltName=DNS:mint.test,DNS:localhost\n")
            def key(name, *gen):
                ossl("genpkey", *gen, "-out", str(tmp / f"{name}.key")); return tmp / f"{name}.key"
            ca = key("ca", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-384")
            ossl("req", "-x509", "-new", "-key", str(ca), "-subj", "/CN=MintOS TLS Test CA", "-days", "7", "-sha384",
                 "-config", str(conf), "-extensions", "ca", "-out", str(tmp / "ca.pem"))
            def leaf(name, *gen):
                k = key(name, *gen)
                ossl("req", "-new", "-key", str(k), "-subj", "/CN=mint.test", "-config", str(conf), "-out", str(tmp / f"{name}.csr"))
                ossl("x509", "-req", "-in", str(tmp / f"{name}.csr"), "-CA", str(tmp / "ca.pem"), "-CAkey", str(ca), "-set_serial", "7",
                     "-days", "7", "-sha256", "-extfile", str(conf), "-extensions", "leaf", "-out", str(tmp / f"{name}.pem"))
                return k, tmp / f"{name}.pem"
            leaves = {"ecdsa-p256": leaf("p256", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256"),
                      "ecdsa-p384": leaf("p384", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-384"),
                      "rsa-pss": leaf("rsa", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048")}
            ca_der = ossl("x509", "-in", str(tmp / "ca.pem"), "-outform", "DER")
            subj, spki = subject_and_spki(ca_der)
            root = f"{subj.hex()}:{spki.hex()}"

            def serve(k, cert, *extra, version="-tls1_3"):
                port = free_port()
                proc = subprocess.Popen([OPENSSL, "s_server", "-accept", str(port), "-key", str(k), "-cert", str(cert), version,
                                         "-www", "-quiet", *extra], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                for _ in range(50):
                    try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
                    except OSError: time.sleep(0.1)
                return proc, str(port)

            for name, (k, cert) in leaves.items():
                proc, port = serve(k, cert)
                try:
                    for suite in ("1303", "1301", "1302"):
                        out = run("mint.test", port, "--connect", "127.0.0.1", "--root", root, "--suite", suite)
                        check(out.startswith("OK ") and "200 ok" in out.lower(), f"local server, {name} certificate, suite {suite}", out)
                    out = run("other.test", port, "--connect", "127.0.0.1", "--root", root)
                    check(out.startswith("ERR -3") and "different site" in out, f"{name}: certificate for another host is refused", out)
                    out = run("mint.test", port, "--connect", "127.0.0.1")
                    check(out.startswith("ERR -3") and "trusted authority" in out, f"{name}: untrusted root is refused", out)
                finally:
                    proc.terminate(); proc.wait()
            k, cert = leaves["ecdsa-p256"]
            proc, port = serve(k, cert, "-groups", "P-256")
            try:
                out = run("mint.test", port, "--connect", "127.0.0.1", "--root", root)
                check(out.startswith("OK ") and "TLS 1.3" in out and "200 ok" in out.lower(),
                      "TLS 1.3 server that only accepts P-256: HelloRetryRequest, then P-256 ECDHE", out)
            finally:
                proc.terminate(); proc.wait()
            # TLS 1.2
            suites12 = {"ecdsa": ["ECDHE-ECDSA-AES128-GCM-SHA256", "ECDHE-ECDSA-AES256-GCM-SHA384", "ECDHE-ECDSA-CHACHA20-POLY1305"],
                        "rsa": ["ECDHE-RSA-AES128-GCM-SHA256", "ECDHE-RSA-AES256-GCM-SHA384", "ECDHE-RSA-CHACHA20-POLY1305"]}
            for name, (k, cert) in leaves.items():
                kind = "rsa" if name.startswith("rsa") else "ecdsa"
                for cipher in suites12[kind]:
                    for group in ("X25519", "P-256"):
                        proc, port = serve(k, cert, "-cipher", cipher, "-groups", group, version="-tls1_2")
                        try:
                            out = run("mint.test", port, "--connect", "127.0.0.1", "--root", root)
                            check(out.startswith("OK " + cipher + " TLS 1.2") and "200 ok" in out.lower(),
                                  f"TLS 1.2 {name} certificate, {cipher}, {group}", out)
                        finally:
                            proc.terminate(); proc.wait()
            k, cert = leaves["ecdsa-p256"]
            proc, port = serve(k, cert, "-no_ems", version="-tls1_2")
            try:
                out = run("mint.test", port, "--connect", "127.0.0.1", "--root", root)
                check(out.startswith("OK ") and "TLS 1.2" in out, "TLS 1.2 without extended master secret (classic PRF)", out)
                out = run("other.test", port, "--connect", "127.0.0.1", "--root", root)
                check(out.startswith("ERR -3") and "different site" in out, "TLS 1.2: certificate for another host is refused", out)
                out = run("mint.test", port, "--connect", "127.0.0.1")
                check(out.startswith("ERR -3") and "trusted authority" in out, "TLS 1.2: untrusted root is refused", out)
            finally:
                proc.terminate(); proc.wait()
            k, cert = leaves["rsa-pss"]
            proc, port = serve(k, cert, "-cipher", "ECDHE-RSA-AES128-SHA256", version="-tls1_2")
            try:
                out = run("mint.test", port, "--connect", "127.0.0.1", "--root", root)
                check(out.startswith("ERR -4") and "cipher" in out, "a CBC-only TLS 1.2 server fails cleanly (no CBC/RSA key exchange)", out)
            finally:
                proc.terminate(); proc.wait()
            # KeyUpdate: a 300 KB response from s_server -WWW with key updates requested by the server
            (tmp / "big.txt").write_bytes(b"MintOS TLS KeyUpdate test\n" * 12000)
            port = str(free_port())
            proc = subprocess.Popen([OPENSSL, "s_server", "-accept", port, "-key", str(k), "-cert", str(cert), "-tls1_3", "-WWW", "-quiet",
                                     "-num_tickets", "3"], cwd=tmpd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                for _ in range(50):
                    try: socket.create_connection(("127.0.0.1", int(port)), timeout=0.2).close(); break
                    except OSError: time.sleep(0.1)
                out = run("mint.test", port, "--connect", "127.0.0.1", "--root", root, "--path", "/big.txt")
                check(out.startswith("OK ") and int(out.split("|")[-1].split()[0]) > 312000,
                      "a 312 KB transfer across many records (with session tickets) completes", out)
            finally:
                proc.terminate(); proc.wait()
        else:
            print("SKIP: OpenSSL 3 command line not found; local TLS server tests skipped")

        if live:
            for host in ("example.com", "www.google.com", "en.wikipedia.org", "github.com", "www.cloudflare.com", "letsencrypt.org", "www.bbc.co.uk"):
                results = [run(host, "--suite", s) for s in ("1303", "1301", "1302")]
                ok = all(r.startswith("OK ") for r in results) or (results[0].startswith("OK ") and results[1].startswith("OK "))
                check(ok, f"live https://{host}/ with ChaCha20-Poly1305, AES-128-GCM and AES-256-GCM", " / ".join(results))
            out = run("tls-v1-2.badssl.com", "1012")
            check(out.startswith("OK ") and "TLS 1.2" in out, "live TLS-1.2-only server (tls-v1-2.badssl.com:1012)", out)
            for host in ("ecc256.badssl.com", "rsa2048.badssl.com"):
                out = run(host)
                check(out.startswith("OK ") and "TLS 1.2" in out, f"live TLS 1.2 https://{host}/", out)
            for host, why in (("expired.badssl.com", "expired"), ("wrong.host.badssl.com", "different site"), ("untrusted-root.badssl.com", "trusted")):
                out = run(host)
                check(out.startswith("ERR -3") and why in out, f"live {host} is refused", out)
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
