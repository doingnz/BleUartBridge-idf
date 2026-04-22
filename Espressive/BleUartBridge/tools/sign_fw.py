#!/usr/bin/env python3
"""
sign_fw.py — sign a built firmware binary with the ECDSA P-256 private key.

Produces two artefacts beside the .bin:
    build/BleUartBridge.bin.sig    64 bytes:  r[32] || s[32] (big-endian)
    build/BleUartBridge.bin.sha    32 bytes:  SHA-256 of the image (sidecar)

The device's DFU_OP_START message carries size, sha256, version, and the raw
64-byte signature.  The host tooling (bpconnect, BpDfuCli) builds the START
payload from these files.

Usage:
    python tools/sign_fw.py                         # signs build/BleUartBridge.bin
    python tools/sign_fw.py path/to/firmware.bin    # signs a specific file

Requires:  pip install cryptography
"""
import argparse
import hashlib
import pathlib
import sys

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils
except ImportError:
    sys.exit("cryptography package not found — run: pip install cryptography")


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
PRIV_PATH = REPO_ROOT / "keys" / "signing_private.pem"
DEFAULT_BIN = REPO_ROOT / "build" / "BleUartBridge.bin"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("bin", nargs="?", default=str(DEFAULT_BIN),
                    help="path to firmware .bin (default: build/BleUartBridge.bin)")
    args = ap.parse_args()

    bin_path = pathlib.Path(args.bin)
    if not bin_path.exists():
        print(f"Missing firmware binary: {bin_path}")
        return 1
    if not PRIV_PATH.exists():
        print(f"Missing private key: {PRIV_PATH}")
        print("Run tools/keygen.py first.")
        return 1

    image = bin_path.read_bytes()
    sha   = hashlib.sha256(image).digest()

    priv = serialization.load_pem_private_key(PRIV_PATH.read_bytes(), password=None)
    if not isinstance(priv, ec.EllipticCurvePrivateKey):
        print("signing_private.pem is not an EC private key")
        return 1

    der_sig = priv.sign(image, ec.ECDSA(hashes.SHA256()))
    r, s = utils.decode_dss_signature(der_sig)
    raw_sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")

    sig_path = bin_path.with_suffix(bin_path.suffix + ".sig")
    sha_path = bin_path.with_suffix(bin_path.suffix + ".sha")
    sig_path.write_bytes(raw_sig)
    sha_path.write_bytes(sha)

    print(f"Image  : {bin_path}  ({len(image):,} bytes)")
    print(f"SHA-256: {sha.hex()}")
    print(f"Wrote  : {sig_path}  (64 B raw r||s)")
    print(f"         {sha_path}  (32 B digest sidecar)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
