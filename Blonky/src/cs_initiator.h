/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth Channel Sounding - Ranging Requestor (initiator) role.
 *
 * Runs CS ranging against a peer that has the Ranging Service (reflector
 * role, e.g. blinky/SS1) and prints the estimated distance as it updates.
 * Fully callback-driven - no blocking calls - so it can run alongside the
 * existing auth-handshake and ESS-subscribe flow.
 */

#ifndef CS_INITIATOR_H_
#define CS_INITIATOR_H_

#include <zephyr/bluetooth/conn.h>

/* Call once the existing auth + sensor-subscribe flow has finished with
 * this connection (i.e. at the end of ess_dm_completed()). Starts Ranging
 * Service discovery and the CS setup sequence.
 * Compiles to a no-op when Channel Sounding is disabled.
 */
#ifdef CONFIG_BT_RAS_RREQ
void cs_initiator_start(struct bt_conn *conn);
#else
static inline void cs_initiator_start(struct bt_conn *conn)
{
	(void)conn;
}
#endif

#endif /* CS_INITIATOR_H_ */
