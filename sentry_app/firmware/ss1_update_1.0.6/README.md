# SS1 firmware 1.0.6 — automated OTA acceptance payload

This production-key-signed payload exists so the complete automation can be
tested after the one-time wired 1.0.5 bootstrap:

1. wire-flash `../ss1_update_1.0.5/ss1_1.0.5_bootstrap.hex`;
2. deploy/start `sentry-agent` with both local releases present;
3. observe the dashboard discover firmware 1.0.5 and automatically queue 1.0.6;
4. observe upload → trial boot → 15-second authenticated health check → Pi
   confirmation → `confirmed` queue state and firmware revision 1.0.6.

This is a real signed/versioned OTA, not a simulated queue item.
