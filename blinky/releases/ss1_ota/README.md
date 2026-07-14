# SS1 BLE OTA test

This release pair performs a real firmware update:

- `ss1_bootstrap_1.0.1.hex`: one-time wired image containing MCUboot and SS1 1.0.1.
- `ss1_update_1.0.2.zip`: Android nRF Connect Device Manager update package.
- `ss1_update_1.0.2.bin`: signed single-image payload for clients that request a BIN file.
- `ss1_update_1.0.2_manifest.json`: generated package metadata.

## What actually happens

The phone uploads the signed 1.0.2 image in chunks over the Bluetooth SMP
service. SS1 writes those chunks to its secondary flash slot. The phone marks
the image for a test boot and resets SS1. MCUboot verifies the ED25519
signature and boots 1.0.2. After SS1 has initialized its hardware, Bluetooth,
settings and advertising, the application confirms the image so it remains
installed after later resets.

## One-time bootstrap

1. Turn SS2 off so it cannot connect to SS1 during the update test.
2. Open nRF Connect for Desktop Programmer and select the SS1 DK.
3. Erase or recover SS1. This intentionally clears the old non-MCUboot image,
   bonds and settings.
4. Add `ss1_bootstrap_1.0.1.hex` and write it to SS1.
5. Reset SS1 and check the serial terminal for:

       MAIN STARTED - SS1 firmware 1.0.1

USB can remain connected for power and serial logging. Do not flash through
the debugger again during the OTA part of the test.

## Real BLE update

1. Install Nordic's **nRF Connect Device Manager** mobile app.
2. Scan for and connect to `Joseph_BLE`.
3. Open Firmware/DFU and select `ss1_update_1.0.2.zip` on Android. If an iOS
   client requests a single image instead of the ZIP, select
   `ss1_update_1.0.2.bin`.
4. Select **Test and Confirm** and start the update. Keep the phone close and
   leave SS2 powered off until SS1 advertises again.
5. The upload, test flag, reset and confirmation are management commands sent
   by the app; no debugger is involved.
6. Check the serial terminal for:

       MAIN STARTED - SS1 firmware 1.0.2
       OTA image confirmed; firmware 1.0.2 is now permanent

7. Power-cycle SS1 once more. It must still report 1.0.2.
8. Turn SS2 back on and verify the normal authenticated Channel Sounding link.

If the phone shows an old GATT service list, disconnect, clear its cached bond
for `Joseph_BLE`, and scan again.

## Future releases

Change the values in `blinky/VERSION`, build the existing `blinky/build`
configuration, and publish the generated `blinky/build/dfu_application.zip`.
The semantic version is embedded in both the application log and MCUboot image
header.

## Worldwide delivery architecture

SS1 is BLE-only and cannot contact an internet server itself. Remote delivery
therefore needs a nearby internet-connected relay:

    Home/CI -> sign release -> HTTPS release store
                                  |
                                  v
                         workplace gateway/phone
                                  |
                               BLE SMP
                                  |
                                  v
                                 SS1

A workplace Raspberry Pi is the usual unattended gateway. It polls a signed
release manifest, downloads the package, finds the provisioned SS1 locally,
performs the same upload/test/reset/confirm sequence, and reports success.
The signing private key stays at home or in CI; the gateway only receives
signed release files.

## Test-only security warning

These artifacts use Nordic's public development MCUboot signing key and the
BLE management characteristic is deliberately open so the first phone test is
straightforward. The signature still rejects arbitrary corrupt/unsigned
images, but this is not production security. Before deploying SS1 devices:

1. Create and protect a private production signing key.
2. Add an authenticated OTA authorization/window for the SMP endpoint.
3. Add downgrade protection, staged rollout and per-device identity.
4. Publish packages and manifests over authenticated HTTPS.

## SHA-256

- Bootstrap HEX: `58cae471d4a79d2ed700420178bc3e733994abf7d6268469e080fa3db0ac2430`
- Update ZIP: `0fa55b98345c3bfc071bccb1b312c6de37093fc065fb54aefa7d57c7f6668ad0`
- Update BIN: `60215060209b0ff66157d231c1f76ab5700956c95599cb52cf9835193b3b0375`
