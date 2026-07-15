# SS1 MCUboot production signing key

`sysbuild.conf` expects `keys/ss1_mcuboot_prod_key.pem`. The private key is
deliberately ignored by Git and must never be committed. A production key was
generated locally on the development machine on 2026-07-15.
The matching public key is committed as `ss1_mcuboot_prod_pub.pem` so release
verification and fleet provenance do not depend on access to the private key.

Back it up in an offline password manager/HSM before flashing production
devices. Losing it means those devices can never accept another signed update;
leaking it means an attacker can sign firmware they control.

For a new development fleet only (never to replace a deployed production key):

```powershell
C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe `
  C:\ncs\v3.4.0\bootloader\mcuboot\scripts\imgtool.py keygen `
  -k keys\ss1_mcuboot_prod_key.pem -t ecdsa-p256
```

The first image using this key must be installed by wired full-chip flashing,
because the old MCUboot currently on a device trusts Nordic's development key.
After that bootstrap, BLE OTA accepts only images signed by this key.
