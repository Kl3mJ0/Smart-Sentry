# Smart Sentry — native dashboard app

Native Qt Quick (PySide6/QML) kiosk app for the Raspberry Pi that connects to
SS1 ("Joseph_BLE"), completes the certificate handshake as client 1002, and
shows live temperature/humidity with LED control. UI is a 1:1 port of the
approved Claude design (`../Smart Sentry Dashboard (offline).html`), scaled
uniformly to any window/screen size.

KEEP `creds.py` PRIVATE — it embeds a client private key. Copy
`creds.example.py` to `creds.py`, then insert credentials provisioned for that
specific installation. The populated file is ignored by Git.

## Files

- `main.py` — entry point (Qt main loop + driver worker thread, fonts)
- `qml/Main.qml` — the entire dashboard UI
- `backend.py` — Qt object bridging driver state to QML
- `fake_driver.py` — simulated SS1 (default on Windows; demo: tap the status
  pill to cycle states, Reconnect plays the full sequence)
- `ble_driver.py` — real SS1 driver (default on Linux; auto-reconnects forever)
- `creds.example.py` — safe credential-file template
- `creds.py` — local provisioned key + certificate; never commit this file

## Run on Windows (development, simulated data)

```powershell
pip install -r requirements.txt
python main.py                 # fake driver, windowed
python main.py --fullscreen    # kiosk preview
```

## Deploy to the Pi

Raspberry Pi OS's Python is externally managed (PEP 668), so Qt comes from
apt and the BLE extras go in a venv that can see the apt packages:

```bash
# copy the sentry_app/ folder over, then:
sudo apt update
sudo apt install -y bluez fonts-jetbrains-mono qt6-wayland \
                    python3-pyside6.qtcore python3-pyside6.qtgui \
                    python3-pyside6.qtqml python3-pyside6.qtquick \
                    python3-cryptography
python3 -m venv --system-site-packages ~/sentry-venv
~/sentry-venv/bin/pip install -r ~/sentry_app/requirements-pi.txt

~/sentry-venv/bin/python ~/sentry_app/main.py                # real BLE, windowed
~/sentry-venv/bin/python ~/sentry_app/main.py --fullscreen   # touchscreen kiosk
~/sentry-venv/bin/python ~/sentry_app/main.py --driver fake  # UI demo, no SS1
```

First Pi run checklist:

1. **LED UUID**: `ble_driver.py` assumes the LED characteristic is
   `7e8f0002-1111-2222-3333-123456789abc` (suffix inferred from the auth
   service pattern) and a 1-byte 0/1 write. If the toggle no-ops, dump SS1's
   GATT table and fix `LED_UUID` / the payload.
2. **Mismatched stale bond errors** (`AuthenticationFailed`): `bluetoothctl
   remove FA:11:FF:FB:B2:14`, then relaunch (see handoff §5).
3. Run as a normal user; if BLE permission errors:
   `sudo usermod -aG bluetooth $USER` and re-login.

## Autostart on boot (kiosk)

`~/.config/systemd/user/sentry-dashboard.service`:

```ini
[Unit]
Description=Smart Sentry dashboard
After=graphical-session.target bluetooth.target

[Service]
ExecStart=%h/sentry-venv/bin/python %h/sentry_app/main.py --driver ble --fullscreen
Restart=on-failure
RestartSec=3

[Install]
WantedBy=graphical-session.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now sentry-dashboard
```

## Notes

- The BLE driver loops scan → connect/pair → cert auth → subscribe and shows
  the RECONNECTING overlay on any drop; killing/restarting is always safe
  (SS1-side ghost-connection recovery landed 2026-07-10).
- Signal bars are 4/0 (connected/not) for now — mapping live RSSI to 1–4
  bars is a TODO in `ble_driver.py`.
- On the Windows dev box the first frame can take a few seconds (shader
  compile) before animations start; harmless.
- Do NOT reintroduce qasync: it breaks on newer Qt/Python (timer assertion
  crashes on Pi OS with Python 3.13). The driver thread + backend.post()
  design exists to avoid it.
- TypeError warnings at app exit come from QML bindings outliving the
  backend during teardown; harmless.

## Debug

```bash
python main.py --screenshot frame.png   # saves a frame after 3 s and exits
```
