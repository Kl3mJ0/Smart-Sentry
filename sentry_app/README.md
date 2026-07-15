# Smart Sentry — native dashboard app

Native Qt Quick (PySide6/QML) kiosk app for the Raspberry Pi that connects to
SS1 ("Joseph_BLE"), completes the certificate handshake as client 1002, and
shows live temperature/humidity with LED control. UI is a 1:1 port of the
approved Claude design (`../Smart Sentry Dashboard (offline).html`), scaled
uniformly to any window/screen size.

KEEP `creds.py` PRIVATE — it embeds client 1002's private key.

## Files

- `main.py` — entry point (qasync event loop, driver selection, fonts)
- `qml/Main.qml` — the entire dashboard UI
- `backend.py` — Qt object bridging driver state to QML
- `fake_driver.py` — simulated SS1 (default on Windows; demo: tap the status
  pill to cycle states, Reconnect plays the full sequence)
- `ble_driver.py` — real SS1 driver (default on Linux; auto-reconnects forever)
- `creds.py` — client 1002 key + certificate (from PI_CLIENT_HANDOFF.md)

## Run on Windows (development, simulated data)

```powershell
pip install -r requirements.txt
python main.py                 # fake driver, windowed
python main.py --fullscreen    # kiosk preview
```

## Deploy to the Pi

```bash
# copy the sentry_app/ folder over, then:
sudo apt update && sudo apt install -y bluez fonts-jetbrains-mono
pip3 install -r requirements-pi.txt
# if the PySide6 pip wheel is unavailable for your OS image, use apt instead:
#   sudo apt install python3-pyside6.qtcore python3-pyside6.qtgui \
#                    python3-pyside6.qtqml python3-pyside6.qtquick
python3 main.py                      # real BLE, windowed
python3 main.py --fullscreen         # touchscreen kiosk
python3 main.py --driver fake        # UI demo without SS1 powered
```

First Pi run checklist:

1. **LED UUID**: `ble_driver.py` assumes the LED characteristic is
   `7e8f0002-1111-2222-3333-123456789abc` (suffix inferred from the auth
   service pattern) and a 1-byte 0/1 write. If the toggle no-ops, dump SS1's
   GATT table and fix `LED_UUID` / the payload.
2. **Stale bond errors** (`AuthenticationFailed`): `bluetoothctl remove
   FA:11:FF:FB:B2:14`, then relaunch (SS1 never bonds; see handoff §5).
3. Run as a normal user; if BLE permission errors:
   `sudo usermod -aG bluetooth $USER` and re-login.

## Autostart on boot (kiosk)

`~/.config/systemd/user/sentry-dashboard.service`:

```ini
[Unit]
Description=Smart Sentry dashboard
After=graphical-session.target bluetooth.target

[Service]
ExecStart=/usr/bin/python3 %h/sentry_app/main.py --driver ble --fullscreen
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
- TypeError warnings at app exit come from QML bindings outliving the
  backend during teardown; harmless.

## Debug

```bash
python main.py --screenshot frame.png   # saves a frame after 3 s and exits
```

## sentry_agent/ - headless fleet and secure updater service

The dashboard connecting to BLE directly (`--driver ble`) is fine for one
device and one process, but doesn't scale to a fleet: there's only one global
connection/sensor set, and an independent OTA process would fight the dashboard for the
adapter. `sentry_agent/` is a separate headless service that owns BLE, the
device inventory and OTA exclusively; the dashboard becomes a thin client
of it via `--driver agent` (see `agent_driver.py`). Not switched on by
default yet - `--driver ble` still works unchanged.

```bash
pip3 install -r requirements-pi.txt   # now also installs smpclient
cd sentry_agent && python3 agent_main.py     # run in foreground to try it
# in another terminal/window:
python3 main.py --driver agent
```

To run it as a systemd service instead (survives UI restarts, starts at
boot regardless of login):

```bash
sudo cp systemd/sentry-agent.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now sentry-agent
```

When `sentry-agent` is enabled, the dashboard must use the agent rather than
opening BLE itself. Change the dashboard service's `ExecStart` to:

```ini
ExecStart=/usr/bin/python3 %h/sentry_app/main.py --driver agent --fullscreen
```

Then run `systemctl --user daemon-reload && systemctl --user restart
sentry-dashboard`. Do not run `--driver ble` at the same time as
`sentry-agent`; the agent is the sole owner of the Pi's Bluetooth adapter.

On startup the agent scans `firmware/**/manifest.json`, selects the newest
local signed release and queues every connected older SS1 once. The dashboard
Firmware / Queue button shows check state, target version, current device and
upload/health/confirmation progress. Jobs are persistent and sequential.

Firmware on this branch advertises a permanent factory ID, certificate-gates
MCUmgr, uses a local production MCUboot signing key, prevents downgrades and
leaves every trial unconfirmed until the Pi observes 15 seconds of healthy,
authenticated sensor data. Failed trials are rebooted into MCUboot rollback.
Cloud release download is the remaining delivery-provider step; local files
are deliberately used for hardware acceptance testing first.

Detailed security and device-identity design:
[`../docs/ota-auth-and-device-id.md`](../docs/ota-auth-and-device-id.md).
