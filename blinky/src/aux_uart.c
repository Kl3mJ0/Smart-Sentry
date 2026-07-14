/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>

#include "aux_uart.h"

#define AUX_UART_NODE DT_NODELABEL(uart30)

#define AUX_PAN_LEN  4
#define AUX_ID_LEN   2
#define AUX_MSG_LEN  9   /* both TSSA... and HSSA... messages are 9 chars */

static const struct device *aux_uart_dev = DEVICE_DT_GET(AUX_UART_NODE);
static bool aux_uart_ready;

static char aux_pan_id[AUX_PAN_LEN + 1] = "0000";
static char aux_unit_id[AUX_ID_LEN + 1] = "00";

static uint8_t aux_seq;

static const char aux_status_msg[AUX_MSG_LEN] = {
	'S', 'S', '3', '6', '1', '#', 'H', '1', 'W',
};

static void aux_uart_write(const char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(aux_uart_dev, buf[i]);
	}
}

void aux_uart_init_check(void)
{
	if (!device_is_ready(aux_uart_dev)) {
		aux_uart_ready = false;
		printk("AUX UART (uart30) is not ready. Packet output disabled.\n");
		return;
	}

	aux_uart_ready = true;

	printk("AUX UART ready: %s (P0.00 TX / P0.01 RX / P0.02 RTS / P0.03 CTS)\n",
	       aux_uart_dev->name);
}

void aux_uart_load_identity(void)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	char hex[2 * 6 + 1];

	bt_id_get(addrs, &count);

	if (count == 0) {
		printk("AUX UART: no BT identity address yet - keeping placeholder PAN=%s ID=%s\n",
		       aux_pan_id, aux_unit_id);
		return;
	}

	/*
	 * bt_addr_le_t stores the 6-byte MAC little-endian (val[0] = LSB).
	 * Rebuild it MSB-first (the order a MAC is conventionally written
	 * in, e.g. as bt_addr_le_to_str prints it) before slicing hex
	 * characters out of it. This is the genuine identity address -
	 * bt_id_get() returns it even when LE Privacy is enabled and a
	 * rotating RPA is what's actually broadcast over the air.
	 */
	snprintk(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
		 addrs[0].a.val[5], addrs[0].a.val[4], addrs[0].a.val[3],
		 addrs[0].a.val[2], addrs[0].a.val[1], addrs[0].a.val[0]);

	memcpy(aux_pan_id, hex, AUX_PAN_LEN);
	aux_pan_id[AUX_PAN_LEN] = '\0';
	memcpy(aux_unit_id, hex + AUX_PAN_LEN, AUX_ID_LEN);
	aux_unit_id[AUX_ID_LEN] = '\0';

	printk("AUX UART packet identity: PAN=%s ID=%s (from BT MAC %s)\n",
	       aux_pan_id, aux_unit_id, hex);
}

/* "TSSA" <sign> <tens><units> '.' <decimal> -> 9 chars, no NUL. */
static void format_temp_msg(char out[AUX_MSG_LEN], int16_t temp_cdeg)
{
	int32_t temp_deci = (temp_cdeg >= 0) ? (temp_cdeg + 5) / 10 : (temp_cdeg - 5) / 10;
	char sign = (temp_deci < 0) ? '-' : '+';
	int32_t whole = (temp_deci < 0) ? -temp_deci : temp_deci;
	int32_t frac = whole % 10;

	whole /= 10;
	if (whole > 99) {
		whole = 99; /* saturate: format is fixed at 2 digits */
	}

	snprintk(out, AUX_MSG_LEN + 1, "TSSA%c%02d.%d", sign, (int)whole, (int)frac);
}

/* "HSSA" <tens><units> '.' <decimal> <pad '-'> -> 9 chars, no NUL, no sign. */
static void format_hum_msg(char out[AUX_MSG_LEN], uint16_t hum_cpercent)
{
	uint32_t deci = (hum_cpercent + 5) / 10;
	int32_t frac = deci % 10;
	int32_t whole = deci / 10;
	int len;

	if (whole > 99) {
		whole = 99;
	}

	len = snprintk(out, AUX_MSG_LEN, "HSSA%02d.%d", (int)whole, (int)frac);
	while (len < AUX_MSG_LEN) {
		out[len++] = '-';
	}
}

/*
 * m <PAN><ID> d <data...> r -<RSSI 000> s <seq 3 digits> CR
 * CR (0x0D) only - no trailing LF, per the reference packet format.
 */
/* Envelope overhead (everything except the data section):
 * 'm' + PAN(4) + ID(2) + 'd' + 'r' + '-' + RSSI(3) + 's' + seq(3) + CR(1) = 19
 */
#define AUX_PACKET_OVERHEAD 19

static void send_packet(const char *data, size_t data_len)
{
	char pkt[AUX_PACKET_OVERHEAD + AUX_MSG_LEN];
	size_t len = 0;

	pkt[len++] = 'm';
	memcpy(&pkt[len], aux_pan_id, AUX_PAN_LEN);
	len += AUX_PAN_LEN;
	memcpy(&pkt[len], aux_unit_id, AUX_ID_LEN);
	len += AUX_ID_LEN;
	pkt[len++] = 'd';
	memcpy(&pkt[len], data, data_len);
	len += data_len;
	pkt[len++] = 'r';
	pkt[len++] = '-';
	memcpy(&pkt[len], "000", 3);
	len += 3;
	pkt[len++] = 's';
	pkt[len++] = '0' + (aux_seq / 100) % 10;
	pkt[len++] = '0' + (aux_seq / 10) % 10;
	pkt[len++] = '0' + (aux_seq % 10);
	pkt[len++] = '\r';

	aux_uart_write(pkt, len);
}

static void advance_sequence(void)
{
	aux_seq++;

	if (aux_seq == 0) {
		/* Rolled over 255 -> 0: send the status message using the
		 * wrapped value, then move past it before normal data resumes.
		 */
		send_packet(aux_status_msg, AUX_MSG_LEN);
		aux_seq++;
	}
}

void aux_uart_send_temp_humidity(int16_t temp_cdeg, uint16_t hum_cpercent)
{
	char msg[AUX_MSG_LEN];

	if (!aux_uart_ready) {
		return;
	}

	/* One packet per value - each is its own transmission and consumes
	 * its own sequence number. */
	format_temp_msg(msg, temp_cdeg);
	send_packet(msg, AUX_MSG_LEN);
	advance_sequence();

	format_hum_msg(msg, hum_cpercent);
	send_packet(msg, AUX_MSG_LEN);
	advance_sequence();
}
