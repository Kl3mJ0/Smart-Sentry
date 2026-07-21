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
- `agent_process.py` — starts the fleet agent in the background when needed
- `agent_driver.py` — dashboard IPC client for the fleet agent
- `ble_driver.py` — legacy single-SS1 diagnostic driver (`--driver ble` only)
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
python3 main.py                      # integrated BLE fleet + OTA, windowed
python3 main.py --fullscreen         # touchscreen kiosk
python3 main.py --driver fake        # UI demo without SS1 powered
python3 main.py --driver ble         # legacy direct-BLE diagnostics only
```

On Linux the dashboard defaults to `--driver agent`. It connects to an
existing sentry-agent service when present; otherwise it starts the bundled
agent as a detached background process. Do not separately run
`sentry_agent/agent_main.py` in a terminal.

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
WorkingDirectory=%h/Smart-Sentry-OTA/sentry_app
ExecStart=%h/sentry-venv/bin/python3 main.py --fullscreen
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

## sentry_agent/ - integrated fleet and secure updater

The dashboard connecting to BLE directly (`--driver ble`) is fine for one
device and one process, but doesn't scale to a fleet: there's only one global
connection/sensor set, and an independent OTA process would fight the dashboard for the
adapter. `sentry_agent/` is the headless process that owns BLE, device
inventory and OTA exclusively; the dashboard is its thin client via
`agent_driver.py`. This is now the Linux default, and the dashboard starts it
automatically when no service is already running.

```bash
pip3 install -r requirements-pi.txt   # now also installs smpclient
python3 main.py
```

For production it can instead run as a systemd service, so BLE monitoring and
an in-progress OTA survive a full UI restart. The dashboard detects and reuses
the service automatically:

```bash
sudo cp systemd/sentry-agent.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now sentry-agent
```

When `sentry-agent` is enabled, the dashboard must use the agent rather than
opening BLE itself. The Linux default already does this. Add
`--external-agent` if you want the UI to report a stopped service instead of
launching its bundled fallback:

```ini
ExecStart=%h/sentry-venv/bin/python3 %h/Smart-Sentry-OTA/sentry_app/main.py --driver agent --external-agent --fullscreen
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

The selected SS1's sensor configuration is available from **SENSOR MODE** on
the dashboard. It controls Debug, Normal I2C and External Auto modes plus the
0–30 second sampling interval. See [the sensor mode protocol](../docs/sensor-modes.md).
Cloud release download is the remaining delivery-provider step; local files
are deliberately used for hardware acceptance testing first.

Detailed security and device-identity design:
[`../docs/ota-auth-and-device-id.md`](../docs/ota-auth-and-device-id.md).
