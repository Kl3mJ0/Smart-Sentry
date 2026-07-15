# SS1 firmware 1.0.5 — production-key bootstrap

- `ss1_1.0.5_bootstrap.hex` contains MCUboot plus the signed application.
  Flash this once by wire to move an existing board from the Nordic
  development signing key to the Smart Sentry production public key.
- `blinky.signed.bin` is the BLE OTA payload for all later devices that
  already run the production-key MCUboot.

Recommended first install from the NCS 3.4.0 environment:

```powershell
west flash -d build
```

Or load `ss1_1.0.5_bootstrap.hex` in Nordic Programmer. This wired bootstrap
also replaces MCUboot; do not try to send the bootstrap HEX through MCUmgr.

SHA-256:

- OTA BIN: `b3f2a53f80d9bfce1d06e13638450389ba75b0aefdfb059277120a8a5ab69654`
- bootstrap HEX: `09507f7490f9bc455bae5e1ef4b8c691c93e7d7f0c0a010e9382336b14ca3777`
