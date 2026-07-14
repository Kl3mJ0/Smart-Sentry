/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from Nordic's channel_sounding_ras_initiator sample
 * (C:\ncs\v3.4.0\nrf\samples\bluetooth\channel_sounding\ras_initiator),
 * restructured to be fully callback-driven (no blocking k_sem_take chain
 * in main()) so it can run after Blonky's existing auth+ESS flow instead
 * of standing alone.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/cs.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/ras.h>
#include <bluetooth/cs_de.h>

#include "cs_initiator.h"

#define CS_CONFIG_ID     0
/* Mode 2 (phase-based) with interleaved mode-1 (RTT) sub-mode steps.
 *
 * Mode 3 was tried and reverted: on this SDC version it periodically
 * desyncs the local/peer step streams ("Mismatch of local and peer step
 * mode" warnings from ras_rreq) and the corrupted procedures poison the
 * distance window. It also bought nothing: this hardware only supports
 * AA-only RTT (no sounding sequence), whose per-connection sync bias
 * (several ns, sign and size varying per session) swamps the true
 * time-of-flight at room scale - so rtt stays clamped at 0 regardless.
 * The ifft estimate is the usable distance readout on this hardware. */
#define CS_CONFIG_MODE   BT_CONN_LE_CS_MAIN_MODE_2_SUB_MODE_1

#define NUM_MODE_0_STEPS       3
#define DE_SLIDING_WINDOW_SIZE 9
#define MAX_AP                 (CONFIG_BT_RAS_MAX_ANTENNA_PATHS)
#define CHANNEL_INDEX_OFFSET   2
#define TONE_QI_OK_TONE_COUNT_THRESHOLD 15

#define LOCAL_PROCEDURE_MEM                                                                \
	((BT_RAS_MAX_STEPS_PER_PROCEDURE * sizeof(struct bt_le_cs_subevent_step)) +        \
	 (BT_RAS_MAX_STEPS_PER_PROCEDURE * BT_RAS_MAX_STEP_DATA_LEN))

static K_MUTEX_DEFINE(distance_estimate_buffer_mutex);
static K_SEM_DEFINE(sem_local_steps, 1, 1);

NET_BUF_SIMPLE_DEFINE_STATIC(latest_local_steps, LOCAL_PROCEDURE_MEM);
NET_BUF_SIMPLE_DEFINE_STATIC(latest_peer_steps, BT_RAS_PROCEDURE_MEM);

static int32_t most_recent_local_ranging_counter = -1;
static int32_t dropped_ranging_counter = -1;
static uint32_t ras_feature_bits;

static struct bt_conn_le_cs_config cs_config;

/* AA-only RTT has a coarse, uncompensated group-delay bias - fall back to it
 * only if the peer doesn't report sounding-sequence RTT support. */
static enum bt_conn_le_cs_rtt_type negotiated_rtt_type = BT_CONN_LE_CS_RTT_TYPE_AA_ONLY;

struct distance_estimate_buffer {
	cs_de_dist_estimates_t estimates[DE_SLIDING_WINDOW_SIZE];
	uint8_t num_valid;
	uint8_t index;
};

static struct distance_estimate_buffer distance_estimate_buffers[MAX_AP];

static uint16_t m_n_iqs[CONFIG_BT_RAS_MAX_ANTENNA_PATHS][CS_DE_NUM_CHANNELS];
static cs_de_report_t m_cs_de_report;

/* ---------------- distance-estimation helpers (ported from the sample) ---------------- */

static void store_distance_estimates_in_buffer(cs_de_dist_estimates_t *p_estimates,
					       struct distance_estimate_buffer *buffer)
{
	k_mutex_lock(&distance_estimate_buffer_mutex, K_FOREVER);

	memcpy(&buffer->estimates[buffer->index], p_estimates, sizeof(cs_de_dist_estimates_t));
	buffer->index = (buffer->index + 1) % DE_SLIDING_WINDOW_SIZE;
	if (buffer->num_valid < DE_SLIDING_WINDOW_SIZE) {
		buffer->num_valid++;
	}

	k_mutex_unlock(&distance_estimate_buffer_mutex);
}

static int float_cmp(const void *a, const void *b)
{
	float fa = *(const float *)a;
	float fb = *(const float *)b;

	return (fa > fb) - (fa < fb);
}

static float median_inplace(int count, float *values)
{
	if (count == 0) {
		return NAN;
	}

	qsort(values, count, sizeof(float), float_cmp);

	if (count % 2 == 0) {
		return (values[count / 2] + values[count / 2 - 1]) / 2;
	}
		return values[count / 2];
}

static cs_de_dist_estimates_t get_distance(uint8_t ap)
{
	cs_de_dist_estimates_t averaged_result = {0};
	uint8_t num_ifft = 0;
	uint8_t num_phase_slope = 0;
	uint8_t num_rtt = 0;

	static float temp_ifft[DE_SLIDING_WINDOW_SIZE];
	static float temp_phase_slope[DE_SLIDING_WINDOW_SIZE];
	static float temp_rtt[DE_SLIDING_WINDOW_SIZE];

	struct distance_estimate_buffer *buffer = &distance_estimate_buffers[ap];

	k_mutex_lock(&distance_estimate_buffer_mutex, K_FOREVER);

	for (uint8_t i = 0; i < buffer->num_valid; i++) {
		if (isfinite(buffer->estimates[i].ifft)) {
			temp_ifft[num_ifft++] = buffer->estimates[i].ifft;
		}
		if (isfinite(buffer->estimates[i].phase_slope)) {
			temp_phase_slope[num_phase_slope++] = buffer->estimates[i].phase_slope;
		}
		if (isfinite(buffer->estimates[i].rtt)) {
			temp_rtt[num_rtt++] = buffer->estimates[i].rtt;
		}
	}

	k_mutex_unlock(&distance_estimate_buffer_mutex);

	averaged_result.ifft = median_inplace(num_ifft, temp_ifft);
	averaged_result.phase_slope = median_inplace(num_phase_slope, temp_phase_slope);
	averaged_result.rtt = median_inplace(num_rtt, temp_rtt);

	return averaged_result;
}

static bool tone_quality_ok(uint16_t num_iqs[CS_DE_NUM_CHANNELS], uint8_t channel_map[10])
{
	uint8_t ok_tones_count = 0;

	for (uint8_t i = 0; i < CS_DE_NUM_CHANNELS; ++i) {
		if (BT_LE_CS_CHANNEL_BIT_GET(channel_map, i + CHANNEL_INDEX_OFFSET) &&
		    num_iqs[i] >= 1) {
			ok_tones_count++;
		}
	}
	return ok_tones_count >= TONE_QI_OK_TONE_COUNT_THRESHOLD;
}

static void cumulate_mean(float *avg, float new_value, uint16_t *n)
{
	float a = 1.0f / (*n);
	float b = 1.0f - a;

	*avg = a * new_value + b * (*avg);
}

static void extract_pcts(cs_de_report_t *p_report, uint8_t channel_index,
			 uint8_t antenna_permutation_index,
			 struct bt_hci_le_cs_step_data_tone_info *local_tone_info,
			 struct bt_hci_le_cs_step_data_tone_info *remote_tone_info)
{
	for (uint8_t tone_index = 0; tone_index < p_report->n_ap; tone_index++) {
		int antenna_path = bt_le_cs_get_antenna_path(p_report->n_ap,
							     antenna_permutation_index, tone_index);
		if (antenna_path < 0) {
			printk("CS initiator: invalid antenna path\n");
			return;
		}

		if (local_tone_info[tone_index].quality_indicator !=
			    BT_HCI_LE_CS_TONE_QUALITY_HIGH ||
		    remote_tone_info[tone_index].quality_indicator !=
			    BT_HCI_LE_CS_TONE_QUALITY_HIGH) {
			return;
		}

		struct bt_le_cs_iq_sample local_iq =
			bt_le_cs_parse_pct(local_tone_info[tone_index].phase_correction_term);
		struct bt_le_cs_iq_sample remote_iq =
			bt_le_cs_parse_pct(remote_tone_info[tone_index].phase_correction_term);

		m_n_iqs[antenna_path][channel_index]++;

		if (m_n_iqs[antenna_path][channel_index] == 1) {
			p_report->iq_tones[antenna_path].i_local[channel_index] = local_iq.i;
			p_report->iq_tones[antenna_path].q_local[channel_index] = local_iq.q;
			p_report->iq_tones[antenna_path].i_remote[channel_index] = remote_iq.i;
			p_report->iq_tones[antenna_path].q_remote[channel_index] = remote_iq.q;
		} else {
			cumulate_mean(&p_report->iq_tones[antenna_path].i_local[channel_index],
				     local_iq.i, &m_n_iqs[antenna_path][channel_index]);
			cumulate_mean(&p_report->iq_tones[antenna_path].q_local[channel_index],
				     local_iq.q, &m_n_iqs[antenna_path][channel_index]);
			cumulate_mean(&p_report->iq_tones[antenna_path].i_remote[channel_index],
				     remote_iq.i, &m_n_iqs[antenna_path][channel_index]);
			cumulate_mean(&p_report->iq_tones[antenna_path].q_remote[channel_index],
				     remote_iq.q, &m_n_iqs[antenna_path][channel_index]);
		}
	}
}

static void extract_rtt_timings(cs_de_report_t *p_report,
				struct bt_hci_le_cs_step_data_mode_1 *local_rtt_data,
				struct bt_hci_le_cs_step_data_mode_1 *peer_rtt_data)
{
	if (local_rtt_data->packet_quality_aa_check !=
		    BT_HCI_LE_CS_PACKET_QUALITY_AA_CHECK_SUCCESSFUL ||
	    local_rtt_data->packet_rssi == BT_HCI_LE_CS_PACKET_RSSI_NOT_AVAILABLE ||
	    local_rtt_data->tod_toa_reflector == BT_HCI_LE_CS_TIME_DIFFERENCE_NOT_AVAILABLE ||
	    peer_rtt_data->packet_quality_aa_check !=
		    BT_HCI_LE_CS_PACKET_QUALITY_AA_CHECK_SUCCESSFUL ||
	    peer_rtt_data->packet_rssi == BT_HCI_LE_CS_PACKET_RSSI_NOT_AVAILABLE ||
	    peer_rtt_data->tod_toa_reflector == BT_HCI_LE_CS_TIME_DIFFERENCE_NOT_AVAILABLE) {
		return;
	}

	if (p_report->role == BT_CONN_LE_CS_ROLE_INITIATOR) {
		p_report->rtt_accumulated_half_ns +=
			local_rtt_data->toa_tod_initiator - peer_rtt_data->tod_toa_reflector;
	} else {
		p_report->rtt_accumulated_half_ns +=
			peer_rtt_data->toa_tod_initiator - local_rtt_data->tod_toa_reflector;
	}

	p_report->rtt_count++;
}

static bool process_ranging_header(struct ras_ranging_header *ranging_header, void *user_data)
{
	cs_de_report_t *p_report = (cs_de_report_t *)user_data;

	p_report->n_ap = MAX(1, ((ranging_header->antenna_paths_mask & BIT(0)) +
				 ((ranging_header->antenna_paths_mask & BIT(1)) >> 1) +
				 ((ranging_header->antenna_paths_mask & BIT(2)) >> 2) +
				 ((ranging_header->antenna_paths_mask & BIT(3)) >> 3)));
	return true;
}

static bool process_step_data(struct bt_le_cs_subevent_step *local_step,
			      struct bt_le_cs_subevent_step *peer_step, void *user_data)
{
	cs_de_report_t *p_report = (cs_de_report_t *)user_data;

	if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_2) {
		struct bt_hci_le_cs_step_data_mode_2 *local_step_data =
			(struct bt_hci_le_cs_step_data_mode_2 *)local_step->data;
		struct bt_hci_le_cs_step_data_mode_2 *peer_step_data =
			(struct bt_hci_le_cs_step_data_mode_2 *)peer_step->data;

		extract_pcts(p_report, local_step->channel - CHANNEL_INDEX_OFFSET,
			    local_step_data->antenna_permutation_index, local_step_data->tone_info,
			    peer_step_data->tone_info);
	} else if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_1) {
		struct bt_hci_le_cs_step_data_mode_1 *local_step_data =
			(struct bt_hci_le_cs_step_data_mode_1 *)local_step->data;
		struct bt_hci_le_cs_step_data_mode_1 *peer_step_data =
			(struct bt_hci_le_cs_step_data_mode_1 *)peer_step->data;

		extract_rtt_timings(p_report, local_step_data, peer_step_data);
	} else if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_3) {
		struct bt_hci_le_cs_step_data_mode_3 *local_step_data =
			(struct bt_hci_le_cs_step_data_mode_3 *)local_step->data;
		struct bt_hci_le_cs_step_data_mode_3 *peer_step_data =
			(struct bt_hci_le_cs_step_data_mode_3 *)peer_step->data;

		extract_pcts(p_report, local_step->channel - CHANNEL_INDEX_OFFSET,
			    local_step_data->antenna_permutation_index, local_step_data->tone_info,
			    peer_step_data->tone_info);
		extract_rtt_timings(p_report,
				    (struct bt_hci_le_cs_step_data_mode_1 *)local_step_data,
				    (struct bt_hci_le_cs_step_data_mode_1 *)peer_step_data);
	}

	return true;
}

/* Exponential moving average over the median-filtered ifft estimate.
 * The median window rejects outliers but does not average, so the
 * displayed value jitters when stationary. At ~3 estimates/s an alpha of
 * 0.15 gives a time constant of roughly 2 s: stationary readings settle,
 * movement still shows up within a couple of seconds.
 */
#define CS_DISPLAY_EMA_ALPHA 0.15f

static float smoothed_distance_m;
static bool smoothed_distance_valid;

static void distance_estimate_print(uint8_t ap)
{
	cs_de_dist_estimates_t d = get_distance(ap);

	if (isfinite(d.ifft)) {
		if (!smoothed_distance_valid) {
			smoothed_distance_m = d.ifft;
			smoothed_distance_valid = true;
		} else {
			smoothed_distance_m +=
				CS_DISPLAY_EMA_ALPHA * (d.ifft - smoothed_distance_m);
		}
	}

	/* ifft is the only trustworthy estimator on this hardware/environment:
	 * rtt carries a per-session clock-sync bias (no absolute reference),
	 * phase_slope is strongly multipath-biased indoors. Both kept as
	 * debug only.
	 */
	printk("SS1 distance: %.2f m   [debug ap%u: ifft_raw=%.2f phase_slope=%.2f rtt=%.2f]\n",
	       (double)smoothed_distance_m, ap, (double)d.ifft,
	       (double)d.phase_slope, (double)d.rtt);
}

static void ranging_data_cb(struct bt_conn *conn, uint16_t ranging_counter, int err)
{
	ARG_UNUSED(conn);

	if (err) {
		printk("CS initiator: error receiving ranging data (counter %u, err %d)\n",
		       ranging_counter, err);
		return;
	}

	if (ranging_counter != most_recent_local_ranging_counter) {
		net_buf_simple_reset(&latest_local_steps);
		k_sem_give(&sem_local_steps);
		return;
	}

	if (latest_local_steps.len == 0) {
		net_buf_simple_reset(&latest_local_steps);
		k_sem_give(&sem_local_steps);
		if (!(ras_feature_bits & RAS_FEAT_REALTIME_RD)) {
			net_buf_simple_reset(&latest_peer_steps);
		}
		return;
	}

	memset(&m_cs_de_report, 0, sizeof(m_cs_de_report));
	memset(m_n_iqs, 0, sizeof(m_n_iqs));
	/* This side is always the initiator - set explicitly rather than relying
	 * on BT_CONN_LE_CS_ROLE_INITIATOR happening to be the zeroed default. */
	m_cs_de_report.role = BT_CONN_LE_CS_ROLE_INITIATOR;

	bt_ras_rreq_rd_subevent_data_parse(&latest_peer_steps, &latest_local_steps, cs_config.role,
					   process_ranging_header, NULL, process_step_data,
					   &m_cs_de_report);

	net_buf_simple_reset(&latest_local_steps);
	if (!(ras_feature_bits & RAS_FEAT_REALTIME_RD)) {
		net_buf_simple_reset(&latest_peer_steps);
	}
	k_sem_give(&sem_local_steps);

	printk("CS initiator: procedure %u parsed, rtt_count=%u n_ap=%u rtt_accum_half_ns=%d\n",
	       ranging_counter, m_cs_de_report.rtt_count, m_cs_de_report.n_ap,
	       m_cs_de_report.rtt_accumulated_half_ns);

	for (uint8_t ap = 0; ap < m_cs_de_report.n_ap; ap++) {
		m_cs_de_report.distance_estimates[ap].ifft = NAN;
		m_cs_de_report.distance_estimates[ap].phase_slope = NAN;
		m_cs_de_report.distance_estimates[ap].rtt = NAN;
		m_cs_de_report.distance_estimates[ap].best = NAN;

		m_cs_de_report.tone_quality[ap] = tone_quality_ok(m_n_iqs[ap], cs_config.channel_map)
							   ? CS_DE_TONE_QUALITY_OK
							   : CS_DE_TONE_QUALITY_BAD;
	}

	if (cs_de_calc(&m_cs_de_report) == CS_DE_QUALITY_OK) {
		/* cs_de_rtt() clamps negative per-procedure results to exactly
		 * 0.0 BEFORE they reach the median window, which rectifies the
		 * noise: at short range roughly half the procedures land
		 * slightly negative, the window fills with hard zeros, and the
		 * median sticks at 0.00 even though the accumulators carry
		 * real signal. Recompute rtt unclamped from the raw
		 * accumulator (average first, no clamp) so noise cancels
		 * across the window instead; negatives are legitimate noise
		 * samples around a small positive truth.
		 */
		if (m_cs_de_report.rtt_count > 0) {
			float tof_ns = ((float)m_cs_de_report.rtt_accumulated_half_ns * 0.5f) /
				       (float)m_cs_de_report.rtt_count / 2.0f;

			m_cs_de_report.distance_estimates[0].rtt = tof_ns * 0.299792458f;
		}

		for (uint8_t ap = 0; ap < m_cs_de_report.n_ap; ap++) {
			if (m_cs_de_report.tone_quality[ap] == CS_DE_TONE_QUALITY_OK ||
			    isfinite(m_cs_de_report.distance_estimates[ap].rtt)) {
				store_distance_estimates_in_buffer(
					&m_cs_de_report.distance_estimates[ap],
					&distance_estimate_buffers[ap]);
				distance_estimate_print(ap);
			}
		}
	}
}

static void subevent_result_cb(struct bt_conn *conn, struct bt_conn_le_cs_subevent_result *result)
{
	ARG_UNUSED(conn);

	if (dropped_ranging_counter == result->header.procedure_counter) {
		return;
	}

	if (most_recent_local_ranging_counter !=
	    bt_ras_rreq_get_ranging_counter(result->header.procedure_counter)) {
		if (k_sem_take(&sem_local_steps, K_NO_WAIT) < 0) {
			dropped_ranging_counter = result->header.procedure_counter;
			return;
		}

		most_recent_local_ranging_counter =
			bt_ras_rreq_get_ranging_counter(result->header.procedure_counter);
	}

	if (result->header.subevent_done_status == BT_CONN_LE_CS_SUBEVENT_ABORTED) {
		/* steps from this subevent are discarded */
	} else if (result->step_data_buf) {
		if (result->step_data_buf->len <= net_buf_simple_tailroom(&latest_local_steps)) {
			uint16_t len = result->step_data_buf->len;
			uint8_t *step_data = net_buf_simple_pull_mem(result->step_data_buf, len);

			net_buf_simple_add_mem(&latest_local_steps, step_data, len);
		} else {
			printk("CS initiator: not enough memory to store step data\n");
			net_buf_simple_reset(&latest_local_steps);
			dropped_ranging_counter = result->header.procedure_counter;
			return;
		}
	}

	dropped_ranging_counter = -1;

	if (result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE) {
		most_recent_local_ranging_counter =
			bt_ras_rreq_get_ranging_counter(result->header.procedure_counter);
	} else if (result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_ABORTED) {
		net_buf_simple_reset(&latest_local_steps);
		k_sem_give(&sem_local_steps);
	}
}

static void ranging_data_ready_cb(struct bt_conn *conn, uint16_t ranging_counter)
{
	if (ranging_counter == most_recent_local_ranging_counter) {
		int err = bt_ras_rreq_cp_get_ranging_data(conn, &latest_peer_steps,
							  ranging_counter, ranging_data_cb);
		if (err) {
			printk("CS initiator: get ranging data failed (err %d)\n", err);
			net_buf_simple_reset(&latest_local_steps);
			net_buf_simple_reset(&latest_peer_steps);
			k_sem_give(&sem_local_steps);
		}
	}
}

static void ranging_data_overwritten_cb(struct bt_conn *conn, uint16_t ranging_counter)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ranging_counter);
}

/* ---------------- CS setup sequence (event-driven) ---------------- */

static void cs_config_get(struct bt_le_cs_create_config_params *config_params)
{
	memset(config_params, 0, sizeof(*config_params));
	config_params->id = CS_CONFIG_ID;
	config_params->mode = CS_CONFIG_MODE;
	config_params->min_main_mode_steps = 2;
	config_params->max_main_mode_steps = 10;
	config_params->main_mode_repetition = 0;
	config_params->mode_0_steps = NUM_MODE_0_STEPS;
	config_params->role = BT_CONN_LE_CS_ROLE_INITIATOR;
	config_params->rtt_type = negotiated_rtt_type;
	config_params->cs_sync_phy = BT_CONN_LE_CS_SYNC_1M_PHY;
	config_params->channel_map_repetition = 1;
	config_params->channel_selection_type = BT_CONN_LE_CS_CHSEL_TYPE_3B;
	config_params->ch3c_shape = BT_CONN_LE_CS_CH3C_SHAPE_HAT;
	config_params->ch3c_jump = 2;

	/* Dense contiguous channel block (26..61), per the Zephyr connected_cs
	 * demo. This exact map measured best in practice (6-13 cm error at
	 * 60 cm face-to-face). Wider/full maps were tried and regressed:
	 * gaps or uneven per-channel coverage distort the IFFT peak more than
	 * the extra aperture helps.
	 */
	memset(config_params->channel_map, 0, sizeof(config_params->channel_map));
	for (uint8_t ch = 26; ch < 62; ch++) {
		BT_LE_CS_CHANNEL_BIT_SET_VAL(config_params->channel_map, ch, 1);
	}
}

static void ras_features_read_cb(struct bt_conn *conn, uint32_t feature_bits, int err)
{
	int rc;

	if (err) {
		printk("CS initiator: reading RAS features failed (err %d)\n", err);
		return;
	}

	ras_feature_bits = feature_bits;

	if (feature_bits & RAS_FEAT_REALTIME_RD) {
		rc = bt_ras_rreq_realtime_rd_subscribe(conn, &latest_peer_steps, ranging_data_cb);
		if (rc) {
			printk("CS initiator: realtime RD subscribe failed (err %d)\n", rc);
			return;
		}
	} else {
		rc = bt_ras_rreq_rd_overwritten_subscribe(conn, ranging_data_overwritten_cb);
		if (rc) {
			printk("CS initiator: RD overwritten subscribe failed (err %d)\n", rc);
			return;
		}
		rc = bt_ras_rreq_rd_ready_subscribe(conn, ranging_data_ready_cb);
		if (rc) {
			printk("CS initiator: RD ready subscribe failed (err %d)\n", rc);
			return;
		}
		rc = bt_ras_rreq_on_demand_rd_subscribe(conn);
		if (rc) {
			printk("CS initiator: on-demand RD subscribe failed (err %d)\n", rc);
			return;
		}
		rc = bt_ras_rreq_cp_subscribe(conn);
		if (rc) {
			printk("CS initiator: CP subscribe failed (err %d)\n", rc);
			return;
		}
	}

	rc = bt_le_cs_read_remote_supported_capabilities(conn);
	if (rc) {
		printk("CS initiator: failed to read remote CS capabilities (err %d)\n", rc);
	}
}

static void remote_capabilities_cb(struct bt_conn *conn, uint8_t status,
				   struct bt_conn_le_cs_capabilities *params)
{
	struct bt_le_cs_create_config_params config_params;
	int err;

	if (status != BT_HCI_ERR_SUCCESS) {
		printk("CS initiator: capability exchange failed (status 0x%02x)\n", status);
		return;
	}

	/* Preference order by timing-correlation quality: sounding sequence
	 * (not supported by this SDC per the v3.4.0 nrfxlib CS docs), then
	 * random payload (supported, 32-128 bit), then AA-only (supported but
	 * poor - single 32-bit correlation point, uncompensated bias). */
	if (params->rtt_sounding_precision != BT_CONN_LE_CS_RTT_SOUNDING_NOT_SUPP) {
		negotiated_rtt_type = BT_CONN_LE_CS_RTT_TYPE_32_BIT_SOUNDING;
		printk("CS initiator: using 32-bit sounding-sequence RTT (peer precision tier %d)\n",
		       params->rtt_sounding_precision);
	} else if (params->rtt_random_payload_precision !=
		   BT_CONN_LE_CS_RTT_RANDOM_PAYLOAD_NOT_SUPP) {
		negotiated_rtt_type = BT_CONN_LE_CS_RTT_TYPE_128_BIT_RANDOM;
		printk("CS initiator: using 128-bit random-payload RTT (peer precision tier %d)\n",
		       params->rtt_random_payload_precision);
	} else {
		negotiated_rtt_type = BT_CONN_LE_CS_RTT_TYPE_AA_ONLY;
		printk("CS initiator: peer lacks sounding/random RTT, falling back to AA-only\n");
	}

	cs_config_get(&config_params);

	err = bt_le_cs_create_config(conn, &config_params,
				     BT_LE_CS_CREATE_CONFIG_CONTEXT_LOCAL_AND_REMOTE);
	if (err) {
		printk("CS initiator: failed to create CS config (err %d)\n", err);
	}
}

static void config_create_cb(struct bt_conn *conn, uint8_t status,
			     struct bt_conn_le_cs_config *config)
{
	int err;

	if (status != BT_HCI_ERR_SUCCESS) {
		printk("CS initiator: config creation failed (status 0x%02x)\n", status);
		return;
	}

	cs_config = *config;

	err = bt_le_cs_security_enable(conn);
	if (err) {
		printk("CS initiator: failed to start CS security (err %d)\n", err);
	}
}

static void security_enable_cb(struct bt_conn *conn, uint8_t status)
{
	struct bt_conn_info info;
	uint16_t acl_interval_in_proc_interval_units;
	uint16_t desired_procedure_interval;
	int err;

	if (status != BT_HCI_ERR_SUCCESS) {
		printk("CS initiator: CS security enable failed (status 0x%02x)\n", status);
		return;
	}

	/* Procedure timing is derived from the actual negotiated ACL connection
	 * interval (CS procedure-interval units are 0.625 ms), not a hardcoded
	 * connection parameter.
	 */
	bt_conn_get_info(conn, &info);
	acl_interval_in_proc_interval_units = (uint16_t)(info.le.interval_us / 625);
	desired_procedure_interval = (ras_feature_bits & RAS_FEAT_REALTIME_RD) ? 5 : 10;

	const struct bt_le_cs_set_procedure_parameters_param procedure_params = {
		.config_id = CS_CONFIG_ID,
		.max_procedure_len =
			acl_interval_in_proc_interval_units * (desired_procedure_interval - 1),
		.min_procedure_interval = desired_procedure_interval,
		.max_procedure_interval = desired_procedure_interval,
		.max_procedure_count = 0,
		.min_subevent_len = 16000,
		.max_subevent_len = 16000,
		.tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1,
		.phy = BT_LE_CS_PROCEDURE_PHY_2M,
		.tx_power_delta = 0x80,
		.preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1,
		.snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED,
		.snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED,
	};

	err = bt_le_cs_set_procedure_parameters(conn, &procedure_params);
	if (err) {
		printk("CS initiator: failed to set procedure parameters (err %d)\n", err);
		return;
	}

	struct bt_le_cs_procedure_enable_param enable_params = {
		.config_id = CS_CONFIG_ID,
		.enable = 1,
	};

	err = bt_le_cs_procedure_enable(conn, &enable_params);
	if (err) {
		printk("CS initiator: failed to enable CS procedures (err %d)\n", err);
	}
}

static void procedure_enable_cb(struct bt_conn *conn, uint8_t status,
				struct bt_conn_le_cs_procedure_enable_complete *params)
{
	ARG_UNUSED(conn);

	if (status != BT_HCI_ERR_SUCCESS) {
		printk("CS initiator: procedure enable failed (status 0x%02x)\n", status);
		return;
	}

	printk("CS initiator: procedures %s\n", params->state ? "enabled - ranging live" : "disabled");
}

BT_CONN_CB_DEFINE(cs_initiator_conn_cb) = {
	.le_cs_read_remote_capabilities_complete = remote_capabilities_cb,
	.le_cs_config_complete = config_create_cb,
	.le_cs_security_enable_complete = security_enable_cb,
	.le_cs_procedure_enable_complete = procedure_enable_cb,
	.le_cs_subevent_data_available = subevent_result_cb,
};

/* ---------------- Ranging Service discovery ---------------- */

static void ras_dm_completed(struct bt_gatt_dm *dm, void *ctx)
{
	struct bt_conn *conn = bt_gatt_dm_conn_get(dm);
	int err;

	ARG_UNUSED(ctx);

	err = bt_ras_rreq_alloc_and_assign_handles(dm, conn);
	if (err) {
		printk("CS initiator: RAS RREQ alloc failed (err %d)\n", err);
	}

	bt_gatt_dm_data_release(dm);

	if (!err) {
		const struct bt_le_cs_set_default_settings_param default_settings = {
			.enable_initiator_role = true,
			.enable_reflector_role = false,
			.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
			.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
		};

		err = bt_le_cs_set_default_settings(conn, &default_settings);
		if (err) {
			printk("CS initiator: failed to set default CS settings (err %d)\n", err);
			return;
		}

		err = bt_ras_rreq_read_features(conn, ras_features_read_cb);
		if (err) {
			printk("CS initiator: failed to read RAS features (err %d)\n", err);
		}
	}
}

static void ras_dm_not_found(struct bt_conn *conn, void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);

	printk("CS initiator: Ranging Service not found on peer\n");
}

static void ras_dm_error(struct bt_conn *conn, int err, void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);

	printk("CS initiator: Ranging Service discovery error: %d\n", err);
}

static struct bt_gatt_dm_cb ras_dm_cb = {
	.completed = ras_dm_completed,
	.service_not_found = ras_dm_not_found,
	.error_found = ras_dm_error,
};

void cs_initiator_start(struct bt_conn *conn)
{
	int err;

	smoothed_distance_valid = false;

	err = bt_gatt_dm_start(conn, BT_UUID_RANGING_SERVICE, &ras_dm_cb, NULL);
	if (err) {
		printk("CS initiator: Ranging Service discovery start failed (err %d)\n", err);
	}
}
