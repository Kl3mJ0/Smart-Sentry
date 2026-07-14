#!/usr/bin/env python3
"""
enroll.py - Smart Sentry one-command device enrolment.

Self-contained: needs only the `cryptography` package.

    python enroll.py init-ca
        Create the ONE company root CA (run once, ever) and print the
        root-CA public key block to paste into SS1's sentry_auth.c.

    python enroll.py add --id 1002 --name line2-gateway
        Issue a certificate for a new device from that CA, generate its
        sentry_client_creds.h, and open it in Notepad ready to copy into
        the device's project. SS1 needs NO changes to accept it.

    python enroll.py ss1-header
        Re-print the root-CA public key block for SS1.

All certs from one CA authenticate against the same SS1 (like HTTPS: SS1
trusts the authority, not each individual device).
"""
import argparse, json, os, sys, time, subprocess, struct
from pathlib import Path
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

CA_DIR   = Path("ca")
CLIENTS  = Path("clients")
CA_KEY   = CA_DIR / "root_ca_key.pem"
CA_PUB   = CA_DIR / "root_ca_pub.bin"

CERT_VERSION = 1
CERT_BODY_FMT = "<B3sII64s"          # version, rsv, id, not_after, pubkey  = 76B

# ---------- crypto helpers ----------
def pubkey_xy(priv):
    n = priv.public_key().public_numbers()
    return n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")

def sign_raw(priv, msg):
    der = priv.sign(msg, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")

def c_array(name, data, per=8):
    out = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per):
        out.append("\t" + ", ".join(f"0x{b:02x}" for b in data[i:i+per]) + ",")
    out.append("};")
    return "\n".join(out)

def ss1_header_text():
    pub = CA_PUB.read_bytes()
    return ("/* Root CA public key (P-256 X||Y). Paste into SS1 sentry_auth.c */\n"
            + c_array("root_ca_pubkey", pub))

# ---------- commands ----------
def init_ca():
    if CA_KEY.exists():
        sys.exit("Root CA already exists - refusing to overwrite (this would "
                 "break every device already enrolled). Delete ca/ only if you "
                 "really mean to start over.")
    CA_DIR.mkdir(exist_ok=True)
    ca = ec.generate_private_key(ec.SECP256R1())
    CA_KEY.write_bytes(ca.private_bytes(serialization.Encoding.PEM,
                                        serialization.PrivateFormat.PKCS8,
                                        serialization.NoEncryption()))
    CA_PUB.write_bytes(pubkey_xy(ca))
    print("Root CA created.\n")
    print(ss1_header_text())
    print("\nPaste the block above into SS1's sentry_auth.c, flash SS1 once, "
          "then enrol devices with:  python enroll.py add --id N --name NAME")

def add(client_id, name, days, out, open_editor):
    if not CA_KEY.exists():
        sys.exit("No CA found. Run:  python enroll.py init-ca")
    CLIENTS.mkdir(exist_ok=True)
    ca = serialization.load_pem_private_key(CA_KEY.read_bytes(), password=None)

    client = ec.generate_private_key(ec.SECP256R1())
    pub_xy = pubkey_xy(client)
    not_after = 0 if days == 0 else int(time.time()) + days * 86400
    body = struct.pack(CERT_BODY_FMT, CERT_VERSION, b"\x00\x00\x00",
                       client_id, not_after, pub_xy)
    cert = body + sign_raw(ca, body)                    # 140-byte certificate
    priv = client.private_numbers().private_value.to_bytes(32, "big")

    base = CLIENTS / f"{client_id}_{name}"
    base.with_name(base.name + "_key.pem").write_bytes(
        client.private_bytes(serialization.Encoding.PEM,
                             serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption()))
    base.with_name(base.name + "_cert.bin").write_bytes(cert)
    base.with_name(base.name + "_meta.json").write_text(
        json.dumps({"client_id": client_id, "name": name,
                    "not_after": not_after, "cert_len": len(cert)}, indent=2))

    header = "\n".join([
        "#pragma once", "", "#include <stdint.h>", "",
        f"#define CLIENT_ID {client_id}",
        f"#define CLIENT_CERT_LEN {len(cert)}",
        f"#define CLIENT_PRIVKEY_LEN {len(priv)}", "",
        c_array("client_cert", cert), "",
        c_array("client_privkey", priv), "",
    ])
    out_path = Path(out) if out else Path("generated") / f"{client_id}_{name}_sentry_client_creds.h"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(header, encoding="utf-8")

    print(f"Enrolled id={client_id} ({name}), expires="
          f"{'never' if not_after==0 else time.ctime(not_after)}")
    print(f"Header written: {out_path}")
    print("Copy client_cert[] and client_privkey[] into this device's "
          "src/sentry_client_creds.h, then build/flash that device. SS1 unchanged.")

    if open_editor:
        try:
            if sys.platform.startswith("win"):
                subprocess.Popen(["notepad.exe", str(out_path)])
            elif sys.platform == "darwin":
                subprocess.Popen(["open", "-t", str(out_path)])
            else:
                subprocess.Popen(["xdg-open", str(out_path)])
        except Exception as e:
            print(f"(could not auto-open editor: {e})")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("init-ca")
    p = sub.add_parser("add")
    p.add_argument("--id", type=int, required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--days", type=int, default=365, help="0 = never expires")
    p.add_argument("--out", default=None, help="header output path (default: generated/...)")
    p.add_argument("--no-open", action="store_true", help="don't open Notepad")
    sub.add_parser("ss1-header")
    a = ap.parse_args()
    if a.cmd == "init-ca": init_ca()
    elif a.cmd == "add": add(a.id, a.name, a.days, a.out, not a.no_open)
    elif a.cmd == "ss1-header": print(ss1_header_text())
