#!/usr/bin/env python3
"""Source contracts for the MintOS TLS 1.3 client, certificate validation and root store."""
import hashlib
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
c = root / "kernel/user/crypto"
tls = (c / "tls.c").read_text()
x509 = (c / "x509.c").read_text()
pubkey = (c / "pubkey.c").read_text()
aead = (c / "aead.c").read_text()
rnd = (c / "random.c").read_text()
roots_c = (c / "roots_data.c").read_text()
make_roots = (root / "tools/make_roots.py").read_text()
pem = (root / "assets/certs/cacert.pem").read_text(encoding="utf-8")
web = (root / "kernel/user/mintde/web.c").read_text()
apps = (root / "kernel/user/mintde/apps.c").read_text()

checks = {
    "TLS 1.3 is preferred; TLS 1.2 only with ECDHE + AEAD suites and the downgrade sentinel checked":
        "type == 43 && elen == 2 && g16(b) == 0x0304" in tls and "DOWNGRADE" in tls and "is_tls12_suite" in tls and
        "if (type != HS_SERVER_KEY_EXCHANGE) return fail(t, MINT_TLS_ERR_PROTOCOL);" in tls and "CBC" in (c / "tls.h").read_text(),
    "TLS 1.2 verifies the ServerKeyExchange signature, uses extended master secret and checks the server Finished":
        "mint_signature_verify(&key, verify_scheme, signed_data" in tls and '"extended master secret"' in tls and
        "if (!mint_ct_equal(expect, m + 4, 12)) return fail(t, MINT_TLS_ERR_DECRYPT);" in tls,
    "P-256 ECDH refuses points that are not on the curve":
        "if (!on_curve(&qx, &qy, c)) return 0;                 /* invalid-curve points are refused */" in pubkey,
    "the server must echo our session id and pick an offered suite":
        "mint_ct_equal(sid_p, t->session_id, 32)" in tls and "if (!offered) return fail(t, MINT_TLS_ERR_PROTOCOL);" in tls,
    "an all-zero X25519 shared secret is rejected":
        "if (!acc) return fail(t, MINT_TLS_ERR_PROTOCOL);" in tls,
    "the certificate chain is validated before CertificateVerify, which must use an allowed TLS 1.3 scheme":
        "mint_x509_verify_chain(certs, lens, count, t->host, t->now, t->roots, t->root_count)" in tls and
        "TLS 1.3, server CertificateVerify" in tls and "MINT_SIG_RSA_PKCS1" not in tls.split("handle_cert_verify")[1].split("}")[0],
    "the server Finished MAC is compared in constant time before application keys are used":
        "if (!mint_ct_equal(expect, m + 4, t->hash_len)) return fail(t, MINT_TLS_ERR_DECRYPT);" in tls and
        tls.find("mint_ct_equal(expect, m + 4") < tls.find('derive_secret(t, t->master, "c ap traffic"'),
    "handshake messages are only accepted in the expected order":
        all(k in tls for k in ["case ST_WAIT_SH:", "case ST_WAIT_EE:", "case ST_WAIT_CERT:", "case ST_WAIT_CV:", "case ST_WAIT_FIN:"]),
    "record and handshake lengths are bounded before copying":
        "len > 16384u + 256u" in tls and "if (t->hs_len + plen > sizeof(t->hs))" in tls and "body > MINT_TLS_HS_MAX - 4u" in tls,
    "secrets are wiped after use and on close":
        "mzero(shared, sizeof(shared)); mzero(t->priv, sizeof(t->priv));" in tls and "mzero(t->master, sizeof(t->master));" in tls,
    "AEAD tags are checked in constant time before any plaintext is released":
        "if (!mint_ct_equal(expect, tag, 16)) return 0;" in aead and aead.find("mint_ct_equal(expect, tag, 16)") < aead.find("gcm_ctr(a, nonce, 2, in, out, len);\n    return 1;"),
    "chain validation checks dates, CA flags, host names and signatures up to a trusted root":
        all(k in x509 for k in ["MINT_CERT_EXPIRED", "MINT_CERT_NOT_YET_VALID", "MINT_CERT_NOT_CA", "mint_x509_matches_host", "check_signature(c, roots[r].spki"]),
    "wildcards cover exactly one non-empty label":
        "wildcard needs a non-empty first label" in x509,
    "RSA keys are limited to 1024-4096 bits and ECDSA public points must lie on the curve":
        "bits < 1024u || bits > 4096u" in pubkey and "if (!on_curve(&qx, &qy, c)) return 0;" in pubkey,
    "big-number results never carry stale high limbs":
        pubkey.count("for (uint32_t i = n; i < BN_LIMBS; ++i) o->v[i] = 0;") == 2,
    "randomness mixes RDSEED/RDRAND with TSC jitter through SHA-512 and re-keys the pool":
        all(k in rnd for k in ["rdseed", "rdrand", "rdtsc", "MINT_SHA512", "Forward secrecy"]),
    "the root store is generated from the Mozilla bundle with fingerprint checks":
        "fingerprint mismatch" in make_roots and "Mozilla" in roots_c and
        f"mint_tls_root_count = {pem.count('BEGIN CERTIFICATE')}u" in roots_c,
    "Web and curl report certificate failures and never offer a bypass":
        "This connection is not private" in web and "SSL certificate problem" in apps,
}
failed = []
for name, ok in checks.items():
    print(("PASS: " if ok else "FAIL: ") + name)
    if not ok:
        failed.append(name)
if failed:
    raise SystemExit(1)
