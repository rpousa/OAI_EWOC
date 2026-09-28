/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef RRC_GNB_CUUP_RELOC_H_
#define RRC_GNB_CUUP_RELOC_H_

#include <stdbool.h>
#include <stdint.h>

#include "common/platform_types.h"
#include "common/platform_constants.h"
#include "e1ap_messages_types.h"
#include "ngap_messages_types.h"

/* forward declarations */
typedef struct gNB_RRC_INST_s gNB_RRC_INST;
typedef struct gNB_RRC_UE_s gNB_RRC_UE_t;

/** @brief Progress of a change of gNB-CU-UP (clause 8.9.5 of TS 38.401).
 *
 * Moves every established PDU session of one UE from the gNB-CU-UP it is bound
 * to (the source) to another gNB-CU-UP of the same gNB-CU-CP (the target). The
 * UE is not involved: the DRB configuration and the AS keys do not change, so no
 * RRC reconfiguration is sent, and PDCP continuity is carried by the COUNT
 * transfer between the two gNB-CU-UPs.
 *
 * Each state is the response the gNB-CU-CP is waiting for, and they follow the
 * order of the steps in clause 8.9.5:
 *
 *   TARGET_SETUP   (steps 2-3)   E1 Bearer Context Setup Response from the
 *                                target, bringing its N3 (downlink) and F1-U
 *                                endpoints.
 *   F1_MOD         (step 4)      F1 UE Context Modification Response from the
 *                                gNB-DU, which now sends uplink to the target.
 *   SOURCE_STATUS  (steps 5-6)   E1 Bearer Context Modification Response from
 *                                the source, carrying the PDCP UL/DL status.
 *                                It comes after the F1 switch on purpose: no
 *                                more uplink reaches the source by then, so the
 *                                COUNTs it reports are final.
 *   TARGET_STATUS  (steps 7-8)   E1 Bearer Context Modification Response from
 *                                the target, which now holds those COUNTs and
 *                                the gNB-DU's endpoints.
 *   PATH_UPDATE    (steps 10-12) NGAP PDU Session Resource Modify Confirm, the
 *                                core having moved the NG-U downlink over. The
 *                                source is released once it arrives (steps
 *                                13-14).
 *
 * Step 9, data forwarding from source to target, is not performed: the E1
 * messages in this implementation carry no data-forwarding IEs, so the packets
 * in flight at the source are lost.
 */
typedef enum cuup_reloc_state_e {
  CUUP_RELOC_IDLE = 0,
  CUUP_RELOC_WAIT_TARGET_SETUP,
  CUUP_RELOC_WAIT_F1_MOD,
  CUUP_RELOC_WAIT_SOURCE_STATUS,
  CUUP_RELOC_WAIT_TARGET_STATUS,
  CUUP_RELOC_WAIT_PATH_UPDATE,
} cuup_reloc_state_t;

/** @brief Per-UE state of an ongoing change of gNB-CU-UP.
 *
 * Both gNB-CU-UPs answer with the same gNB-CU-CP UE E1AP ID -- that ID is scoped
 * to one E1 interface instance, so the same value on two associations is two
 * different UE contexts. Every response is therefore attributed by the SCTP
 * association it arrived on, which the E1 task puts in the ITTI message's
 * originInstance. */
typedef struct cuup_reloc_context_s {
  cuup_reloc_state_t state;
  /// SCTP association of the gNB-CU-UP the UE is moving away from
  sctp_assoc_t source_assoc;
  /// SCTP association of the gNB-CU-UP the UE is moving to
  sctp_assoc_t target_assoc;
  /// gNB-CU-UP IDs of both, for logging: the CU-UP tree is keyed by association
  int64_t source_cuup_id;
  int64_t target_cuup_id;
  /// PDCP COUNTs reported by the source, on their way to the target
  int n_drb_status;
  int drb_ids[MAX_DRBS_PER_UE];
  e1_pdcp_status_info_t drb_status[MAX_DRBS_PER_UE];
  /// the source's F1-U endpoints, to put back should the move be given up on
  int n_source_tunnels;
  int source_tunnel_drb[MAX_DRBS_PER_UE];
  gtpu_tunnel_t source_tunnel[MAX_DRBS_PER_UE];
  /// the target answered the Bearer Context Setup, so it holds a context now
  bool target_has_context;
  /// the source's bearer context has been released (steps 13-14 are done)
  bool source_released;
} cuup_reloc_context_t;

/** @brief Start moving a UE's bearers to another gNB-CU-UP.
 * @param target_cuup_id gNB-CU-UP ID to move to, or -1 to pick any connected
 *        gNB-CU-UP other than the current one.
 * @return 0 when the change has been started (an E1 Bearer Context Setup is in
 *         flight), a negative value when it could not be started at all. */
int nr_rrc_trigger_cuup_reloc(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, int64_t target_cuup_id);

/** @brief Drop the relocation context of a UE, releasing the source gNB-CU-UP's
 * bearer context first if that has not happened yet. Safe to call on a UE that
 * is not relocating; used on UE teardown. */
void nr_rrc_cuup_reloc_finalize(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue);

/** @brief Give up on an ongoing change of gNB-CU-UP: put the UE back on the
 * source gNB-CU-UP where that is still possible, release what was built on the
 * target, and drop the context. */
void nr_rrc_cuup_reloc_abort(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, const char *reason);

/** @brief One of the two gNB-CU-UPs lost its E1 association. Abandons a change
 * that involves it without sending anything to the association that is gone.
 * Safe to call for any UE and any association. */
void nr_rrc_cuup_reloc_cuup_lost(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, sctp_assoc_t assoc_id);

/* Event hooks, called from the E1/F1/NGAP handlers in rrc_gNB.c. Each returns
 * true when the event belonged to a relocation and has been consumed, in which
 * case the caller must not run its normal handling. */

bool nr_rrc_cuup_reloc_e1_setup_resp(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, sctp_assoc_t assoc_id);

bool nr_rrc_cuup_reloc_e1_modif_resp(gNB_RRC_INST *rrc,
                                     gNB_RRC_UE_t *ue,
                                     sctp_assoc_t assoc_id,
                                     int n_drb,
                                     const int *drb_ids,
                                     const e1_pdcp_status_info_t *status);

bool nr_rrc_cuup_reloc_f1_mod_resp(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue);

bool nr_rrc_cuup_reloc_path_update_confirm(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, int n_changed, const int *changed_pdu_ids);

bool nr_rrc_cuup_reloc_e1_failure(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, sctp_assoc_t assoc_id);

/** @brief Trigger a change of gNB-CU-UP from the telnet shell. */
void nr_cuup_reloc_trigger_telnet(gNB_RRC_INST *rrc, uint32_t rrc_ue_id, int64_t target_cuup_id);

#endif /* RRC_GNB_CUUP_RELOC_H_ */
