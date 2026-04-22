#!/usr/bin/env python3
"""
keygen.py — generate an ECDSA P-256 signing keypair for DFU image signing.

Writes:
    keys/signing_private.pem   (32-byte private scalar, PEM-encoded)
    keys/signing_public.pem    (65-byte uncompressed SEC1 point, PEM-encoded)

The private key never leaves this repo (gitignored).  The public key is
committed (via main/signing_pubkey.h after running embed_pubkey.py) and
shipped in every firmware build so devices can verify signatures.

Usage:
    python tools/keygen.py          # generate fresh keys (aborts if they exist)
    python tools/keygen.py --force  # overwrite existing keys

Requires:  pip install cryptography
"""
import argparse
import os
import pathlib
import sys

try:
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import serialization
except ImportError:
    sys.exit("cryptography package not found — run: pip install cryptography")


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
KEYS_DIR  = REPO_ROOT / "keys"
PRIV_PATH = KEYS_DIR / "signing_private.pem"
PUB_PATH  = KEYS_DIR / "signing_public.pem"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--force", action="store_true",
                    help="overwrite existing keys (DANGER: invalidates fleet trust)")
    args = ap.parse_args()

    KEYS_DIR.mkdir(exist_ok=True)
    if (PRIV_PATH.exists() or PUB_PATH.exists()) and not args.force:
        print(f"Key already exists at {PRIV_PATH} or {PUB_PATH}.")
        print("Re-run with --force to replace (this invalidates all devices")
        print("currently trusting the old public key).")
        return 1

    priv = ec.generate_private_key(ec.SECP256R1())
    pub  = priv.public_key()

    PRIV_PATH.write_bytes(priv.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    ))
    # Restrict private-key file mode on POSIX; Windows inherits ACL from the
    # directory, which is good enough for a dev machine.
    try:
        os.chmod(PRIV_PATH, 0o600)
    except OSError:
        pass

    PUB_PATH.write_bytes(pub.public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    ))

    print(f"Wrote {PRIV_PATH}  (keep this file private)")
    print(f"Wrote {PUB_PATH}")
    print()
    print("Next steps:")
    print("  1. python tools/embed_pubkey.py    # overwrites main/signing_pubkey.h")
    print("  2. idf.py build                    # compiles the fleet's trusted pubkey in")
    print("  3. python tools/sign_fw.py         # signs build/BleUartBridge.bin for OTA")
    return 0


if __name__ == "__main__":
    sys.exit(main())
