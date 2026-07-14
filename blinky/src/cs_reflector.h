/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth Channel Sounding - Ranging Responder (reflector) role.
 *
 * Lets a connected, encrypted peer (e.g. Blonky/SS2) run CS ranging against
 * this device and read back its own distance estimate. The Ranging Service
 * GATT server itself is provided automatically by the BT_RAS_RRSP library
 * once CONFIG_BT_RAS_RRSP=y - this module only reacts to the CS connection
 * callbacks needed to accept the peer's ranging configuration.
 */

#ifndef CS_REFLECTOR_H_
#define CS_REFLECTOR_H_

#include <zephyr/bluetooth/conn.h>

/* Call after a connection reaches BT_SECURITY_L2 and passes certificate auth.
 * Enables the reflector role for that connection; the peer drives the rest
 * of the CS setup.
 * Compiles to a no-op when Channel Sounding is disabled.
 */
#ifdef CONFIG_BT_RAS_RRSP
void cs_reflector_conn_ready(struct bt_conn *conn);
#else
static inline void cs_reflector_conn_ready(struct bt_conn *conn)
{
	(void)conn;
}
#endif

#endif /* CS_REFLECTOR_H_ */
