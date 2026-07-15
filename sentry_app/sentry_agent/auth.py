"""Certificate authentication shared by sensor and MCUmgr BLE sessions."""
from pathlib import Path
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from creds import CLIENT_CERT, CLIENT_KEY_PEM, CERT_LEN, NONCE_LEN, SIG_LEN  # noqa: E402

AUTH_CHALLENGE_UUID = "7e8f00a1-1111-2222-3333-123456789abc"
AUTH_RESPONSE_UUID = "7e8f00a2-1111-2222-3333-123456789abc"


def sign_nonce(nonce: bytes) -> bytes:
    key = serialization.load_pem_private_key(CLIENT_KEY_PEM, password=None)
    der = key.sign(nonce, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


async def authenticate(client) -> None:
    """Pair/encrypt, then prove possession of the provisioned Pi key."""
    try:
        await client.pair()
    except Exception as exc:
        print(f"[auth] pair() note: {exc}")
    nonce = bytes(await client.read_gatt_char(AUTH_CHALLENGE_UUID))
    if len(nonce) != NONCE_LEN:
        raise ValueError(f"unexpected nonce length {len(nonce)}")
    response = CLIENT_CERT + sign_nonce(nonce)
    assert len(response) == CERT_LEN + SIG_LEN
    await client.write_gatt_char(AUTH_RESPONSE_UUID, response, response=True)
