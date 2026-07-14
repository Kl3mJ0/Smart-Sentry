/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart Sentry certificate-based device pre-approval - implementation.
 *
 * GATT auth service:
 *   Challenge characteristic (READ, encrypted): 32-byte per-connection nonce.
 *   Response  characteristic (WRITE, encrypted): client writes
 *       [ cert (140) || ecdsa_sign(nonce) (64) ] = 204 bytes.
 *
 * On a valid response the connection is flagged authenticated. If a
 * connection has not authenticated within SENTRY_AUTH_TIMEOUT_MS it is
 * disconnected.
 */

#include "sentry_auth.h"
#include "cs_reflector.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>

#include <psa/crypto.h>

/* ---------------- Root CA public key ----------------
 *
 * Paste the output of `python3 provision.py firmware-header` here.
 * This is the ONLY trust anchor on the device. The CA *private* key never
 * touches the firmware.
 *
 * The 64 bytes below are a PLACEHOLDER - replace them or auth will reject
 * every client.
 */
static const uint8_t root_ca_pubkey[64] = {
	0xcb, 0x3b, 0x9a, 0x45, 0x19, 0x49, 0xb7, 0xc2,
	0x50, 0x51, 0x34, 0xa7, 0x37, 0x62, 0x70, 0x96,
	0x64, 0xb7, 0x9d, 0x97, 0x77, 0x65, 0x0d, 0xb3,
	0xb1, 0x8f, 0xc6, 0x2b, 0x91, 0x5b, 0x30, 0x75,
	0xdf, 0x38, 0xf4, 0xbc, 0x73, 0x49, 0xfe, 0x9b,
	0x43, 0x6d, 0xd9, 0x71, 0x0c, 0xe8, 0x26, 0x4c,
	0x8d, 0x4a, 0x80, 0x94, 0x07, 0x4a, 0x51, 0xc0,
	0x0c, 0x9c, 0xbc, 0x34, 0x98, 0x95, 0xb1, 0x21,
};

/* ---------------- Custom UUIDs ----------------
 * Auth service:   7e8f00a0-...
 * Challenge char: 7e8f00a1-...
 * Response char:  7e8f00a2-...
 */
#define BT_UUID_SENTRY_AUTH_SVC_VAL \
	BT_UUID_128_ENCODE(0x7e8f00a0, 0x1111, 0x2222, 0x3333, 0x123456789abc)
#define BT_UUID_SENTRY_AUTH_CHALLENGE_VAL \
	BT_UUID_128_ENCODE(0x7e8f00a1, 0x1111, 0x2222, 0x3333, 0x123456789abc)
#define BT_UUID_SENTRY_AUTH_RESPONSE_VAL \
	BT_UUID_128_ENCODE(0x7e8f00a2, 0x1111, 0x2222, 0x3333, 0x123456789abc)

static struct bt_uuid_128 auth_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_AUTH_SVC_VAL);
static struct bt_uuid_128 auth_challenge_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_AUTH_CHALLENGE_VAL);
static struct bt_uuid_128 auth_response_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_AUTH_RESPONSE_VAL);

/* ---------------- Per-connection auth state ---------------- */

struct auth_slot {
	struct bt_conn *conn;
	uint8_t nonce[SENTRY_NONCE_LEN];
	bool nonce_valid;
	bool authenticated;
	struct k_work_delayable timeout;
};

static struct auth_slot slots[CONFIG_BT_MAX_CONN];

static struct auth_slot *slot_for(struct bt_conn *conn)
{
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (slots[i].conn == conn) {
			return &slots[i];
		}
	}
	return NULL;
}

static struct auth_slot *slot_alloc(struct bt_conn *conn)
{
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (slots[i].conn == NULL) {
			slots[i].conn = conn;
			slots[i].nonce_valid = false;
			slots[i].authenticated = false;
			return &slots[i];
		}
	}
	return NULL;
}

/* ---------------- ECDSA-P256 verify via PSA ----------------
 *
 * Mirrors device_sim.py: verify(pub_xy, message, raw_r||s).
 * PSA wants the public key as an uncompressed point (0x04 || X || Y).
 */
static bool ecdsa_p256_verify(const uint8_t pub_xy[64],
			      const uint8_t *msg, size_t msg_len,
			      const uint8_t sig[SENTRY_SIG_LEN])
{
	psa_status_t status;
	psa_key_id_t key_id;
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	uint8_t point[65];

	point[0] = 0x04;
	memcpy(&point[1], pub_xy, 64);

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr,
			 PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	status = psa_import_key(&attr, point, sizeof(point), &key_id);
	if (status != PSA_SUCCESS) {
		printk("psa_import_key failed: %d\n", (int)status);
		return false;
	}

	status = psa_verify_message(key_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
				    msg, msg_len, sig, SENTRY_SIG_LEN);

	psa_destroy_key(key_id);

	return status == PSA_SUCCESS;
}

/* ---------------- Response verification ---------------- */

static bool verify_response(struct auth_slot *slot,
			    const uint8_t *resp, uint16_t len)
{
	const uint8_t *cert;
	const uint8_t *nonce_sig;
	const uint8_t *body;
	const uint8_t *ca_sig;
	const uint8_t *client_pub;

	if (len != SENTRY_RESPONSE_LEN) {
		printk("Auth: bad response length %u (want %u)\n",
		       len, SENTRY_RESPONSE_LEN);
		return false;
	}

	if (!slot->nonce_valid) {
		printk("Auth: no valid nonce for this connection\n");
		return false;
	}

	cert = resp;
	nonce_sig = resp + SENTRY_CERT_TOTAL_LEN;
	body = cert;
	ca_sig = cert + SENTRY_CERT_BODY_LEN;
	client_pub = body + SENTRY_CERT_OFF_PUBKEY;

	if (body[0] != SENTRY_CERT_VERSION) {
		printk("Auth: unsupported cert version %u\n", body[0]);
		return false;
	}

	/* 1. Is this cert genuinely issued by our CA? */
	if (!ecdsa_p256_verify(root_ca_pubkey, body, SENTRY_CERT_BODY_LEN,
			       ca_sig)) {
		printk("Auth: CA signature invalid - cert not ours\n");
		return false;
	}

	/* 2. Does the peer actually hold the cert's private key? */
	if (!ecdsa_p256_verify(client_pub, slot->nonce, SENTRY_NONCE_LEN,
			       nonce_sig)) {
		printk("Auth: nonce signature invalid - no private key\n");
		return false;
	}

	/*
	 * TODO (revocation / expiry):
	 *   - not_after at body + SENTRY_CERT_OFF_NOTAFTER, needs a trusted
	 *     time source to enforce.
	 *   - client_id at body + SENTRY_CERT_OFF_ID: check against an on-device
	 *     allowlist/denylist here to support revocation.
	 */
	return true;
}

/* ---------------- Auth timeout ---------------- */

static void auth_timeout(struct k_work *work)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	struct auth_slot *slot = CONTAINER_OF(dw, struct auth_slot, timeout);

	if (slot->conn && !slot->authenticated) {
		printk("Auth: timeout, disconnecting unauthenticated peer\n");
		bt_conn_disconnect(slot->conn,
				   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

/* ---------------- GATT callbacks ---------------- */

static ssize_t read_challenge_cb(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 void *buf, uint16_t len, uint16_t offset)
{
	struct auth_slot *slot = slot_for(conn);

	if (!slot || !slot->nonce_valid) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 slot->nonce, SENTRY_NONCE_LEN);
}

static ssize_t write_response_cb(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 const void *buf, uint16_t len,
				 uint16_t offset, uint8_t flags)
{
	struct auth_slot *slot = slot_for(conn);

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (!slot) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	if (verify_response(slot, buf, len)) {
		slot->authenticated = true;
		slot->nonce_valid = false; /* one-shot: consume the nonce */
		k_work_cancel_delayable(&slot->timeout);
		printk("Auth: connection AUTHENTICATED\n");
		/* Do not start controller channel-sounding setup until the GATT
		 * certificate exchange has proved this peer. This also keeps CS HCI
		 * traffic out of the reconnect's critical ATT setup window. */
		cs_reflector_conn_ready(conn);
	} else {
		printk("Auth: verification FAILED, disconnecting\n");
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(sentry_auth_svc,
	BT_GATT_PRIMARY_SERVICE(&auth_svc_uuid),

	BT_GATT_CHARACTERISTIC(&auth_challenge_uuid.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_challenge_cb, NULL, NULL),
	BT_GATT_CUD("Auth Challenge (nonce)", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(&auth_response_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE_ENCRYPT,
			       NULL, write_response_cb, NULL),
	BT_GATT_CUD("Auth Response (cert + nonce sig)", BT_GATT_PERM_READ),
);

/* ---------------- Public API ---------------- */

int sentry_auth_init(void)
{
	psa_status_t status = psa_crypto_init();

	if (status != PSA_SUCCESS) {
		printk("psa_crypto_init failed: %d\n", (int)status);
		return -1;
	}

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		k_work_init_delayable(&slots[i].timeout, auth_timeout);
	}

	printk("Sentry auth ready (cert-based pre-approval)\n");
	return 0;
}

void sentry_auth_conn_added(struct bt_conn *conn)
{
	struct auth_slot *slot = slot_alloc(conn);
	psa_status_t status;

	if (!slot) {
		printk("Auth: no free auth slot\n");
		return;
	}

	status = psa_generate_random(slot->nonce, SENTRY_NONCE_LEN);
	if (status != PSA_SUCCESS) {
		printk("Auth: nonce generation failed: %d\n", (int)status);
		slot->nonce_valid = false;
		bt_conn_disconnect(conn, BT_HCI_ERR_UNSPECIFIED);
		return;
	}

	slot->nonce_valid = true;
	slot->authenticated = false;

	k_work_reschedule(&slot->timeout, K_MSEC(SENTRY_AUTH_TIMEOUT_MS));
	printk("Auth: fresh nonce issued, %d ms to authenticate\n",
	       SENTRY_AUTH_TIMEOUT_MS);
}

void sentry_auth_conn_removed(struct bt_conn *conn)
{
	struct auth_slot *slot = slot_for(conn);

	if (!slot) {
		return;
	}

	k_work_cancel_delayable(&slot->timeout);
	memset(slot->nonce, 0, SENTRY_NONCE_LEN);
	slot->nonce_valid = false;
	slot->authenticated = false;
	slot->conn = NULL;
}

bool sentry_auth_is_authenticated(struct bt_conn *conn)
{
	struct auth_slot *slot = slot_for(conn);

	return slot && slot->authenticated;
}
