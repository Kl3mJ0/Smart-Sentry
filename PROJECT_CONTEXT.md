# Smart Sentry - BLE certificate authentication (project context)

Context for Claude Code. Read this before touching any file. This document
describes a two-board nRF54L15 BLE system plus its host-side PKI tooling.
Can be used as a `CLAUDE.md` at the repo root.

---

## What this is

A Smart Sentry (internship) environmental sensor built on **nRF54L15-DK**,
**nRF Connect SDK v3.4.0 / Zephyr 4.4.0**, board target
`nrf54l15dk/nrf54l15/cpuapp`, developed on Windows with the nRF Connect for
VS Code extension (builds run under **sysbuild**).

The sensor exposes temperature + humidity over BLE with an encrypted link.
On top of that we added **certificate-based device pre-approval**: a device
is trusted only if it presents a certificate signed by a company CA. The
trust model is deliberately **HTTPS-like** - a device with a valid cert
connects and authenticates automatically, with no manual BLE pairing step.

There are two firmware apps and one host tool.

### SS1 - the sensor (BLE peripheral)
- Advertises as `Joseph_BLE`.
- GATT: Environmental Sensing Service (temp `0x2A6E` int16 centi-degC,
  humidity `0x2A6F` uint16 centi-%RH) + a custom "Sentry" LED/maintenance
  service. Sensor is an SHT3x on I2C (`i2c21`, addr 0x45), with a fake-data
  fallback if absent.
- Holds the **CA public key** only. It VERIFIES client certs; it never signs.
- Sensitive characteristics (temp/hum/led reads, writes, and notifies) are
  gated behind successful cert authentication - not just encryption.
- Key files: `main.c`, `sentry_auth.c`, `sentry_auth.h`.

### SS2 - the certified gateway / test client (BLE central)
- Scans for `Joseph_BLE`, connects, completes the cert handshake, subscribes
  to temp/humidity and prints them to its own serial.
- Holds a **client certificate + private key** and SIGNS the challenge.
- This is the reference client. A production phone app would implement the
  same three protocol steps.
- Key files: SS2 `main.c`, `sentry_client_creds.h` (generated per device).

### enroll.py - host-side PKI tool (Python, needs only `cryptography`)
- `python enroll.py init-ca` - create the ONE root CA (run once, ever) and
  print the CA public-key C block to paste into SS1's `sentry_auth.c`.
- `python enroll.py add --id N --name X` - issue a cert for a new device from
  that CA, generate its `sentry_client_creds.h`, open it in Notepad.
- `python enroll.py ss1-header` - reprint the CA public-key block.
- Any number of devices enrolled from one CA all authenticate against the
  same SS1. SS1 is a fixed trust anchor and never changes when adding devices.

---

## THE INVARIANT: wire format (do not break)

SS1's verifier, SS2's signer, and enroll.py's issuer must agree byte-for-byte.
If you change any of this, change all three together.

Certificate body (little-endian, 76 bytes), then signatures:

    offset  size  field
    0       1     version (=1)
    1       3     reserved (zero)
    4       4     client_id (uint32)
    8       4     not_after (uint32 unix seconds, 0 = never)
    12      64    client public key (P-256 X||Y, no 0x04 prefix)
    --- 76-byte body ends ---
    76      64    CA signature over the 76-byte body (ECDSA P-256, raw r||s)
    --- 140-byte certificate ends ---

Auth response written by the client = certificate(140) || nonce_signature(64)
= **204 bytes total**. Nonce is 32 bytes.

All ECDSA signatures are **raw r||s (64 bytes)**, NOT DER. PSA
(`psa_verify_message` / `psa_sign_message`) uses raw r||s; the Python side
converts DER->raw. This is the single most common breakage point.

Public keys: the cert stores 64 bytes (X||Y). PSA import needs the `0x04`
uncompressed-point prefix prepended (65 bytes) - SS1/SS2 do this in code.

---

## Auth protocol (per connection)

1. Client connects; LESC "Just Works" pairing brings up an encrypted link
   automatically (see "HTTPS-style" below - no button).
2. SS1 generates a fresh random 32-byte nonce for that connection.
3. Client reads the **challenge** characteristic (the nonce).
4. Client signs the nonce with its private key and writes
   `cert || nonce_sig` (204 B) to the **response** characteristic.
5. SS1 verifies: (a) the cert body is signed by the CA (public key baked in),
   and (b) the nonce signature validates against the cert's public key
   (proof the peer holds the private key - stops stolen-cert replay).
6. Pass -> connection flagged authenticated, sensor data unlocked.
   Fail or 10 s timeout -> disconnect.

Custom UUIDs (128-bit): auth service `7e8f00a0-...`, challenge `7e8f00a1-...`,
response `7e8f00a2-...` (base `-1111-2222-3333-123456789abc`). The Sentry LED
service base is `7e8f0000-...`.

Crypto backend: PSA with the **CRACEN** driver on nRF54L. ECDSA-P256 /
SHA-256. SS1 imports the CA key as `ECC_PUBLIC_KEY` (verify); SS2 imports the
client key as `ECC_KEY_PAIR` (sign).

---

## HTTPS-style trust model (current direction)

The cert is the credential, so the old manual BLE pairing button is being
removed:

- SS1 `auth_pairing_accept` returns `BT_SECURITY_ERR_SUCCESS` unconditionally
  (accept the encrypted link from anyone; the cert check is the real gate).
- Both boards retain their Just Works LTK bond so a rebooted SS2 can restore
  encryption immediately. The certificate remains the real trust gate and is
  checked on every connection with a fresh nonce. Zephyr owns server-side
  GATT/CCC teardown; SS2's sensor subscriptions are volatile and are recreated
  after certificate auth.
- SS2 requests plain `BT_SECURITY_L2` (no `FORCE_PAIR` - that flag caused the
  re-pair-every-time failure and must not come back).
- SS2 is the sole SMP/security initiator. SS1 waits for that request, avoiding
  simultaneous security procedures during reconnect.
- SS2's outer security watchdog is 35 s, after Zephyr's 30 s SMP timeout. A
  10 s watchdog must not be used because fresh Secure Connections key setup can
  legitimately exceed it and otherwise reports security error 9.
- NCS 3.4 RAS/Channel Sounding state is not reset reliably by a host-only
  `bt_disable()`/`bt_enable()` cycle. If an authenticated CS link dies with
  supervision timeout (`reason 8`), SS1 waits until no other client is active,
  then cold-reboots directly from the disconnect callback, matching Nordic's
  RAS sample. The bond survives in NVS, so SS2 reconnects automatically. Do not send CS disable or
  remove-config HCI commands from a `disconnected()` callback.

There is no pairing button or manual pairing step.

---

## Build / config gotchas (already hit - don't relearn)

- **Do NOT set `CONFIG_NRF_SECURITY=y` directly** - it has no prompt and errors
  out. It is selected indirectly. Instead enable the PSA features:
  `CONFIG_NRF_OBERON=y`, `CONFIG_PSA_WANT_ALG_ECDSA=y`,
  `CONFIG_PSA_WANT_ALG_SHA_256=y`, `CONFIG_PSA_WANT_ECC_SECP_R1_256=y`, plus
  `CONFIG_PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY=y` (SS1, verify) or
  `CONFIG_PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT=y` +
  `..._KEY_PAIR_BASIC=y` (SS2, sign).
- If a `PSA_WANT_*` symbol errors, copy the exact enabler from
  `C:\ncs\v3.4.0\nrf\samples\crypto\` (the ECDSA sample) - symbol names drift
  between SDK versions.
- MTU is bumped so the 204-byte response fits one write:
  `CONFIG_BT_L2CAP_TX_MTU=247`, `CONFIG_BT_BUF_ACL_TX_SIZE=251`,
  `CONFIG_BT_BUF_ACL_RX_SIZE=251`. Keep these on both boards. SS1 reserves 12
  ATT/L2CAP TX buffers and 6 ACL TX buffers so teardown of a RAS link cannot
  starve the next connection's MTU response. SS2 waits 500 ms after L2
  security before beginning MTU exchange and authentication discovery.
- SS2 uses NCS helper libs: `CONFIG_BT_SCAN=y`, `CONFIG_BT_GATT_DM=y`,
  `CONFIG_BT_GATT_CLIENT=y`, plus `CONFIG_BT_CENTRAL=y`.
- Two DKs attached: flash by J-Link serial (`west flash --dev-id <serial>` or
  the extension's device dropdown) so SS1 and SS2 never get crossed.

---

## Known gaps / TODO (priority order)

1. **Revocation - not implemented.** The cert carries `client_id` and
   `not_after` but SS1 enforces neither. Today the only way to cut off a
   leaked key is to rotate the CA and reflash all sensors. Design target:
   an on-device allowlist/denylist of `client_id`s, or short-lived certs
   with an enforced clock. `client_id` was added specifically so this can key
   off it - keep ids unique per device.
2. **`not_after` (expiry) unenforced** - needs a trusted time source on SS1.
3. **MITM** - Just Works pairing is MITM-able at the link layer; the cert
   auth is not yet bound to the LE link. Acceptable for the internship demo;
   production hardening = LESC OOB or bind cert-auth to the link key.
4. **`CONFIG_BT_MAX_CONN=3`** caps *simultaneous* connections (not total
   approved devices). Raise if more than 3 must connect at once (RAM permit).
5. SS2's BLE-central plumbing (`bt_gatt_dm` discovery + `bt_gatt_subscribe`
   wiring) was written to the NCS `central_uart` sample idioms but not
   independently compiled - verify handle extraction against that sample if
   discovery/subscribe misbehaves.

## Conventions

- Zephyr/NCS C style: tabs, `static` file-scope functions, `printk` logging,
  device-tree aliases for GPIO. Match the existing structure in `main.c`.
- No em dashes in generated text/docs (author preference).
- **Never re-run `enroll.py init-ca`** on a working setup - it would mint a
  new CA and invalidate every enrolled device and SS1's baked-in key. Adding
  devices uses `add` only.
- The CA private key (`ca/root_ca_key.pem`) is the trust root: keep it off
  every device, backed up. SS1 holds only the CA public key.

## Safe change checklist

- Touching cert/nonce/signature layout? Update SS1 verify, SS2 sign, and
  enroll.py together, and re-run enroll's issue->verify path to confirm.
- Adding a sensitive characteristic to SS1? Gate it on
  `sentry_auth_is_authenticated(conn)` in the read/write callbacks AND skip
  unauthenticated conns in its notify loop - the cert gate is the sole
  security boundary now that the pairing button is gone.
- Changing PSA config? Confirm against the NCS crypto sample, not memory.
