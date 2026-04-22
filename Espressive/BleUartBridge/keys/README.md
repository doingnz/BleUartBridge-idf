# keys/

Holds the ECDSA P-256 keypair that signs DFU firmware images.

- `signing_private.pem` — **SECRET**. Gitignored. Signs firmware builds.
  Keep it out of CI artefacts, shared drives, and untrusted hosts.
  If it leaks, every device running a firmware built with the matching
  public key will accept malicious updates from whoever has it.

- `signing_public.pem` — public. Also uncommitted (generated alongside the
  private key), but not secret. It is embedded into every firmware build
  via `main/signing_pubkey.h`, which **is** committed.

## Generating a fresh keypair

```
python tools/keygen.py
python tools/embed_pubkey.py
idf.py build
```

The first command writes both PEMs into this directory. The second rewrites
`main/signing_pubkey.h` with the new public key. The third compiles the
firmware with that key baked in.

## Rotating keys

Replacing `signing_public.pem` **invalidates every device in the field** that
is running a firmware built with the old public key. Before rotating:

1. Generate a transitional firmware that trusts **both** the old and the new
   public key. (Today the code trusts exactly one key; a multi-key extension
   would live in `main/signing_pubkey.h` and `verify_ecdsa.c`.)
2. Roll the transitional firmware to every device in the fleet.
3. Only then retire the old key and ship firmware signed by the new key.

Skipping step 1 bricks your fleet for DFU purposes (you'd have to physically
reflash via USB/UART).

## Backup

A single lost `signing_private.pem` means you can never deliver another OTA
to the existing fleet. Keep an offline backup — e.g. printed on paper in a
safe, stored on an air-gapped USB stick, or held in a hardware security
module.
