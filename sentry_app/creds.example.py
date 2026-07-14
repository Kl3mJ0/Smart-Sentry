"""Template for local Sentry client credentials.

Copy this file to ``creds.py`` and replace both placeholders with credentials
created for this installation. Never commit the populated ``creds.py`` file.
"""

CLIENT_KEY_PEM = b"""-----BEGIN PRIVATE KEY-----
REPLACE_WITH_A_PROVISIONED_PRIVATE_KEY
-----END PRIVATE KEY-----"""

CLIENT_CERT = bytes.fromhex(
    "REPLACE_WITH_THE_PROVISIONED_CERTIFICATE_HEX"
)

CERT_LEN = 140
SIG_LEN = 64
NONCE_LEN = 32
