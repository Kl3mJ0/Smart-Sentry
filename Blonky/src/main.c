/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SS2 - certified BLE central for the Smart Sentry rig.
 *
 * Connects to SS1 (peripheral "Joseph_BLE"), completes the certificate
 * challenge-response, then subscribes to temperature + humidity and prints
 * them to its own serial console.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <bluetooth/scan.h>
#include <bluetooth/gatt_dm.h>

#include <psa/crypto.h>

#include "sentry_client_creds.h"   /* client_cert[140], client_privkey[32] */
#include "cs_initiator.h"

/* ---- must match SS1 sentry_auth.h ---- */
#define NONCE_LEN        32
#define SIG_LEN          64
#define CERT_TOTAL_LEN   140
#define RESPONSE_LEN     (CERT_TOTAL_LEN + SIG_LEN)   /* 204 */

#define TARGET_NAME "Joseph_BLE"
#define SCAN_RETRY_DELAY_MS 500
#define SECURITY_RETRY_DELAY_MS 2000
#define SECURITY_START_DELAY_MS 500
#define SECURITY_SETUP_TIMEOUT_MS 35000
#define GATT_START_DELAY_MS 500
#define GATT_SETUP_TIMEOUT_MS 8000

/* ---- UUIDs (mirror SS1) ---- */
#define UUID_AUTH_SVC \
	BT_UUID_128_ENCODE(0x7e8f00a0, 0x1111, 0x2222, 0x3333, 0x123456789abc)

#define UUID_AUTH_CHALLENGE \
	BT_UUID_128_ENCODE(0x7e8f00a1, 0x1111, 0x2222, 0x3333, 0x123456789abc)

#define UUID_AUTH_RESPONSE \
	BT_UUID_128_ENCODE(0x7e8f00a2, 0x1111, 0x2222, 0x3333, 0x123456789abc)

static const struct bt_uuid_128 auth_svc_uuid  = BT_UUID_INIT_128(UUID_AUTH_SVC);
static const struct bt_uuid_128 challenge_uuid = BT_UUID_INIT_128(UUID_AUTH_CHALLENGE);
static const struct bt_uuid_128 response_uuid  = BT_UUID_INIT_128(UUID_AUTH_RESPONSE);

static const struct bt_uuid_16 ess_uuid  = BT_UUID_INIT_16(0x181A);
static const struct bt_uuid_16 temp_uuid = BT_UUID_INIT_16(0x2A6E);
static const struct bt_uuid_16 hum_uuid  = BT_UUID_INIT_16(0x2A6F);

/* ---- state ---- */
static struct bt_conn *default_conn;
static bool security_retry_backoff;
static bool gatt_setup_complete;
static bool bond_cleanup_pending;
static bt_addr_le_t bond_cleanup_addr;
static uint8_t bond_cleanup_attempts;

static uint16_t challenge_handle;
static uint16_t response_handle;

static struct bt_gatt_read_params  read_params;
static struct bt_gatt_write_params write_params;
static struct bt_gatt_exchange_params mtu_params;

static void gatt_setup_timeout_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(gatt_setup_timeout_work,
			       gatt_setup_timeout_fn);
static void gatt_start_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(gatt_start_work, gatt_start_fn);

static struct bt_gatt_subscribe_params temp_sub;
static struct bt_gatt_subscribe_params hum_sub;

static uint8_t response_buf[RESPONSE_LEN];

static psa_key_id_t client_key_id;

/* ---------------- GATT-DM handle helpers ---------------- */

static uint16_t dm_char_value_handle(const struct bt_gatt_dm_attr *chr)
{
	struct bt_gatt_chrc *chrc_val;

	if (chr == NULL) {
		return 0;
	}

	chrc_val = bt_gatt_dm_attr_chrc_val(chr);
	if (chrc_val == NULL) {
		return 0;
	}

	return chrc_val->value_handle;
}

static uint16_t dm_attr_handle(const struct bt_gatt_dm_attr *attr)
{
	if (attr == NULL) {
		return 0;
	}

	return attr->handle;
}

/* ---------------- crypto ---------------- */

static int import_client_key(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t s;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	s = psa_import_key(&attr, client_privkey, sizeof(client_privkey),
			   &client_key_id);
	if (s != PSA_SUCCESS) {
		printk("client key import failed: %d\n", (int)s);
		return -1;
	}

	printk("Client private key imported\n");
	return 0;
}

static int sign_nonce(const uint8_t *nonce, size_t nonce_len,
		      uint8_t out_sig[SIG_LEN])
{
	size_t sig_len = 0;
	psa_status_t s;

	s = psa_sign_message(client_key_id,
			     PSA_ALG_ECDSA(PSA_ALG_SHA_256),
			     nonce, nonce_len,
			     out_sig, SIG_LEN, &sig_len);

	if (s != PSA_SUCCESS || sig_len != SIG_LEN) {
		printk("sign failed: %d (len %u)\n", (int)s, (unsigned int)sig_len);
		return -1;
	}

	return 0;
}

/* ---------------- notifications ---------------- */

static uint8_t on_temp(struct bt_conn *conn,
		       struct bt_gatt_subscribe_params *params,
		       const void *data,
		       uint16_t len)
{
	ARG_UNUSED(conn);

	if (!data) {
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	if (len >= 2) {
		int16_t v = (int16_t)sys_get_le16(data);
		printk("SS1 temperature: %d.%02d C\n", v / 100, abs(v % 100));
	}

	return BT_GATT_ITER_CONTINUE;
}

static uint8_t on_hum(struct bt_conn *conn,
		      struct bt_gatt_subscribe_params *params,
		      const void *data,
		      uint16_t len)
{
	ARG_UNUSED(conn);

	if (!data) {
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	if (len >= 2) {
		uint16_t v = sys_get_le16(data);
		printk("SS1 humidity:    %u.%02u %%RH\n", v / 100, v % 100);
	}

	return BT_GATT_ITER_CONTINUE;
}

/* ---------------- discovery: sensor service after auth ---------------- */

static void ess_dm_completed(struct bt_gatt_dm *dm, void *ctx)
{
	const struct bt_gatt_dm_attr *chr;
	const struct bt_gatt_dm_attr *ccc;
	int err;

	ARG_UNUSED(ctx);

	/* Temperature */
	chr = bt_gatt_dm_char_by_uuid(dm, &temp_uuid.uuid);
	ccc = chr ? bt_gatt_dm_desc_by_uuid(dm, chr, BT_UUID_GATT_CCC) : NULL;

	if (chr && ccc) {
		memset(&temp_sub, 0, sizeof(temp_sub));

		temp_sub.notify = on_temp;
		temp_sub.value = BT_GATT_CCC_NOTIFY;
		temp_sub.value_handle = dm_char_value_handle(chr);
		temp_sub.ccc_handle = dm_attr_handle(ccc);
		atomic_set_bit(temp_sub.flags,
			       BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

		if (temp_sub.value_handle && temp_sub.ccc_handle) {
			err = bt_gatt_subscribe(default_conn, &temp_sub);
			if (err && err != -EALREADY) {
				printk("Temperature subscribe failed: %d\n", err);
			} else {
				printk("Subscribed to temperature notifications\n");
			}
		} else {
			printk("Temperature handles invalid\n");
		}
	} else {
		printk("Temperature characteristic/CCC not found\n");
	}

	/* Humidity */
	chr = bt_gatt_dm_char_by_uuid(dm, &hum_uuid.uuid);
	ccc = chr ? bt_gatt_dm_desc_by_uuid(dm, chr, BT_UUID_GATT_CCC) : NULL;

	if (chr && ccc) {
		memset(&hum_sub, 0, sizeof(hum_sub));

		hum_sub.notify = on_hum;
		hum_sub.value = BT_GATT_CCC_NOTIFY;
		hum_sub.value_handle = dm_char_value_handle(chr);
		hum_sub.ccc_handle = dm_attr_handle(ccc);
		atomic_set_bit(hum_sub.flags,
			       BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

		if (hum_sub.value_handle && hum_sub.ccc_handle) {
			err = bt_gatt_subscribe(default_conn, &hum_sub);
			if (err && err != -EALREADY) {
				printk("Humidity subscribe failed: %d\n", err);
			} else {
				printk("Subscribed to humidity notifications\n");
			}
		} else {
			printk("Humidity handles invalid\n");
		}
	} else {
		printk("Humidity characteristic/CCC not found\n");
	}

	printk("Sensor discovery complete\n");
	bt_gatt_dm_data_release(dm);

	cs_initiator_start(default_conn);
}

static void dm_error(struct bt_conn *conn, int err, void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);

	printk("Discovery error: %d\n", err);
}

static struct bt_gatt_dm_cb ess_dm_cb = {
	.completed = ess_dm_completed,
	.error_found = dm_error,
};

static void discover_sensors(void)
{
	int err;

	err = bt_gatt_dm_start(default_conn, &ess_uuid.uuid, &ess_dm_cb, NULL);
	if (err) {
		printk("ESS discovery start failed: %d\n", err);
	}
}

/* ---------------- auth: write response ---------------- */

static void response_written(struct bt_conn *conn,
			     uint8_t err,
			     struct bt_gatt_write_params *params)
{
	ARG_UNUSED(params);

	if (default_conn != conn) {
		return;
	}

	if (err) {
		printk("Auth response write failed: 0x%02x\n", err);
		return;
	}

	gatt_setup_complete = true;
	k_work_cancel_delayable(&gatt_setup_timeout_work);
	printk("Auth response accepted - discovering sensors\n");
	discover_sensors();
}

static void send_response(void)
{
	int err;

	memcpy(response_buf, client_cert, CERT_TOTAL_LEN);

	write_params.func = response_written;
	write_params.handle = response_handle;
	write_params.offset = 0;
	write_params.data = response_buf;
	write_params.length = RESPONSE_LEN;

	err = bt_gatt_write(default_conn, &write_params);
	if (err) {
		printk("bt_gatt_write failed: %d\n", err);
	}
}

/* ---------------- auth: read challenge ---------------- */

static uint8_t challenge_read(struct bt_conn *conn,
			      uint8_t err,
			      struct bt_gatt_read_params *params,
			      const void *data,
			      uint16_t len)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err || !data || len < NONCE_LEN) {
		printk("Challenge read failed (err %u len %u)\n", err, len);
		return BT_GATT_ITER_STOP;
	}

	printk("Got %u-byte nonce, signing...\n", len);

	if (sign_nonce(data, NONCE_LEN, &response_buf[CERT_TOTAL_LEN]) == 0) {
		send_response();
	}

	return BT_GATT_ITER_STOP;
}

static void read_challenge(void)
{
	int err;

	read_params.func = challenge_read;
	read_params.handle_count = 1;
	read_params.single.handle = challenge_handle;
	read_params.single.offset = 0;

	err = bt_gatt_read(default_conn, &read_params);
	if (err) {
		printk("bt_gatt_read failed: %d\n", err);
	}
}

/* ---------------- discovery: auth service ---------------- */

static void auth_dm_completed(struct bt_gatt_dm *dm, void *ctx)
{
	const struct bt_gatt_dm_attr *chr;

	ARG_UNUSED(ctx);

	challenge_handle = 0;
	response_handle = 0;

	chr = bt_gatt_dm_char_by_uuid(dm, &challenge_uuid.uuid);
	if (chr) {
		challenge_handle = dm_char_value_handle(chr);
	}

	chr = bt_gatt_dm_char_by_uuid(dm, &response_uuid.uuid);
	if (chr) {
		response_handle = dm_char_value_handle(chr);
	}

	bt_gatt_dm_data_release(dm);

	if (challenge_handle && response_handle) {
		printk("Auth service found - reading challenge\n");
		read_challenge();
	} else {
		printk("Auth characteristics not found. challenge=0x%04x response=0x%04x\n",
		       challenge_handle, response_handle);
	}
}

static struct bt_gatt_dm_cb auth_dm_cb = {
	.completed = auth_dm_completed,
	.error_found = dm_error,
};

/* ---------------- MTU exchange ---------------- */

static void mtu_done(struct bt_conn *conn,
		     uint8_t err,
		     struct bt_gatt_exchange_params *params)
{
	int e;

	ARG_UNUSED(params);

	if (default_conn != conn) {
		return;
	}

	if (err) {
		printk("MTU exchange failed: ATT error 0x%02x\n", err);
		return;
	}

	printk("MTU exchange ok (%u)\n", bt_gatt_get_mtu(conn));

	e = bt_gatt_dm_start(conn, &auth_svc_uuid.uuid, &auth_dm_cb, NULL);
	if (e) {
		printk("Auth discovery start failed: %d\n", e);
	}
}

static void gatt_start_fn(struct k_work *work)
{
	struct bt_conn *conn = default_conn;
	int err;

	ARG_UNUSED(work);

	if (!conn) {
		return;
	}

	/* Do not issue the first ATT request directly from security_changed().
	 * The peer is still restoring its bonded CCCs and running its own security
	 * callback at that point. A short gap also lets old-link TX buffers drain. */
	k_work_reschedule(&gatt_setup_timeout_work,
			  K_MSEC(GATT_SETUP_TIMEOUT_MS));
	printk("Starting authenticated GATT setup\n");
	mtu_params.func = mtu_done;
	err = bt_gatt_exchange_mtu(conn, &mtu_params);
	if (err) {
		printk("MTU exchange request failed: %d\n", err);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

/* ---------------- SS2 pairing/auth callbacks ---------------- */

static void mark_security_recovery(struct bt_conn *conn)
{
	security_retry_backoff = true;
	if (conn) {
		bt_addr_le_copy(&bond_cleanup_addr, bt_conn_get_dst(conn));
		bond_cleanup_pending = true;
		bond_cleanup_attempts = 0;
	}
}

/* If stored keys genuinely fail, remove only that bond after the connection
 * object has finished tearing down. The next retry can then pair cleanly. */
static void bond_cleanup_work_fn(struct k_work *work)
{
	struct bt_conn *existing;
	int err;

	if (!bond_cleanup_pending) {
		return;
	}

	existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &bond_cleanup_addr);
	if (existing) {
		bt_conn_unref(existing);
		if (++bond_cleanup_attempts < 6) {
			k_work_reschedule(k_work_delayable_from_work(work),
					  K_MSEC(250));
		}
		return;
	}

	err = bt_unpair(BT_ID_DEFAULT, &bond_cleanup_addr);
	if (err && err != -ENOENT) {
		printk("Failed to clear rejected SS1 bond: %d\n", err);
	} else {
		printk("Rejected SS1 bond cleared; next attempt will pair fresh\n");
	}
	bond_cleanup_pending = false;
}

static K_WORK_DELAYABLE_DEFINE(bond_cleanup_work, bond_cleanup_work_fn);

static enum bt_security_err ss2_pairing_accept(struct bt_conn *conn,
					       const struct bt_conn_pairing_feat *const feat)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(feat);

	printk("SS2 pairing accepted\n");
	return BT_SECURITY_ERR_SUCCESS;
}

static void ss2_pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	printk("SS2 pairing cancelled\n");
}

static void ss2_pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	printk("SS2 pairing complete. Bonded: %s\n", bonded ? "yes" : "no");
}

static void ss2_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	printk("SS2 pairing failed. Reason: %d\n", reason);
	if (default_conn == conn) {
		mark_security_recovery(conn);
	}
}

static struct bt_conn_auth_cb ss2_auth_cb = {
	.pairing_accept = ss2_pairing_accept,
	.cancel = ss2_pairing_cancel,
};

static struct bt_conn_auth_info_cb ss2_auth_info_cb = {
	.pairing_complete = ss2_pairing_complete,
	.pairing_failed = ss2_pairing_failed,
};
/* ---------------- connection callbacks ---------------- */
/* Allow Zephyr's full 30-second Secure Connections procedure to finish before
 * this outer watchdog intervenes. The old 10-second limit aborted valid fresh
 * pairings and surfaced as BT_SECURITY_ERR_UNSPECIFIED (9). */
static void security_timeout_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (default_conn && !gatt_setup_complete) {
		printk("Security not established in time - disconnecting to retry\n");
		mark_security_recovery(default_conn);
		bt_conn_disconnect(default_conn, BT_HCI_ERR_AUTH_FAIL);
	}
}
static K_WORK_DELAYABLE_DEFINE(security_timeout_work, security_timeout_fn);

static void security_start_fn(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (!default_conn) {
		return;
	}

	/* Let SS1 finish its connected callback and fixed-channel setup before
	 * sending the first SMP PDU. */
	k_work_reschedule(&security_timeout_work,
			  K_MSEC(SECURITY_SETUP_TIMEOUT_MS));
	printk("Requesting encrypted security for %s\n", TARGET_NAME);
	err = bt_conn_set_security(default_conn, BT_SECURITY_L2);
	if (err) {
		printk("Failed to set security: %d\n", err);
		mark_security_recovery(default_conn);
		bt_conn_disconnect(default_conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}

	printk("Security request queued\n");
}
static K_WORK_DELAYABLE_DEFINE(security_start_work, security_start_fn);

static void gatt_setup_timeout_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (default_conn) {
		printk("GATT setup stalled - disconnecting to retry cleanly\n");
		bt_gatt_cancel(default_conn, &mtu_params);
		bt_gatt_cancel(default_conn, &read_params);
		bt_gatt_cancel(default_conn, &write_params);
		bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}
static void restart_scan_work_fn(struct k_work *work)
{
	int err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err == -EALREADY) {
		return;                 /* already scanning, fine */
	}
	if (err) {
		printk("Scan restart failed: %d, retrying...\n", err);
		k_work_reschedule(k_work_delayable_from_work(work),
				  K_MSEC(SCAN_RETRY_DELAY_MS));
	} else {
		printk("Scanning for %s ...\n", TARGET_NAME);
	}
}
static K_WORK_DELAYABLE_DEFINE(restart_scan_work, restart_scan_work_fn);

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		printk("Failed to connect to %s: 0x%02x\n", addr, err);

		if (default_conn == conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;
		}

		k_work_reschedule(&restart_scan_work,
				  K_MSEC(SCAN_RETRY_DELAY_MS));
		return;
	}

	printk("Connected to %s (%s)\n", TARGET_NAME, addr);
	security_retry_backoff = false;
	gatt_setup_complete = false;

	printk("Allowing SS1 %u ms to settle before security\n",
	       SECURITY_START_DELAY_MS);
	k_work_reschedule(&security_start_work,
			  K_MSEC(SECURITY_START_DELAY_MS));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	uint32_t retry_delay_ms;

	printk("Disconnected (reason 0x%02x)\n", reason);

	if (default_conn != conn) {
		printk("Ignoring stale disconnect callback\n");
		return;
	}

	k_work_cancel_delayable(&security_timeout_work);
	k_work_cancel_delayable(&security_start_work);
	k_work_cancel_delayable(&gatt_start_work);
	k_work_cancel_delayable(&gatt_setup_timeout_work);

	if (default_conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}
	if (bond_cleanup_pending) {
		k_work_reschedule(&bond_cleanup_work, K_MSEC(250));
	}
	challenge_handle = 0;
	response_handle = 0;
	gatt_setup_complete = false;

	/* Security failures need a real no-connection gap so SS1 can finish SMP
	 * teardown before this central is allowed to reconnect. Normal link drops
	 * still retry quickly. */
	retry_delay_ms = security_retry_backoff ? SECURITY_RETRY_DELAY_MS :
						 SCAN_RETRY_DELAY_MS;
	security_retry_backoff = false;
	printk("Restarting scan in %u ms\n", retry_delay_ms);
	k_work_reschedule(&restart_scan_work, K_MSEC(retry_delay_ms));
}

static void security_changed(struct bt_conn *conn,
			     bt_security_t level,
			     enum bt_security_err err)
{
	if (default_conn != conn) {
		printk("Ignoring stale security callback\n");
		return;
	}

	if (err) {
		printk("Security failed: level %u err %d - disconnecting to retry\n",
		       level, err);
		mark_security_recovery(conn);
		/* Drop the link so disconnected() restarts scanning; staying
		 * connected unencrypted would just hang here forever. */
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}

	printk("Security level %u\n", level);
	if (level < BT_SECURITY_L2) {
		return;
	}

	security_retry_backoff = false;
	bond_cleanup_pending = false;

	k_work_cancel_delayable(&security_timeout_work);
	printk("Allowing SS1 %u ms to finish post-security setup\n",
	       GATT_START_DELAY_MS);
	k_work_reschedule(&gatt_start_work, K_MSEC(GATT_START_DELAY_MS));
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

/* ---------------- scanning ---------------- */

static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match,
			      bool connectable)
{
	ARG_UNUSED(device_info);
	ARG_UNUSED(filter_match);
	ARG_UNUSED(connectable);

	printk("SS1 found, connecting...\n");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
			    struct bt_conn *conn)
{
	ARG_UNUSED(device_info);

	if (default_conn == conn) {
		return;
	}

	if (default_conn) {
		printk("Replacing stale pending connection reference\n");
		bt_conn_unref(default_conn);
	}
	default_conn = bt_conn_ref(conn);
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL, NULL, scan_connecting);

static int scan_init(void)
{
	int err;

	struct bt_scan_init_param param = {
		.connect_if_match = true,
		.scan_param = NULL,
		.conn_param = NULL,
	};

	bt_scan_init(&param);
	bt_scan_cb_register(&scan_cb);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_NAME, TARGET_NAME);
	if (err) {
		printk("Scan filter add failed: %d\n", err);
		return err;
	}

	err = bt_scan_filter_enable(BT_SCAN_NAME_FILTER, false);
	if (err) {
		printk("Scan filter enable failed: %d\n", err);
		return err;
	}

	return 0;
}

/* ---------------- main ---------------- */

int main(void)
{
	int err;
	psa_status_t psa_status;

	printk("\n\nSS2 CENTRAL STARTED\n");

	err = bt_enable(NULL);
	if (err) {
		printk("bt_enable failed: %d\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	err = bt_conn_auth_cb_register(&ss2_auth_cb);
	if (err) {
		printk("SS2 auth cb register failed: %d\n", err);
		return 0;
	}

	err = bt_conn_auth_info_cb_register(&ss2_auth_info_cb);
	if (err) {
		printk("SS2 auth info cb register failed: %d\n", err);
		return 0;
	}

	printk("SS2 pairing/auth callbacks registered\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		err = settings_load();
		if (err) {
			printk("Settings load failed: %d\n", err);
		} else {
			printk("Settings loaded\n");
		}
	}

	psa_status = psa_crypto_init();
	if (psa_status != PSA_SUCCESS) {
		printk("PSA crypto init failed: %d\n", (int)psa_status);
		return 0;
	}

	printk("PSA crypto initialized\n");

	if (import_client_key()) {
		printk("Crypto key import failed\n");
		return 0;
	}

	err = scan_init();
	if (err) {
		printk("Scan init failed: %d\n", err);
		return 0;
	}

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err) {
		printk("Scan start failed: %d\n", err);
		return 0;
	}

	printk("Scanning for %s ...\n", TARGET_NAME);

	return 0;
}
