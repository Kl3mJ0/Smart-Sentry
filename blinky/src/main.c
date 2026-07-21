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
#include <zephyr/drivers/sensor.h>
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
#include "serial30_mux.h"

/* ---------------- Timing ---------------- */

#define FAST_SAMPLE_PERIOD_MS 500
#define MAX_SAMPLE_INTERVAL_SECONDS 30
#define BUTTON_DEBOUNCE_MS 250
#define RECONNECT_QUIET_MS 1500

#define TEMP_AVG_WINDOW_SAMPLES 6
#define HUM_AVG_WINDOW_SAMPLES  6

/* ---------------- I2C / SHT3x ---------------- */

#define PROD_I2C_NODE DT_NODELABEL(prod_i2c)
#define DK_I2C_NODE   DT_NODELABEL(dk_i2c)
#define EXT_I2C_NODE  DT_NODELABEL(ext_i2c)
#define EXT_THERMISTOR_NODE DT_NODELABEL(external_thermistor)
#define SHT3X_ADDR_PRIMARY   0x44
#define SHT3X_ADDR_ALTERNATE 0x45

static const struct device *const prod_i2c = DEVICE_DT_GET(PROD_I2C_NODE);
static const struct device *const dk_i2c = DEVICE_DT_GET(DK_I2C_NODE);
static const struct device *const ext_i2c = DEVICE_DT_GET(EXT_I2C_NODE);
static const struct device *const ext_thermistor = DEVICE_DT_GET(EXT_THERMISTOR_NODE);

enum operating_mode {
	MODE_DEBUG = 0,
	MODE_NORMAL_I2C = 1,
	MODE_EXTERNAL_AUTO = 2,
};

enum sensor_kind {
	SENSOR_NONE = 0,
	SENSOR_SHT3X = 1,
	SENSOR_SHT4X_INTERNAL = 2,
	SENSOR_SHT4X_EXTERNAL = 3,
	SENSOR_THERMISTOR = 4,
};

enum sensor_error {
	SENSOR_ERROR_OK = 0,
	SENSOR_ERROR_NOT_FOUND = 1,
	SENSOR_ERROR_BUS = 2,
	SENSOR_ERROR_CRC = 3,
	SENSOR_ERROR_ADC = 4,
	SENSOR_ERROR_HUMIDITY_UNAVAILABLE = 5,
};

static enum operating_mode operating_mode = MODE_DEBUG;
static uint8_t sample_interval_seconds;
static bool sensor_ready;
static enum sensor_kind sensor_kind;
static enum sensor_error sensor_error = SENSOR_ERROR_NOT_FOUND;
static uint16_t sensor_addr = SHT3X_ADDR_PRIMARY;
static const struct device *i2c_dev;
static bool serial30_twim_active;
static bool temp_valid;
static bool hum_valid;
static bool mode_reboot_pending;

static int debug_uart_char_out(int c)
{
	/* A raw UART hook does not perform the console's LF-to-CRLF conversion. */
	if (c == '\n') {
		serial30_uart_putc('\r');
	}
	serial30_uart_putc(c);
	return c;
}

static void debug_uart_force_console(void)
{
	if (operating_mode == MODE_DEBUG && serial30_uart_init() == 0) {
		/* Debug mode owns SERIAL30 as UARTE30 on the soldered P0.00 TX. */
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

static uint32_t sample_period_ms(void)
{
	return sample_interval_seconds == 0 ? FAST_SAMPLE_PERIOD_MS :
		(uint32_t)sample_interval_seconds * 1000U;
}

static const char *operating_mode_name(enum operating_mode mode)
{
	switch (mode) {
	case MODE_DEBUG:
		return "Debug UART + GPIO I2C";
	case MODE_NORMAL_I2C:
		return "Normal TWIM30";
	case MODE_EXTERNAL_AUTO:
		return "External auto-detect";
	default:
		return "Invalid";
	}
}

static int app_settings_set(const char *name, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	uint8_t value;
	int ret;

	if (len != sizeof(value)) {
		return -EINVAL;
	}
	ret = read_cb(cb_arg, &value, sizeof(value));
	if (ret < 0) {
		return ret;
	}

	if (strcmp(name, "mode") == 0) {
		if (value > MODE_EXTERNAL_AUTO) {
			return -EINVAL;
		}
		operating_mode = (enum operating_mode)value;
		return 0;
	}
	if (strcmp(name, "interval") == 0) {
		if (value > MAX_SAMPLE_INTERVAL_SECONDS) {
			return -EINVAL;
		}
		sample_interval_seconds = value;
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ss1_app, "ss1", NULL, app_settings_set,
			       NULL, NULL);

static void mode_reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(mode_reboot_work, mode_reboot_work_handler);

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

#define BT_UUID_SENTRY_MODE_VAL \
	BT_UUID_128_ENCODE(0x7e8f0003, 0x1111, 0x2222, 0x3333, 0x123456789abc)

#define BT_UUID_SENTRY_INTERVAL_VAL \
	BT_UUID_128_ENCODE(0x7e8f0004, 0x1111, 0x2222, 0x3333, 0x123456789abc)

#define BT_UUID_SENTRY_STATUS_VAL \
	BT_UUID_128_ENCODE(0x7e8f0005, 0x1111, 0x2222, 0x3333, 0x123456789abc)

static struct bt_uuid_128 sentry_service_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_SERVICE_VAL);

static struct bt_uuid_128 sentry_led_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_LED_VAL);

static struct bt_uuid_128 sentry_mode_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_MODE_VAL);

static struct bt_uuid_128 sentry_interval_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_INTERVAL_VAL);

static struct bt_uuid_128 sentry_status_uuid =
	BT_UUID_INIT_128(BT_UUID_SENTRY_STATUS_VAL);

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
	if (!temp_valid) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
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
	if (!hum_valid) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
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

struct sensor_status_value {
	uint8_t mode;
	uint8_t sensor_kind;
	uint8_t flags;
	uint8_t error;
	uint16_t interval_ms_le;
	uint16_t reserved_le;
} __packed;

#define STATUS_TEMP_VALID BIT(0)
#define STATUS_HUM_VALID BIT(1)
#define STATUS_REBOOT_PENDING BIT(2)

static ssize_t read_mode_cb(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	uint8_t value = (uint8_t)operating_mode;

	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

static ssize_t write_mode_cb(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t value;
	int ret;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	value = *(const uint8_t *)buf;
	if (value > MODE_EXTERNAL_AUTO) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (value == (uint8_t)operating_mode) {
		return len;
	}

	ret = settings_save_one("ss1/mode", &value, sizeof(value));
	if (ret != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	mode_reboot_pending = true;
	printk("Mode change saved: %u; rebooting to transfer SERIAL30 ownership\n", value);
	k_work_reschedule(&mode_reboot_work, K_MSEC(750));
	return len;
}

static ssize_t read_interval_cb(struct bt_conn *conn,
				const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	uint8_t value = sample_interval_seconds;

	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

static ssize_t write_interval_cb(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr, const void *buf,
				 uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t value;
	int ret;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	value = *(const uint8_t *)buf;
	if (value > MAX_SAMPLE_INTERVAL_SECONDS) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	ret = settings_save_one("ss1/interval", &value, sizeof(value));
	if (ret != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	sample_interval_seconds = value;
	printk("Sampling interval changed to %u ms\n", sample_period_ms());
	return len;
}

static ssize_t read_sensor_status_cb(struct bt_conn *conn,
				     const struct bt_gatt_attr *attr, void *buf,
				     uint16_t len, uint16_t offset)
{
	struct sensor_status_value value = {
		.mode = (uint8_t)operating_mode,
		.sensor_kind = (uint8_t)sensor_kind,
		.flags = (temp_valid ? STATUS_TEMP_VALID : 0) |
			 (hum_valid ? STATUS_HUM_VALID : 0) |
			 (mode_reboot_pending ? STATUS_REBOOT_PENDING : 0),
		.error = (uint8_t)sensor_error,
		.interval_ms_le = sys_cpu_to_le16((uint16_t)sample_period_ms()),
		.reserved_le = 0,
	};

	if (!sentry_auth_is_authenticated(conn)) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
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

	BT_GATT_CHARACTERISTIC(&sentry_mode_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
			       read_mode_cb, write_mode_cb, NULL),
	BT_GATT_CUD("Operating Mode (0 Debug, 1 I2C, 2 External)", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(&sentry_interval_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
			       read_interval_cb, write_interval_cb, NULL),
	BT_GATT_CUD("Sampling Interval Seconds (0 = 500 ms)", BT_GATT_PERM_READ),

	BT_GATT_CHARACTERISTIC(&sentry_status_uuid.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_sensor_status_cb, NULL, NULL),
	BT_GATT_CUD("Sensor Capability and Error Status", BT_GATT_PERM_READ),
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

static const char *sensor_kind_name(enum sensor_kind kind)
{
	switch (kind) {
	case SENSOR_SHT3X:
		return "SHT3x";
	case SENSOR_SHT4X_INTERNAL:
		return "Onboard SHT4x";
	case SENSOR_SHT4X_EXTERNAL:
		return "External SHT4x";
	case SENSOR_THERMISTOR:
		return "External 10k B3950 thermistor";
	default:
		return "No sensor";
	}
}

static int sensor_bus_write(uint16_t address, const uint8_t *data, size_t length)
{
	if (serial30_twim_active) {
		return serial30_twim_write(address, data, length);
	}
	return i2c_write(i2c_dev, data, length, address);
}

static int sensor_bus_read(uint16_t address, uint8_t *data, size_t length)
{
	if (serial30_twim_active) {
		return serial30_twim_read(address, data, length);
	}
	return i2c_read(i2c_dev, data, length, address);
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
		temp_valid = false;
		hum_valid = false;
		sensor_error = SENSOR_ERROR_NOT_FOUND;
		printk("Sensor unavailable; no measurement published\n");
		return false;
	}

	if (sensor_kind == SENSOR_THERMISTOR) {
		struct sensor_value temperature;

		ret = sensor_sample_fetch(ext_thermistor);
		if (ret == 0) {
			ret = sensor_channel_get(ext_thermistor,
						 SENSOR_CHAN_AMBIENT_TEMP, &temperature);
		}
		if (ret != 0) {
			temp_valid = false;
			hum_valid = false;
			sensor_error = SENSOR_ERROR_ADC;
			printk("External thermistor read failed: %d; no measurement published\n", ret);
			return false;
		}

		*out_temp_cdeg = (int16_t)(temperature.val1 * 100 +
						 temperature.val2 / 10000);
		temp_valid = true;
		hum_valid = false;
		sensor_error = SENSOR_ERROR_HUMIDITY_UNAVAILABLE;
		return true;
	}

	if (sensor_kind == SENSOR_SHT4X_INTERNAL ||
	    sensor_kind == SENSOR_SHT4X_EXTERNAL) {
		cmd = &sht4x_cmd;
		cmd_len = sizeof(sht4x_cmd);
	} else {
		cmd = sht3x_cmd;
		cmd_len = sizeof(sht3x_cmd);
	}

	ret = sensor_bus_write(sensor_addr, cmd, cmd_len);
	if (ret != 0) {
		printk("%s measurement command failed: %d; no measurement published\n",
		       sensor_kind_name(sensor_kind), ret);
		temp_valid = false;
		hum_valid = false;
		sensor_error = SENSOR_ERROR_BUS;
		return false;
	}

	k_msleep(20);

	ret = sensor_bus_read(sensor_addr, rx, sizeof(rx));
	if (ret != 0) {
		printk("%s read failed: %d; no measurement published\n",
		       sensor_kind_name(sensor_kind), ret);
		temp_valid = false;
		hum_valid = false;
		sensor_error = SENSOR_ERROR_BUS;
		return false;
	}

	temp_crc = sht3x_crc8(&rx[0], 2);
	hum_crc = sht3x_crc8(&rx[3], 2);

	if (temp_crc != rx[2]) {
		printk("%s temperature CRC failed; no measurement published\n",
		       sensor_kind_name(sensor_kind));
		temp_valid = false;
		hum_valid = false;
		sensor_error = SENSOR_ERROR_CRC;
		return false;
	}

	if (hum_crc != rx[5]) {
		printk("%s humidity CRC failed; no measurement published\n",
		       sensor_kind_name(sensor_kind));
		temp_valid = false;
		hum_valid = false;
		sensor_error = SENSOR_ERROR_CRC;
		return false;
	}

	raw_temp = ((uint16_t)rx[0] << 8) | rx[1];
	raw_hum = ((uint16_t)rx[3] << 8) | rx[4];

	temp_cdeg = -4500 + (int32_t)(((int64_t)17500 * raw_temp) / 65535);
	if (sensor_kind == SENSOR_SHT4X_INTERNAL ||
	    sensor_kind == SENSOR_SHT4X_EXTERNAL) {
		hum_cpercent = -600 + (int32_t)(((int64_t)12500 * raw_hum) / 65535);
		hum_cpercent = CLAMP(hum_cpercent, 0, 10000);
	} else {
		hum_cpercent = (int32_t)(((int64_t)10000 * raw_hum) / 65535);
	}

	*out_temp_cdeg = (int16_t)temp_cdeg;
	*out_hum_cpercent = (uint16_t)hum_cpercent;
	temp_valid = true;
	hum_valid = true;
	sensor_error = SENSOR_ERROR_OK;

	return true;
}

static void update_sensor_averages(void)
{
	int16_t temp_sample;
	uint16_t hum_sample = 0;

	int32_t temp_sum = 0;
	uint32_t hum_sum = 0;

	uint8_t temp_count;
	uint8_t hum_count;

	if (!read_sensor_sample(&temp_sample, &hum_sample)) {
		return;
	}

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

	if (hum_valid) {
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

		printk("%s temp: %d.%02d C, avg: %d.%02d C | humidity: %u.%02u %%RH, avg: %u.%02u %%RH\n",
		       sensor_kind_name(sensor_kind), temp_sample / 100,
		       temp_sample < 0 ? -(temp_sample % 100) : temp_sample % 100,
		       temp_avg_cdeg / 100,
		       temp_avg_cdeg < 0 ? -(temp_avg_cdeg % 100) : temp_avg_cdeg % 100,
		       hum_sample / 100, hum_sample % 100,
		       hum_avg_cpercent / 100, hum_avg_cpercent % 100);
	} else {
		printk("%s temp: %d.%02d C, avg: %d.%02d C | humidity unavailable\n",
		       sensor_kind_name(sensor_kind), temp_sample / 100,
		       temp_sample < 0 ? -(temp_sample % 100) : temp_sample % 100,
		       temp_avg_cdeg / 100,
		       temp_avg_cdeg < 0 ? -(temp_avg_cdeg % 100) : temp_avg_cdeg % 100);
	}
}

/* ---------------- BLE notify helpers ---------------- */

static void notify_temperature(void)
{
	int err;
	int16_t value_le;

	if (!temp_valid) {
		return;
	}
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

	if (!hum_valid) {
		return;
	}
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
	static const uint16_t dk_addresses[] = {0x44, 0x45};
	const uint8_t sht3x_cmd[2] = {0x24, 0x00};
	const uint8_t sht4x_cmd = 0xFD;
	uint8_t rx[6];
	int ret;

	sensor_ready = false;
	sensor_kind = SENSOR_NONE;
	sensor_error = SENSOR_ERROR_NOT_FOUND;
	temp_valid = false;
	hum_valid = false;
	serial30_twim_active = false;

	/* Local helper implemented as a macro to keep all probing on the selected
	 * backend. A valid CRC is required; an ACK by itself is not detection. */
#define PROBE_SELECTED(_kind, _address, _cmd, _cmd_len) \
	(sensor_bus_write((_address), (_cmd), (_cmd_len)) == 0 && \
	 (k_msleep(20), sensor_bus_read((_address), rx, sizeof(rx))) == 0 && \
	 sht3x_crc8(&rx[0], 2) == rx[2] && sht3x_crc8(&rx[3], 2) == rx[5])

	if (operating_mode == MODE_DEBUG) {
		if (device_is_ready(prod_i2c)) {
			i2c_dev = prod_i2c;
			(void)i2c_recover_bus(i2c_dev);
			if (PROBE_SELECTED(SENSOR_SHT4X_INTERNAL, 0x44,
					   &sht4x_cmd, sizeof(sht4x_cmd))) {
				sensor_kind = SENSOR_SHT4X_INTERNAL;
				sensor_addr = 0x44;
				sensor_ready = true;
			}
		}
	} else if (operating_mode == MODE_NORMAL_I2C) {
		ret = serial30_twim_init();
		if (ret == 0) {
			serial30_twim_active = true;
			if (PROBE_SELECTED(SENSOR_SHT4X_INTERNAL, 0x44,
					   &sht4x_cmd, sizeof(sht4x_cmd))) {
				sensor_kind = SENSOR_SHT4X_INTERNAL;
				sensor_addr = 0x44;
				sensor_ready = true;
			}
		}
	} else {
		if (device_is_ready(ext_i2c)) {
			i2c_dev = ext_i2c;
			(void)i2c_recover_bus(i2c_dev);
			if (PROBE_SELECTED(SENSOR_SHT4X_EXTERNAL, 0x44,
					   &sht4x_cmd, sizeof(sht4x_cmd))) {
				sensor_kind = SENSOR_SHT4X_EXTERNAL;
				sensor_addr = 0x44;
				sensor_ready = true;
			}
		}

		if (!sensor_ready && device_is_ready(ext_thermistor)) {
			struct sensor_value temperature;

			ret = sensor_sample_fetch(ext_thermistor);
			if (ret == 0) {
				ret = sensor_channel_get(ext_thermistor,
							 SENSOR_CHAN_AMBIENT_TEMP,
							 &temperature);
			}
			if (ret == 0) {
				sensor_kind = SENSOR_THERMISTOR;
				sensor_ready = true;
				sensor_error = SENSOR_ERROR_HUMIDITY_UNAVAILABLE;
			}
		}
	}

	/* The DK's onboard SHT3x remains supported by the same image. It is only
	 * considered for onboard modes, never as an external auto-detect result. */
	if (!sensor_ready && operating_mode != MODE_EXTERNAL_AUTO &&
	    device_is_ready(dk_i2c)) {
		serial30_twim_active = false;
		i2c_dev = dk_i2c;
		(void)i2c_recover_bus(i2c_dev);
		for (size_t i = 0; i < ARRAY_SIZE(dk_addresses); i++) {
			if (PROBE_SELECTED(SENSOR_SHT3X, dk_addresses[i],
					   sht3x_cmd, sizeof(sht3x_cmd))) {
				sensor_kind = SENSOR_SHT3X;
				sensor_addr = dk_addresses[i];
				sensor_ready = true;
				break;
			}
		}
	}

#undef PROBE_SELECTED

	if (sensor_ready) {
		printk("Detected %s; measurements will be published only when valid\n",
		       sensor_kind_name(sensor_kind));
	} else {
		printk("No sensor valid for mode %s; BLE status reports unavailable\n",
		       operating_mode_name(operating_mode));
	}
}

/* ---------------- main ---------------- */

int main(void)
{
	int ret;
	int64_t last_sample_ms;
	int64_t last_maint_button_ms = 0;

	/* Load just our mode before touching SERIAL30. The complete settings load
	 * remains after bt_enable(), once Bluetooth has registered its handlers. */
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		ret = settings_subsys_init();
		if (ret == 0) {
			ret = settings_load_subtree("ss1");
		}
		if (ret != 0) {
			operating_mode = MODE_DEBUG;
			sample_interval_seconds = 0;
		}
	}
	debug_uart_force_console();
	printk("\n\nMAIN STARTED - SS1 firmware %s\n", APP_VERSION_STRING);
	printk("Mode: %s; interval: %u ms\n",
	       operating_mode_name(operating_mode), sample_period_ms());

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
	update_sensor_averages();

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

	last_sample_ms = k_uptime_get() - sample_period_ms();

	while (1) {
		int64_t now = k_uptime_get();

		if ((now - last_sample_ms) >= sample_period_ms()) {
			last_sample_ms = now;
			update_sensor_averages();
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
