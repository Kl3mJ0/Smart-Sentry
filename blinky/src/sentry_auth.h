/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart Sentry certificate-based device pre-approval.
 *
 * Layered on top of the existing LESC-encrypted link:
 *   - LESC (Just Works) gives you an ENCRYPTED transport (unchanged).
 *   - This module adds an application-layer AUTHENTICATION step over that
 *     encrypted link: the client must present a certificate signed by the
 *     company root CA, and prove it holds the matching private key by
 *     signing a per-connection random nonce.
 *
 * Sensitive characteristics stay locked until sentry_auth_is_authenticated()
 * returns true for that connection.
 */

#ifndef SENTRY_AUTH_H_
#define SENTRY_AUTH_H_

#include <stdbool.h>
#include <zephyr/bluetooth/conn.h>

/* Wire format - MUST stay byte-for-byte identical to protocol.py */
#define SENTRY_CERT_VERSION     1
#define SENTRY_CERT_BODY_LEN    76   /* version(1)+rsv(3)+id(4)+not_after(4)+pubkey(64) */
#define SENTRY_SIG_LEN          64   /* raw r||s, P-256 */
#define SENTRY_CERT_TOTAL_LEN   (SENTRY_CERT_BODY_LEN + SENTRY_SIG_LEN)   /* 140 */
#define SENTRY_NONCE_LEN        32
#define SENTRY_RESPONSE_LEN     (SENTRY_CERT_TOTAL_LEN + SENTRY_SIG_LEN)  /* 204 */

/* Offsets inside the cert body */
#define SENTRY_CERT_OFF_ID        4
#define SENTRY_CERT_OFF_NOTAFTER  8
#define SENTRY_CERT_OFF_PUBKEY    12

/* Seconds a fresh connection has to authenticate before it is dropped. */
#define SENTRY_AUTH_TIMEOUT_MS    10000

/* Call once after bt_enable(). Initialises PSA crypto. Returns 0 on success. */
int sentry_auth_init(void);

/* Call from your connected() callback (after the link is up). */
void sentry_auth_conn_added(struct bt_conn *conn);

/* Call from your disconnected() callback. */
void sentry_auth_conn_removed(struct bt_conn *conn);

/* Gate check for sensitive reads/writes/notifies. */
bool sentry_auth_is_authenticated(struct bt_conn *conn);

#endif /* SENTRY_AUTH_H_ */
