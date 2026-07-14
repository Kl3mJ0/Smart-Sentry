/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Auxiliary UART (uart30) - wired sensor-bridge packet output.
 *
 * Independent, second UART instance on P0.00(TX)/P0.01(RX)/P0.02(RTS)/
 * P0.03(CTS) - physically separate from the uart20 console and unrelated
 * to the BLE link. Feeds a TTL serial converter, not SS2.
 *
 * Implements the reference "sensor bridge unit" packet format:
 *   m <PAN 4ch> <unit ID 2ch> d <data> r -<RSSI 3ch> s <seq 3ch> CR
 * with per-reading messages:
 *   Temperature: "TSSA" <sign> <tens><units> '.' <decimal>       (9 chars)
 *   Humidity:    "HSSA" <tens><units> '.' <decimal> <pad '-'>    (9 chars)
 * One packet per value: a temperature reading and a humidity reading are
 * sent as two separate packets, each with its own sequence number. RSSI is
 * always "000" (wired, no radio). The sequence counter is a rolling 1-byte
 * counter, shared across both packet types; on wraparound (255 -> 0) a
 * status message ("SS361#H1W", hardware char 'W' = wired sensor) is sent
 * using the wrapped value before normal data resumes.
 *
 * PAN ID / unit ID are derived from this device's real BLE identity
 * address (via bt_id_get(), which always returns the genuine, fixed
 * address - not a rotating LE Privacy/RPA address): the first 4 hex
 * characters of the MAC (in conventional MSB-first display order) become
 * the PAN ID, the next 2 become the unit ID.
 */

#ifndef AUX_UART_H_
#define AUX_UART_H_

#include <stdint.h>

/* Call once from main(), alongside sht3x_init_check(). Readies the uart30
 * device only; does not touch BLE.
 */
void aux_uart_init_check(void);

/* Call once from main(), after bt_enable() and settings_load() have both
 * completed, so the real identity address (and any settings-restored
 * address) is available. Derives the PAN ID / unit ID from it.
 */
void aux_uart_load_identity(void);

/* Send two packets, temperature then humidity. No-op if uart30 isn't
 * ready. temp_cdeg: centi-degrees C (2534 = 25.34 C). hum_cpercent:
 * centi-%RH.
 */
void aux_uart_send_temp_humidity(int16_t temp_cdeg, uint16_t hum_cpercent);

#endif /* AUX_UART_H_ */
