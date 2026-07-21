/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/mcuboot.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/hwinfo.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>

#include <zephyr/settings/settings.h>

#include <app_version.h>

#include "sentry_auth.h"
#include "cs_reflector.h"

/* ---------------- Timing ---------------- */

#define SAMPLE_PERIOD_MS 500
#define NOTIFY_PERIOD_MS 1000
#define BUTTON_DEBOUNCE_MS 250
#define RECONNECT_QUIET_MS 1500

#define TEMP_AVG_WINDOW_SAMPLES 6
#define HUM_AVG_WINDOW_SAMPLES  6

/* ---------------- I2C / SHT3x ---------------- */

#define PROD_I2C_NODE DT_NODELABEL(prod_i2c)
#define DK_I2C_NODE   DT_NODELABEL(i2c21)
#define DEBUG_UART_NODE DT_NODELABEL(uart30)
#define SHT3X_ADDR_PRIMARY   0x44
#define SHT3X_ADDR_ALTERNATE 0x45

static const struct device *const i2c_candidates[] = {
	DEVICE_DT_GET(PROD_I2C_NODE),
	DEVICE_DT_GET(DK_I2C_NODE),
};
static const struct device *i2c_dev;
static const struct device *const debug_uart = DEVICE_DT_GET(DEBUG_UART_NODE);
enum sensor_kind {
	SENSOR_NONE,
	SENSOR_SHT3X,
	SENSOR_SHT4X,
};

static bool sensor_ready;
static enum sensor_kind sensor_kind;
static uint16_t sensor_addr = SHT3X_ADDR_PRIMARY;

static int debug_uart_char_out(int c)
{
	/* A raw UART hook does not perform the console's LF-to-CRLF conversion. */
	if (c == '\n') {
		uart_poll_out(debug_uart, '\r');
	}
	uart_poll_out(debug_uart, (unsigned char)c);
	return c;
}

static void debug_uart_force_console(void)
{
	if (device_is_ready(debug_uart)) {
		/* Ensure printk always targets the production board's soldered TX. */
		__printk_hook_install(debug_uart_char_out);
	}
}

/* ---------------- Device name ---------------- */

#define DEVICE_NAME_PREFIX CONFIG_BT_DEVICE_NAME "-"
#define DEVICE_ID_BYTES 8
#define DEVICE_ID_HEX_LEN (DEVICE_ID_BYTES * 2)

static char device_name[sizeof(DEVICE_NAME_PREFIX) + DEVICE_ID_HEX_LEN];

/* ---------------- GPIO aliases ---------------- */

#define MAINT_LED_NODE     DT_ALIAS(led0)
#define MAINT_BUTTON_NODE  DT_ALIAS(sw0)
#define SENSOR_ENABLE_NODE DT_NODELABEL(sensor_enable)

static const struct gpio_dt_spec maint_led =
	GPIO_DT_SPEC_GET(MAINT_LED_NODE, gpios);

static const struct gpio_dt_spec maint_button =
	GPIO_DT_SPEC_GET(MAINT_BUTTON_NODE, gpios);

static const struct gpio_dt_spec sensor_enable =
	GPIO_DT_SPEC_GET(SENSOR_ENABLE_NODE, enable_gpios);

static struct gpio_callback maint_button_cb;

/* ---------------- Shared sensor state ----------------
 *
 * Temperature:
 *   centi-degrees Celsius
 *   2534 = 25.34 C
 *
 * Humidity:
 *   centi-percent relative humidity
 *   5034 = 50.34 %RH
 */

static int16_t temp_samples_cdeg[TEMP_AVG_WINDOW_SAMPLES];
static uint8_t temp_sample_count;
static uint8_t temp_sample_index;
static int16_t temp_avg_cdeg;

static uint16_t hum_samples_cpercent[HUM_AVG_WINDOW_SAMPLES];
static uint8_t hum_sample_count;
static uint8_t hum_sample_index;
static uint16_t hum_avg_cpercent;

static uint8_t led_state;

static volatile bool maint_button_pending;

/* ---------------- Multiple connection state ---------------- */

static struct bt_conn *connections[CONFIG_BT_MAX_CONN];
static uint8_t active_conn_count;
static bool reconnect_quiet;
static bool cold_reboot_pending;

/* ---------------- Bluetooth UUIDs ----------------
 *
 * Environmental Sensing Service:
 *   UUID 0x181A
 *
 * Temperature Characteristic:
 *   UUID 0x2A6E
 *   sint16, 0.01 C resolution
 *
 * Humidity Characteristic:
 *   UUID 0x2A6F
 *   uint16, 0.01 %RH resolution
 */

#define MY_BT_UUID_ESS_VAL         0x181A
#define MY_BT_UUID_TEMPERATURE_VAL 0x2A6E
#define MY_BT_UUID_HUMIDITY_VAL    0x2A6F

static struct bt_uuid_16 ess_service_uuid =
	BT_UUID_INIT_16(MY_BT_UUID_ESS_VAL);

static struct bt_uuid_16 ess_temp_uuid =
	BT_UUID_INIT_16(MY_BT_UUID_TEMPERATURE_VAL);

static struct bt_uuid_16 ess_hum_uuid =
	BT_UUID_INIT_16(MY_BT_UUID_HUMIDITY_VAL);

/* ---------------- Custom UUIDs ---------------- */

#define BT_UUID_SENTRY_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x7e8f0000, 0x1111, 0x2222, 0x3333, 0x123456789abc)

#define BT_UUID_SENTRY_LED_VAL \
	BT_UUID_128_ENCODE(0x7e8f0002, 0x1111, 0x2222, 0x3333, 0x123456789abc)

static struct bt_uuid_128 sentry_service_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_SERVICE_VAL);

static struct bt_uuid_128 sentry_led_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_LED_VAL);

/* ---------------- Forward declarations ---------------- */

static void notify_temperature(void);
static void notify_humidity(void);
static void notify_led_state(void);
static void set_led_state(uint8_t state, bool send_notify);

static void advertising_start(void);

static int init_permanent_device_name(void)
{
	uint8_t id[DEVICE_ID_BYTES];
	ssize_t id_len = hwinfo_get_device_id(id, sizeof(id));
	int pos;

	if (id_len < 0) {
		return (int)id_len;
	}
	if (id_len < DEVICE_ID_BYTES) {
		return -ENODATA;
	}

	pos = snprintf(device_name, sizeof(device_name), "%s", DEVICE_NAME_PREFIX);
	for (int i = 0; i < DEVICE_ID_BYTES; i++) {
		pos += snprintf(&device_name[pos], sizeof(device_name) - pos, "%02X", id[i]);
	}
	return 0;
}

/* ---------------- Connection tracking helpers ---------------- */

static int connection_index(struct bt_conn *conn)
{
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (connections[i] == conn) {
			return i;
		}
	}

	return -1;
}

static bool connection_store(struct bt_conn *conn)
{
	if (connection_index(conn) >= 0) {
		return true;
	}

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (connections[i] == NULL) {
			connections[i] = bt_conn_ref(conn);
			active_conn_count++;

			printk("Connection stored in slot %d. Active connections: %u/%d\n",
			       i, active_conn_count, CONFIG_BT_MAX_CONN);

			return true;
		}
	}

	printk("No free connection slots\n");
	return false;
}

static void connection_remove(struct bt_conn *conn)
{
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (connections[i] == conn) {
			bt_conn_unref(connections[i]);
			connections[i] = NULL;

			if (active_conn_count > 0) {
				active_conn_count--;
			}

			printk("Connection removed from slot %d. Active connections: %u/%d\n",
			       i, active_conn_count, CONFIG_BT_MAX_CONN);

			return;
		}
	}
}

/* ---------------- Security / pairing callbacks ---------------- */

static enum bt_security_err auth_pairing_accept(struct bt_conn *conn,
						const struct bt_conn_pairing_feat *const feat)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(feat);

	/* Cert auth is the real gate now (HTTPS-style). Accept the encrypted
	 * link from anyone; the challenge-response decides who gets data. */
	return BT_SECURITY_ERR_SUCCESS;
}

static void auth_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	printk("Pairing cancelled\n");
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);

	printk("Pairing complete. Bonded: %s\n", bonded ? "yes" : "no");
}

/* Clearing a stale or mismatched bond after a failed pairing must NOT happen
 * inside the pairing_failed/disconnected callbacks: those run in the BT
 * stack's own thread while the connection object and its session keys are
 * still being torn down, and bt_unpair() there frees memory the teardown
 * still touches (observed as a bus fault). Instead the failure is flagged
 * and the unpair runs later from the system workqueue, only once no live
 * connection to that address remains.
 */
static bt_addr_le_t unpair_addr;
static uint8_t unpair_attempts;

static void unpair_work_handler(struct k_work *work)
{
	struct bt_conn *existing;
	int err;

	existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, &unpair_addr);
	if (existing) {
		/* Peer already reconnected (or teardown unfinished): can't
		 * clear state under a live connection. Retry briefly; if the
		 * peer is still connected after that, this cycle is skipped -
		 * if its pairing wedges, its own security timeout will
		 * disconnect and reschedule us. */
		bt_conn_unref(existing);
		if (++unpair_attempts < 6) {
			k_work_reschedule(k_work_delayable_from_work(work),
					  K_MSEC(500));
		}
		return;
	}

	err = bt_unpair(BT_ID_DEFAULT, &unpair_addr);
	if (err && err != -ENOENT) {
		printk("Failed to clear stale pairing state: %d\n", err);
	} else {
		printk("Pairing state cleared for departed peer\n");
	}
}

static K_WORK_DELAYABLE_DEFINE(unpair_work, unpair_work_handler);

static void schedule_unpair(struct bt_conn *conn)
{
	bt_addr_le_copy(&unpair_addr, bt_conn_get_dst(conn));
	unpair_attempts = 0;
	k_work_reschedule(&unpair_work, K_MSEC(500));
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	printk("Pairing failed. Reason: %d\n", reason);

	/* A bond that can no longer encrypt is not useful. Clear it after link
	 * teardown so the next attempt performs a clean automatic pairing. */
	schedule_unpair(conn);
}

static struct bt_conn_auth_cb auth_cb = {
	.pairing_accept = auth_pairing_accept,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* ---------------- GATT callbacks: Temperature ---------------- */

static ssize_t read_temp_cb(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr,
			    void *buf,
			    uint16_t len,
			    uint16_t offset)
{
	int16_t value_le;

	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}

	ARG_UNUSED(attr);

	value_le = (int16_t)sys_cpu_to_le16((uint16_t)temp_avg_cdeg);

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &value_le, sizeof(value_le));
}

static void temp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	printk("Temperature notifications %s by a client\n",
	       value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

/* ---------------- GATT callbacks: Humidity ---------------- */

static ssize_t read_hum_cb(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   void *buf,
			   uint16_t len,
			   uint16_t offset)
{
	uint16_t value_le;

	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}

	ARG_UNUSED(attr);

	value_le = sys_cpu_to_le16(hum_avg_cpercent);

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &value_le, sizeof(value_le));
}

static void hum_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	printk("Humidity notifications %s by a client\n",
	       value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

/* ---------------- GATT callbacks: LED / Maintenance ---------------- */

static ssize_t read_led_cb(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   void *buf,
			   uint16_t len,
			   uint16_t offset)
{
	uint8_t value = led_state;

	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}

	ARG_UNUSED(attr);

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &value, sizeof(value));
}

static ssize_t write_led_cb(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr,
			    const void *buf,
			    uint16_t len,
			    uint16_t offset,
			    uint8_t flags)
{
	const uint8_t *data = buf;

	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (data[0] != 0 && data[0] != 1) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	set_led_state(data[0], true);

	printk("LED0 state written from phone: %u\n", data[0]);

	return len;
}

static void led_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	printk("LED state notifications %s by a client\n",
	       value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

static ssize_t read_firmware_revision_cb(struct bt_conn *conn,
					 const struct bt_gatt_attr *attr,
					 void *buf, uint16_t len, uint16_t offset)
{
	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 APP_VERSION_STRING, strlen(APP_VERSION_STRING));
}

static bool sentry_write_authorize(struct bt_conn *conn,
				   const struct bt_gatt_attr *attr)
{
	/* CCC and all application characteristics keep their own permissions.
	 * The SMP request characteristic is the only stock Zephyr endpoint whose
	 * write callback otherwise has no application authentication hook. */
	if (bt_uuid_cmp(attr->uuid, SMP_BT_CHR_UUID) == 0) {
		return sentry_auth_is_authenticated(conn);
	}
	return true;
}

static const struct bt_gatt_authorization_cb sentry_authorization = {
	.write_authorize = sentry_write_authorize,
};

/* ---------------- GATT services ---------------- */

BT_GATT_SERVICE_DEFINE(ess_svc,
	BT_GATT_PRIMARY_SERVICE(&ess_service_uuid.uuid),

	BT_GATT_CHARACTERISTIC(&ess_temp_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_temp_cb,
			       NULL,
			       NULL),
	BT_GATT_CCC(temp_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CUD("Average Temperature", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(&ess_hum_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_hum_cb,
			       NULL,
			       NULL),
	BT_GATT_CCC(hum_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CUD("Average Humidity", BT_GATT_PERM_READ),
);

BT_GATT_SERVICE_DEFINE(sentry_svc,
	BT_GATT_PRIMARY_SERVICE(&sentry_service_uuid),

	BT_GATT_CHARACTERISTIC(&sentry_led_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
			       read_led_cb,
			       write_led_cb,
			       NULL),
	BT_GATT_CCC(led_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CUD("LED Control / Maintenance State", BT_GATT_PERM_READ),
);

BT_GATT_SERVICE_DEFINE(device_info_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DIS),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_FIRMWARE_REVISION,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_firmware_revision_cb, NULL, NULL),
);

/* ---------------- Advertising ---------------- */

static const struct bt_le_adv_param *adv_param =
	BT_LE_ADV_PARAM(
		BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE,
		BT_GAP_ADV_FAST_INT_MIN_2,
		BT_GAP_ADV_FAST_INT_MAX_2,
		NULL
	);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SMP_BT_SVC_UUID_VAL),
};

static struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, device_name, 0),
};

static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	advertising_start();
}

K_WORK_DEFINE(adv_work, adv_work_handler);

static void reconnect_quiet_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	reconnect_quiet = false;
	printk("Reconnect cleanup complete\n");
	advertising_start();
}

static K_WORK_DELAYABLE_DEFINE(reconnect_quiet_work,
				       reconnect_quiet_work_handler);

static void advertising_start(void)
{
	int err;

	if (reconnect_quiet) {
		return;
	}

	if (active_conn_count >= CONFIG_BT_MAX_CONN) {
		printk("Max connections reached. Advertising not restarted.\n");
		return;
	}

	err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err == -EALREADY) {
		return;
	}

	if (err) {
		printk("Advertising failed to start: %d\n", err);
		return;
	}

	printk("Advertising started: %s, connection slots available: %u/%d\n",
	       device_name,
	       CONFIG_BT_MAX_CONN - active_conn_count,
	       CONFIG_BT_MAX_CONN);
}

/* ---------------- Connection callbacks ---------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	bool duplicate_peer = false;
	int disconnect_err;

	if (err) {
		printk("Connection failed: %u\n", err);
		return;
	}

	/* Advertising can already be active for another connection slot when the
	 * timed-out CS link is removed. Do not let a fast-scanning SS2 enter the
	 * 300 ms reset window and postpone the controller reset. */
	if (cold_reboot_pending) {
		printk("Rejecting connection: Channel Sounding reset pending\n");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	printk("Connected\n");

	/* A power-cycled central can reconnect before its old link's supervision
	 * timeout fires. Starting SMP on that replacement while the old link is
	 * only *disconnecting* races Zephyr's per-peer SMP teardown and can wedge
	 * every subsequent attempt. Detect the collision before requesting
	 * security, close both copies, and briefly stop advertising so all link
	 * state is gone before the central tries again. */
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (connections[i] != NULL && connections[i] != conn &&
		    bt_addr_le_cmp(bt_conn_get_dst(connections[i]),
				   bt_conn_get_dst(conn)) == 0) {
			duplicate_peer = true;
			break;
		}
	}

	if (duplicate_peer) {
		reconnect_quiet = true;
		k_work_reschedule(&reconnect_quiet_work,
				  K_MSEC(RECONNECT_QUIET_MS));
		printk("Duplicate peer link detected - resetting both links\n");

		for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
			if (connections[i] != NULL && connections[i] != conn &&
			    bt_addr_le_cmp(bt_conn_get_dst(connections[i]),
					   bt_conn_get_dst(conn)) == 0) {
				disconnect_err = bt_conn_disconnect(
					connections[i],
					BT_HCI_ERR_REMOTE_USER_TERM_CONN);
				if (disconnect_err && disconnect_err != -EALREADY) {
					printk("Failed to drop stale peer link: %d\n",
					       disconnect_err);
				}
			}
		}
	}

	if (!connection_store(conn)) {
		printk("Disconnecting: no free connection slots\n");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	if (duplicate_peer) {
		disconnect_err = bt_conn_disconnect(
			conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (disconnect_err) {
			printk("Failed to reset replacement peer link: %d\n",
			       disconnect_err);
		}
		return;
	}


	/* The central is the sole security initiator. Initiating SMP from both
	 * sides in their connected callbacks is harmless on a fresh link, but
	 * can wedge Zephyr SMP when this link replaces a half-dead predecessor. */
	printk("Waiting for central to establish encrypted security\n");


	if (active_conn_count < CONFIG_BT_MAX_CONN) {
		k_work_submit(&adv_work);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	bool was_authenticated;

	printk("Disconnected, reason: %u\n", reason);

	was_authenticated = sentry_auth_is_authenticated(conn);
	connection_remove(conn);
	sentry_auth_conn_removed(conn);

	if (was_authenticated && reason == BT_HCI_ERR_CONN_TIMEOUT &&
	    IS_ENABLED(CONFIG_BT_CHANNEL_SOUNDING)) {
		cold_reboot_pending = true;
		printk("Authenticated CS link timed out - cold reset required\n");
	}

	if (cold_reboot_pending && active_conn_count == 0U) {
		/* Nordic's NCS 3.4 RAS samples reset directly from disconnected().
		 * Delaying this on the system workqueue leaves enough time for a new
		 * ACL event to wedge the controller before the work can run. */
		printk("Cold rebooting to reset Channel Sounding controller state\n");
		sys_reboot(SYS_REBOOT_COLD);
		return;
	}

	if (reconnect_quiet) {
		k_work_reschedule(&reconnect_quiet_work,
				  K_MSEC(RECONNECT_QUIET_MS));
	} else if (active_conn_count < CONFIG_BT_MAX_CONN) {
		k_work_submit(&adv_work);
	}
}

static void security_changed(struct bt_conn *conn,
			     bt_security_t level,
			     enum bt_security_err err)
{
	if (err) {
		printk("Security failed: level %u err %d\n", level, err);
		schedule_unpair(conn);
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}

	printk("Security changed: level %u\n", level);

	if (level >= BT_SECURITY_L2) {
		sentry_auth_conn_added(conn);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

/* ---------------- Button interrupts ---------------- */

static void maint_button_pressed_cb(const struct device *port,
				    struct gpio_callback *cb,
				    gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	maint_button_pending = true;
}

/* ---------------- SHT3x helpers ---------------- */

static uint8_t sht3x_crc8(const uint8_t *data, int len)
{
	uint8_t crc = 0xFF;

	for (int i = 0; i < len; i++) {
		crc ^= data[i];

		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x80) {
				crc = (crc << 1) ^ 0x31;
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

static int16_t fake_temp_cdeg(void)
{
	static int16_t fake = 2400;

	fake += 5;

	if (fake > 2800) {
		fake = 2400;
	}

	return fake;
}

static const char *sensor_kind_name(enum sensor_kind kind)
{
	return kind == SENSOR_SHT4X ? "SHT4x" : "SHT3x";
}

static bool read_sensor_sample(int16_t *out_temp_cdeg,
			       uint16_t *out_hum_cpercent)
{
	int ret;
	const uint8_t sht3x_cmd[2] = {0x24, 0x00};
	const uint8_t sht4x_cmd = 0xFD;
	const uint8_t *cmd;
	size_t cmd_len;
	uint8_t rx[6];
	uint8_t temp_crc;
	uint8_t hum_crc;
	uint16_t raw_temp;
	uint16_t raw_hum;
	int32_t temp_cdeg;
	int32_t hum_cpercent;

	if (!sensor_ready) {
		printk("Temperature sensor not ready. Using fake temp.\n");
		*out_temp_cdeg = fake_temp_cdeg();
		*out_hum_cpercent = 0;
		return false;
	}

	if (sensor_kind == SENSOR_SHT4X) {
		cmd = &sht4x_cmd;
		cmd_len = sizeof(sht4x_cmd);
	} else {
		cmd = sht3x_cmd;
		cmd_len = sizeof(sht3x_cmd);
	}

	ret = i2c_write(i2c_dev, cmd, cmd_len, sensor_addr);
	if (ret != 0) {
		printk("%s measurement command failed: %d. Using fake temp.\n",
		       sensor_kind_name(sensor_kind), ret);
		*out_temp_cdeg = fake_temp_cdeg();
		*out_hum_cpercent = 0;
		return false;
	}

	k_msleep(20);

	ret = i2c_read(i2c_dev, rx, sizeof(rx), sensor_addr);
	if (ret != 0) {
		printk("%s read failed: %d. Using fake temp.\n",
		       sensor_kind_name(sensor_kind), ret);
		*out_temp_cdeg = fake_temp_cdeg();
		*out_hum_cpercent = 0;
		return false;
	}

	temp_crc = sht3x_crc8(&rx[0], 2);
	hum_crc = sht3x_crc8(&rx[3], 2);

	if (temp_crc != rx[2]) {
		printk("%s temperature CRC failed. Using fake temp.\n",
		       sensor_kind_name(sensor_kind));
		*out_temp_cdeg = fake_temp_cdeg();
		*out_hum_cpercent = 0;
		return false;
	}

	if (hum_crc != rx[5]) {
		printk("%s humidity CRC failed. Using fake temp.\n",
		       sensor_kind_name(sensor_kind));
		*out_temp_cdeg = fake_temp_cdeg();
		*out_hum_cpercent = 0;
		return false;
	}

	raw_temp = ((uint16_t)rx[0] << 8) | rx[1];
	raw_hum = ((uint16_t)rx[3] << 8) | rx[4];

	temp_cdeg = -4500 + (int32_t)(((int64_t)17500 * raw_temp) / 65535);
	if (sensor_kind == SENSOR_SHT4X) {
		hum_cpercent = -600 + (int32_t)(((int64_t)12500 * raw_hum) / 65535);
		hum_cpercent = CLAMP(hum_cpercent, 0, 10000);
	} else {
		hum_cpercent = (int32_t)(((int64_t)10000 * raw_hum) / 65535);
	}

	*out_temp_cdeg = (int16_t)temp_cdeg;
	*out_hum_cpercent = (uint16_t)hum_cpercent;

	return true;
}

static void update_sensor_averages(void)
{
	int16_t temp_sample;
	uint16_t hum_sample;

	int32_t temp_sum = 0;
	uint32_t hum_sum = 0;

	uint8_t temp_count;
	uint8_t hum_count;

	read_sensor_sample(&temp_sample, &hum_sample);

	temp_samples_cdeg[temp_sample_index] = temp_sample;
	temp_sample_index = (temp_sample_index + 1) % TEMP_AVG_WINDOW_SAMPLES;

	if (temp_sample_count < TEMP_AVG_WINDOW_SAMPLES) {
		temp_sample_count++;
	}

	temp_count = temp_sample_count;

	for (uint8_t i = 0; i < temp_count; i++) {
		temp_sum += temp_samples_cdeg[i];
	}

	temp_avg_cdeg = (int16_t)(temp_sum / temp_count);

	hum_samples_cpercent[hum_sample_index] = hum_sample;
	hum_sample_index = (hum_sample_index + 1) % HUM_AVG_WINDOW_SAMPLES;

	if (hum_sample_count < HUM_AVG_WINDOW_SAMPLES) {
		hum_sample_count++;
	}

	hum_count = hum_sample_count;

	for (uint8_t i = 0; i < hum_count; i++) {
		hum_sum += hum_samples_cpercent[i];
	}

	hum_avg_cpercent = (uint16_t)(hum_sum / hum_count);

	printk("%s temp: %d.%02d C, temp avg: %d.%02d C | humidity: %u.%02u %%RH, humidity avg: %u.%02u %%RH\n",
	       sensor_kind_name(sensor_kind),
	       temp_sample / 100,
	       temp_sample < 0 ? -(temp_sample % 100) : temp_sample % 100,
	       temp_avg_cdeg / 100,
	       temp_avg_cdeg < 0 ? -(temp_avg_cdeg % 100) : temp_avg_cdeg % 100,
	       hum_sample / 100,
	       hum_sample % 100,
	       hum_avg_cpercent / 100,
	       hum_avg_cpercent % 100);
}

/* ---------------- BLE notify helpers ---------------- */

static void notify_temperature(void)
{
	int err;
	int16_t value_le;

	value_le = (int16_t)sys_cpu_to_le16((uint16_t)temp_avg_cdeg);

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (connections[i] == NULL) {
			continue;
		}

		if (!sentry_auth_is_authenticated(connections[i])) {
			continue;
		}

		if (!bt_gatt_is_subscribed(connections[i],
					   &ess_svc.attrs[2],
					   BT_GATT_CCC_NOTIFY)) {
			continue;
		}

		err = bt_gatt_notify(connections[i],
				     &ess_svc.attrs[2],
				     &value_le,
				     sizeof(value_le));

		if (err) {
			printk("Temperature notify failed on slot %d: %d\n", i, err);
		}
	}
}

static void notify_humidity(void)
{
	int err;
	uint16_t value_le;

	value_le = sys_cpu_to_le16(hum_avg_cpercent);

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (connections[i] == NULL) {
			continue;
		}

		if (!sentry_auth_is_authenticated(connections[i])) {
			continue;
		}

		if (!bt_gatt_is_subscribed(connections[i],
					   &ess_svc.attrs[6],
					   BT_GATT_CCC_NOTIFY)) {
			continue;
		}

		err = bt_gatt_notify(connections[i],
				     &ess_svc.attrs[6],
				     &value_le,
				     sizeof(value_le));

		if (err) {
			printk("Humidity notify failed on slot %d: %d\n", i, err);
		}
	}
}

static void notify_led_state(void)
{
	int err;
	uint8_t value = led_state;

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (connections[i] == NULL) {
			continue;
		}

		if (!sentry_auth_is_authenticated(connections[i])) {
			continue;
		}

		if (!bt_gatt_is_subscribed(connections[i],
					   &sentry_svc.attrs[2],
					   BT_GATT_CCC_NOTIFY)) {
			continue;
		}

		err = bt_gatt_notify(connections[i],
				     &sentry_svc.attrs[2],
				     &value,
				     sizeof(value));

		if (err) {
			printk("LED state notify failed on slot %d: %d\n", i, err);
		}
	}
}

static void set_led_state(uint8_t state, bool send_notify)
{
	int ret;

	led_state = state ? 1 : 0;

	ret = gpio_pin_set_dt(&maint_led, led_state);
	if (ret < 0) {
		printk("Failed to set LED0: %d\n", ret);
		return;
	}

	printk("LED0 maintenance state is now: %u\n", led_state);

	if (send_notify) {
		notify_led_state();
	}
}

/* ---------------- Init helpers ---------------- */

static int gpio_init_all(void)
{
	int ret;

	if (!gpio_is_ready_dt(&maint_led)) {
		printk("LED0 GPIO not ready\n");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&maint_button)) {
		printk("Button0 GPIO not ready\n");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&sensor_enable)) {
		printk("Sensor-enable GPIO not ready\n");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&maint_led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("Failed to configure LED0: %d\n", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&sensor_enable, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("Failed to enable sensor rail: %d\n", ret);
		return ret;
	}

	/* Allow the load switch, pull-ups and SHT41/SHT3x to power up. */
	k_msleep(20);
	printk("Sensor rail enabled on P2.03\n");

	ret = gpio_pin_configure_dt(&maint_button, GPIO_INPUT);
	if (ret < 0) {
		printk("Failed to configure Button0: %d\n", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&maint_button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		printk("Failed to configure Button0 interrupt: %d\n", ret);
		return ret;
	}

	gpio_init_callback(&maint_button_cb,
			   maint_button_pressed_cb,
			   BIT(maint_button.pin));

	ret = gpio_add_callback(maint_button.port, &maint_button_cb);
	if (ret < 0) {
		printk("Failed to add Button0 callback: %d\n", ret);
		return ret;
	}

	printk("GPIO ready\n");

	return 0;
}

static void sensor_init_check(void)
{
	static const uint16_t addresses[] = {
		SHT3X_ADDR_PRIMARY,
		SHT3X_ADDR_ALTERNATE,
	};
	const uint8_t sht3x_cmd[2] = {0x24, 0x00};
	const uint8_t sht4x_cmd = 0xFD;
	uint8_t rx[6];

	for (size_t bus = 0; bus < ARRAY_SIZE(i2c_candidates); bus++) {
		const struct device *candidate = i2c_candidates[bus];
		enum sensor_kind candidate_kind = bus == 0 ? SENSOR_SHT4X : SENSOR_SHT3X;
		const uint8_t *cmd = candidate_kind == SENSOR_SHT4X ?
			&sht4x_cmd : sht3x_cmd;
		size_t cmd_len = candidate_kind == SENSOR_SHT4X ?
			sizeof(sht4x_cmd) : sizeof(sht3x_cmd);

		if (!device_is_ready(candidate)) {
			printk("I2C bus %s is not ready\n", candidate->name);
			continue;
		}

		printk("Probing %s on I2C bus %s (%s wiring)\n",
		       sensor_kind_name(candidate_kind), candidate->name,
		       bus == 0 ? "production P0.04/P0.03" : "DK P1.11/P1.12");

		int recovery_ret = i2c_recover_bus(candidate);
		printk("I2C bus recovery on %s: %d\n", candidate->name, recovery_ret);

		for (size_t i = 0; i < ARRAY_SIZE(addresses); i++) {
			uint16_t address = addresses[i];

			/* The production SHT41 has a fixed 0x44 address. */
			if (candidate_kind == SENSOR_SHT4X &&
			    address != SHT3X_ADDR_PRIMARY) {
				continue;
			}

			printk("Probing %s address 0x%02X\n",
			       sensor_kind_name(candidate_kind), address);
			int write_ret = i2c_write(candidate, cmd, cmd_len, address);
			if (write_ret != 0) {
				printk("%s 0x%02X command failed: %d\n",
				       sensor_kind_name(candidate_kind), address, write_ret);
				continue;
			}
			k_msleep(20);
			int read_ret = i2c_read(candidate, rx, sizeof(rx), address);
			if (read_ret != 0) {
				printk("%s 0x%02X read failed: %d\n",
				       sensor_kind_name(candidate_kind), address, read_ret);
				continue;
			}
			if (sht3x_crc8(&rx[0], 2) != rx[2] ||
			    sht3x_crc8(&rx[3], 2) != rx[5]) {
				printk("%s 0x%02X replied with invalid CRC\n",
				       sensor_kind_name(candidate_kind), address);
				continue;
			}

			i2c_dev = candidate;
			sensor_addr = address;
			sensor_kind = candidate_kind;
			sensor_ready = true;
			printk("Detected %s on %s at address 0x%02X\n",
			       sensor_kind_name(sensor_kind), i2c_dev->name,
			       sensor_addr);
			printk("Sensor provides temperature and humidity over BLE\n");
			return;
		}
	}

	sensor_ready = false;
	sensor_kind = SENSOR_NONE;
	printk("No supported temperature sensor found at 0x%02X or 0x%02X. Using fake temperature.\n",
	       SHT3X_ADDR_PRIMARY, SHT3X_ADDR_ALTERNATE);
}

/* ---------------- main ---------------- */

int main(void)
{
	int ret;
	int64_t last_sample_ms;
	int64_t last_notify_ms;
	int64_t last_maint_button_ms = 0;

	debug_uart_force_console();
	printk("\n\nMAIN STARTED - SS1 firmware %s\n", APP_VERSION_STRING);
	printk("OTA validation marker: forced UART30 with CRLF + production GPIO I2C 1.0.15 is running\n");

	ret = init_permanent_device_name();
	if (ret) {
		printk("Permanent device ID unavailable: %d\n", ret);
		return ret;
	}
	sd[0].data_len = strlen(device_name);

	ret = gpio_init_all();
	if (ret < 0) {
		printk("GPIO init failed: %d\n", ret);
		return 0;
	}

	sensor_init_check();

	set_led_state(0, false);

	ret = bt_enable(NULL);
	if (ret) {
		printk("Bluetooth init failed: %d\n", ret);
		return 0;
	}

	printk("Bluetooth initialized\n");

	ret = bt_conn_auth_cb_register(&auth_cb);
	if (ret) {
		printk("Failed to register auth callbacks: %d\n", ret);
		return 0;
	}

	ret = bt_conn_auth_info_cb_register(&auth_info_cb);
	if (ret) {
		printk("Failed to register auth info callbacks: %d\n", ret);
		return 0;
	}

	ret = sentry_auth_init();
    if (ret) {
        printk("Sentry auth init failed: %d\n", ret);
        return 0;
    }

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		ret = settings_load();
		if (ret) {
			printk("Settings load failed: %d\n", ret);
		} else {
			printk("Settings loaded\n");
		}
	}

	ret = bt_gatt_authorization_cb_register(&sentry_authorization);
	if (ret) {
		printk("GATT authorization registration failed: %d\n", ret);
		return ret;
	}

	advertising_start();
	printk("OTA image state: %s; awaiting Pi health confirmation when pending\n",
	       boot_is_img_confirmed() ? "confirmed" : "trial/unconfirmed");

	last_sample_ms = k_uptime_get();
	last_notify_ms = k_uptime_get();

	while (1) {
		int64_t now = k_uptime_get();

		if ((now - last_sample_ms) >= SAMPLE_PERIOD_MS) {
			last_sample_ms = now;
			update_sensor_averages();
		}

		if ((now - last_notify_ms) >= NOTIFY_PERIOD_MS) {
			last_notify_ms = now;
			notify_temperature();
			notify_humidity();
		}

		if (maint_button_pending) {
			maint_button_pending = false;

			if ((now - last_maint_button_ms) >= BUTTON_DEBOUNCE_MS) {
				last_maint_button_ms = now;

				printk("Button0 pressed: maintenance dealt with, clearing LED0\n");
				set_led_state(0, true);
			}
		}

		k_msleep(50);
	}

	return 0;
}
