/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief Change of gNB-CU-UP on the gNB-CU-CP (clause 8.9.5 of TS 38.401).
 *
 * Moves every established PDU session of one UE from the gNB-CU-UP it is bound
 * to to another gNB-CU-UP of the same gNB-CU-CP, without involving the UE: the
 * DRB configuration and the AS keys are unchanged, so no RRC reconfiguration is
 * needed and PDCP continues on the target from the COUNTs the source reports.
 *
 * The steps below are those of clause 8.9.5; the numbers are its own:
 *
 *   2-3.   E1 Bearer Context Setup on the target, which allocates its own N3
 *          (towards the core) and F1-U endpoints for every DRB.
 *   4.     F1 UE Context Modification towards the gNB-DU, changing the uplink
 *          F1-U endpoint of every DRB. These are DRBs to be *modified*: the
 *          radio bearers are untouched, so the UE is never reconfigured.
 *   5-6.   E1 Bearer Context Modification on the source asking for the PDCP
 *          UL/DL status. No uplink reaches the source any more, so the COUNTs
 *          it reports are final -- which is why this follows step 4.
 *   7-8.   E1 Bearer Context Modification on the target handing it those COUNTs
 *          together with the gNB-DU's F1-U endpoints, which the Bearer Context
 *          Setup has no IE to carry.
 *   10-12. NGAP PDU Session Resource Modify Indication, so the core moves the
 *          NG-U downlink to the target's endpoint.
 *   13-14. E1 Bearer Context Release on the source.
 *
 * Step 9, data forwarding between the two gNB-CU-UPs, is not performed: the E1
 * messages here carry no data-forwarding IEs.
 *
 * Uplink recovers at step 4 (its destination, the UPF, never changed); downlink
 * recovers when the core answers step 10-12. Packets in flight are lost.
 */

#include "rrc_gNB_cuup_reloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "assertions.h"
#include "common/ran_context.h"
#include "common/utils/LOG/log.h"
#include "common/utils/alg/foreach.h"
#include "nr_rrc_defs.h"
#include "nr_rrc_proto.h"
#include "openair2/F1AP/f1ap_ids.h"
#include "rrc_gNB_NGAP.h"
#include "rrc_gNB_UE_context.h"

static const char *reloc_state2str(cuup_reloc_state_t state)
{
  switch (state) {
    case CUUP_RELOC_IDLE:
      return "idle";
    case CUUP_RELOC_WAIT_TARGET_SETUP:
      return "waiting for the target's Bearer Context Setup Response";
    case CUUP_RELOC_WAIT_F1_MOD:
      return "waiting for the DU's UE Context Modification Response";
    case CUUP_RELOC_WAIT_SOURCE_STATUS:
      return "waiting for the source's PDCP SN status";
    case CUUP_RELOC_WAIT_TARGET_STATUS:
      return "waiting for the target to acknowledge the PDCP SN status";
    case CUUP_RELOC_WAIT_PATH_UPDATE:
      return "waiting for the PDU Session Resource Modify Confirm";
    default:
      return "unknown";
  }
}

/** @brief Number of established PDU sessions, and the slice of the first one. */
static int count_established_sessions(const gNB_RRC_UE_t *ue, nssai_t *first_nssai)
{
  int n = 0;
  FOR_EACH_SEQ_ARR (rrc_pdu_session_param_t *, pduSession, &((gNB_RRC_UE_t *)ue)->pduSessions) {
    if (pduSession->status != PDU_SESSION_STATUS_ESTABLISHED)
      continue;
    if (n == 0 && first_nssai != NULL)
      *first_nssai = pduSession->param.nssai;
    n++;
  }
  return n;
}

/** @brief Steps 13-14: release the bearer context the source still holds. */
static void release_source(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, cuup_reloc_context_t *ctx)
{
  if (ctx->source_released)
    return;
  ctx->source_released = true;
  e1_release_bearer_context_on(rrc, ue, ctx->source_assoc);
}

void nr_rrc_cuup_reloc_finalize(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue)
{
  if (ue == NULL || ue->cuup_reloc == NULL)
    return;
  /* The UE may be torn down while a change of gNB-CU-UP is in flight. Its
   * release only reaches the gNB-CU-UP it is bound to, so the source would keep
   * a bearer context for a UE that is gone. */
  release_source(rrc, ue, ue->cuup_reloc);
  free(ue->cuup_reloc);
  ue->cuup_reloc = NULL;
}

void nr_rrc_cuup_reloc_abort(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, const char *reason)
{
  cuup_reloc_context_t *ctx = ue->cuup_reloc;
  if (ctx == NULL)
    return;

  LOG_E(NR_RRC,
        "UE %d: change of gNB-CU-UP %d => %d given up while %s: %s\n",
        ue->rrc_ue_id,
        ctx->source_assoc,
        ctx->target_assoc,
        reloc_state2str(ctx->state),
        reason);

  /* Up to and including step 4 the source still holds the user plane, so the UE
   * can be put back on it: its F1-U endpoints were kept for exactly this. */
  const bool source_can_serve = ctx->state != CUUP_RELOC_WAIT_SOURCE_STATUS && ctx->state != CUUP_RELOC_WAIT_TARGET_STATUS
                                && ctx->state != CUUP_RELOC_WAIT_PATH_UPDATE;

  if (source_can_serve) {
    rrc_bind_cuup_to_ue(ue, ctx->source_assoc);
    if (ctx->state == CUUP_RELOC_WAIT_F1_MOD && ctx->n_source_tunnels > 0) {
      /* the DU may already have been told to use the target: point it back */
      rrc_restore_cuup_tunnels(ue, ctx->n_source_tunnels, ctx->source_tunnel_drb, ctx->source_tunnel);
      rrc_send_f1_ue_context_modification_for_cuup_change(rrc, ue);
    }
    if (ctx->target_has_context)
      e1_release_bearer_context_on(rrc, ue, ctx->target_assoc);
    LOG_I(NR_RRC, "UE %d: stays on gNB-CU-UP assoc_id %d\n", ue->rrc_ue_id, ctx->source_assoc);
    ctx->source_released = true; /* the source keeps serving: nothing to release */
  } else {
    /* The source has reported its PDCP COUNTs, so it must not resume: the two
     * would diverge. The UE stays on the target with whatever it has. */
    LOG_E(NR_RRC,
          "UE %d: the user plane is on gNB-CU-UP assoc_id %d; downlink stays broken until the core accepts a new endpoint\n",
          ue->rrc_ue_id,
          ctx->target_assoc);
  }

  nr_rrc_cuup_reloc_finalize(rrc, ue);
}

void nr_rrc_cuup_reloc_cuup_lost(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, sctp_assoc_t assoc_id)
{
  cuup_reloc_context_t *ctx = ue->cuup_reloc;
  if (ctx == NULL || (assoc_id != ctx->source_assoc && assoc_id != ctx->target_assoc))
    return;

  /* From step 5 on the target holds the user plane and the source has given up
   * its PDCP state. */
  const bool target_is_serving = ctx->state == CUUP_RELOC_WAIT_SOURCE_STATUS || ctx->state == CUUP_RELOC_WAIT_TARGET_STATUS
                                 || ctx->state == CUUP_RELOC_WAIT_PATH_UPDATE;

  if (assoc_id == ctx->source_assoc) {
    ctx->source_released = true; /* nothing may be sent to it any more */
    if (target_is_serving) {
      LOG_I(NR_RRC,
            "UE %d: source gNB-CU-UP assoc_id %d went down after its bearers moved away, the change is unaffected\n",
            ue->rrc_ue_id,
            assoc_id);
      return;
    }
    LOG_E(NR_RRC,
          "UE %d: source gNB-CU-UP assoc_id %d went down while %s, the UE has lost its user plane\n",
          ue->rrc_ue_id,
          assoc_id,
          reloc_state2str(ctx->state));
    if (ctx->target_has_context)
      e1_release_bearer_context_on(rrc, ue, ctx->target_assoc);
    nr_rrc_cuup_reloc_finalize(rrc, ue);
    return;
  }

  /* The target is gone: nothing may be sent there, so no release either. */
  ctx->target_has_context = false;
  if (target_is_serving) {
    LOG_E(NR_RRC,
          "UE %d: target gNB-CU-UP assoc_id %d went down after the user plane moved there, the UE has lost its user plane\n",
          ue->rrc_ue_id,
          assoc_id);
    nr_rrc_cuup_reloc_finalize(rrc, ue);
    return;
  }
  nr_rrc_cuup_reloc_abort(rrc, ue, "the target gNB-CU-UP went down");
}

int nr_rrc_trigger_cuup_reloc(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, int64_t target_cuup_id)
{
  DevAssert(rrc != NULL);
  DevAssert(ue != NULL);

  if (ue->cuup_reloc != NULL) {
    LOG_E(NR_RRC,
          "UE %d: a change of gNB-CU-UP is already ongoing (%s), cannot start another\n",
          ue->rrc_ue_id,
          reloc_state2str(ue->cuup_reloc->state));
    return -1;
  }
  if (ue->ho_context != NULL) {
    LOG_E(NR_RRC, "UE %d: a handover is ongoing, cannot change the gNB-CU-UP\n", ue->rrc_ue_id);
    return -1;
  }
  if (!ue->f1_ue_context_active) {
    LOG_E(NR_RRC, "UE %d: no F1 UE context, cannot change the gNB-CU-UP\n", ue->rrc_ue_id);
    return -1;
  }
  if (!ue_associated_to_cuup(ue)) {
    LOG_E(NR_RRC, "UE %d: not associated to any gNB-CU-UP, nothing to move\n", ue->rrc_ue_id);
    return -1;
  }

  nssai_t nssai = {0};
  const int n_sessions = count_established_sessions(ue, &nssai);
  if (n_sessions == 0) {
    LOG_E(NR_RRC, "UE %d: no established PDU session, nothing to move\n", ue->rrc_ue_id);
    return -1;
  }

  const sctp_assoc_t source = get_existing_cuup_for_ue(ue);
  const sctp_assoc_t target = rrc_select_other_cuup_for_ue(rrc, ue, target_cuup_id, nssai.sst, nssai.sd);
  if (target == 0)
    return -1;
  DevAssert(target != source);

  cuup_reloc_context_t *ctx = calloc_or_fail(1, sizeof(*ctx));
  ctx->source_assoc = source;
  ctx->target_assoc = target;
  ctx->source_cuup_id = rrc_get_cuup_id_by_assoc(rrc, source);
  ctx->target_cuup_id = rrc_get_cuup_id_by_assoc(rrc, target);
  ctx->state = CUUP_RELOC_WAIT_TARGET_SETUP;
  /* Keep the source's F1-U endpoints: the target's Bearer Context Setup Response
   * overwrites them, and giving up before step 5 means putting them back. */
  ctx->n_source_tunnels = rrc_save_cuup_tunnels(ue, ctx->source_tunnel_drb, ctx->source_tunnel);
  if (ctx->n_source_tunnels == 0) {
    LOG_E(NR_RRC, "UE %d: no DRB, nothing to move\n", ue->rrc_ue_id);
    free(ctx);
    return -1;
  }
  ue->cuup_reloc = ctx;

  LOG_A(NR_RRC,
        "UE %d: moving %d PDU session(s) from gNB-CU-UP ID %ld (assoc_id %d) to gNB-CU-UP ID %ld (assoc_id %d)\n",
        ue->rrc_ue_id,
        n_sessions,
        ctx->source_cuup_id,
        source,
        ctx->target_cuup_id,
        target);

  /* Steps 2-3: build the whole bearer context on the target. The source keeps
   * serving the UE until step 4. */
  trigger_bearer_setup_on_cuup(rrc, ue, ue->ambr.dl_br, target, E1_BEARER_SETUP_ESTABLISHED_SESSIONS);
  return 0;
}

bool nr_rrc_cuup_reloc_e1_setup_resp(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, sctp_assoc_t assoc_id)
{
  cuup_reloc_context_t *ctx = ue->cuup_reloc;
  if (ctx == NULL)
    return false;
  if (ctx->state != CUUP_RELOC_WAIT_TARGET_SETUP) {
    LOG_W(NR_RRC,
          "UE %d: unexpected Bearer Context Setup Response from assoc_id %d while %s\n",
          ue->rrc_ue_id,
          assoc_id,
          reloc_state2str(ctx->state));
    return false;
  }
  if (assoc_id != ctx->target_assoc) {
    LOG_W(NR_RRC,
          "UE %d: Bearer Context Setup Response from assoc_id %d, expected the target %d\n",
          ue->rrc_ue_id,
          assoc_id,
          ctx->target_assoc);
    return false;
  }

  ctx->target_has_context = true;
  LOG_I(NR_RRC, "UE %d: target gNB-CU-UP assoc_id %d holds the bearers, repointing the DU\n", ue->rrc_ue_id, ctx->target_assoc);

  /* Step 4 */
  ctx->state = CUUP_RELOC_WAIT_F1_MOD;
  if (rrc_send_f1_ue_context_modification_for_cuup_change(rrc, ue) == 0) {
    nr_rrc_cuup_reloc_abort(rrc, ue, "could not repoint the DU at the target gNB-CU-UP");
    return true;
  }
  return true;
}

bool nr_rrc_cuup_reloc_f1_mod_resp(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue)
{
  cuup_reloc_context_t *ctx = ue->cuup_reloc;
  if (ctx == NULL || ctx->state != CUUP_RELOC_WAIT_F1_MOD)
    return false;

  /* The DU sends uplink to the target now, so the target is the gNB-CU-UP that
   * serves this UE: bind it before anything else goes out on E1. */
  if (!rrc_bind_cuup_to_ue(ue, ctx->target_assoc)) {
    nr_rrc_cuup_reloc_abort(rrc, ue, "could not bind the UE to the target gNB-CU-UP");
    return true;
  }

  LOG_I(NR_RRC,
        "UE %d: the DU sends uplink to gNB-CU-UP assoc_id %d, asking the source for the PDCP SN status\n",
        ue->rrc_ue_id,
        ctx->target_assoc);

  /* Steps 5-6 */
  ctx->state = CUUP_RELOC_WAIT_SOURCE_STATUS;
  e1_request_pdcp_status_on(rrc, ue, ctx->source_assoc);
  return true;
}

bool nr_rrc_cuup_reloc_e1_modif_resp(gNB_RRC_INST *rrc,
                                     gNB_RRC_UE_t *ue,
                                     sctp_assoc_t assoc_id,
                                     int n_drb,
                                     const int *drb_ids,
                                     const e1_pdcp_status_info_t *status)
{
  cuup_reloc_context_t *ctx = ue->cuup_reloc;
  if (ctx == NULL)
    return false;

  switch (ctx->state) {
    case CUUP_RELOC_WAIT_SOURCE_STATUS: {
      if (assoc_id != ctx->source_assoc)
        return false;
      if (n_drb <= 0) {
        nr_rrc_cuup_reloc_abort(rrc, ue, "the source gNB-CU-UP reported no PDCP SN status");
        return true;
      }
      DevAssert(drb_ids != NULL && status != NULL);
      ctx->n_drb_status = n_drb < MAX_DRBS_PER_UE ? n_drb : MAX_DRBS_PER_UE;
      for (int i = 0; i < ctx->n_drb_status; i++) {
        ctx->drb_ids[i] = drb_ids[i];
        ctx->drb_status[i] = status[i];
      }
      LOG_I(NR_RRC,
            "UE %d: source gNB-CU-UP reported the PDCP status of %d DRB(s), handing it to the target\n",
            ue->rrc_ue_id,
            ctx->n_drb_status);

      /* Steps 7-8 */
      ctx->state = CUUP_RELOC_WAIT_TARGET_STATUS;
      e1_notify_pdcp_status_drbs(rrc, ue, ctx->target_assoc, ctx->n_drb_status, ctx->drb_ids, ctx->drb_status);
      return true;
    }

    case CUUP_RELOC_WAIT_TARGET_STATUS: {
      if (assoc_id != ctx->target_assoc)
        return false;

      LOG_I(NR_RRC,
            "UE %d: target gNB-CU-UP is serving the bearers, announcing the new downlink endpoint to the core\n",
            ue->rrc_ue_id);

      /* Steps 10-12 */
      ctx->state = CUUP_RELOC_WAIT_PATH_UPDATE;
      if (rrc_gNB_send_NGAP_PDUSESSION_MODIFY_INDICATION(rrc, ue) == 0) {
        LOG_E(NR_RRC,
              "UE %d: could not announce the new downlink endpoint; the UE is on gNB-CU-UP assoc_id %d but the core still "
              "sends downlink to the old one\n",
              ue->rrc_ue_id,
              ctx->target_assoc);
        nr_rrc_cuup_reloc_finalize(rrc, ue);
      }
      return true;
    }

    default:
      /* Any other modification response during a change of gNB-CU-UP is not
       * ours to consume. */
      return false;
  }
}

bool nr_rrc_cuup_reloc_path_update_confirm(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, int n_changed, const int *changed_pdu_ids)
{
  cuup_reloc_context_t *ctx = ue->cuup_reloc;
  if (ctx == NULL || ctx->state != CUUP_RELOC_WAIT_PATH_UPDATE)
    return false;

  /* The core switched its downlink to the target. If it also moved the uplink,
   * the SMF put a different UPF on the path and the target has to be told where
   * to send uplink now. */
  if (n_changed > 0) {
    LOG_I(NR_RRC, "UE %d: the core moved the uplink of %d PDU session(s) while switching the path\n", ue->rrc_ue_id, n_changed);
    e1_update_n3_uplink_tunnels(rrc, ue, ctx->target_assoc, n_changed, changed_pdu_ids);
  }

  LOG_A(NR_RRC,
        "UE %d: change of gNB-CU-UP complete, now served by gNB-CU-UP ID %ld (assoc_id %d)\n",
        ue->rrc_ue_id,
        ctx->target_cuup_id,
        ctx->target_assoc);

  /* Steps 13-14, once the core no longer sends downlink to the source: releasing
   * it earlier would leave the UPF sending to a TEID that no longer exists. */
  nr_rrc_cuup_reloc_finalize(rrc, ue);
  return true;
}

bool nr_rrc_cuup_reloc_e1_failure(gNB_RRC_INST *rrc, gNB_RRC_UE_t *ue, sctp_assoc_t assoc_id)
{
  cuup_reloc_context_t *ctx = ue->cuup_reloc;
  if (ctx == NULL)
    return false;

  if (assoc_id == ctx->target_assoc) {
    if (ctx->state == CUUP_RELOC_WAIT_TARGET_SETUP)
      ctx->target_has_context = false; /* it never took the context */
    nr_rrc_cuup_reloc_abort(rrc, ue, "the target gNB-CU-UP rejected an E1 procedure");
    return true;
  }
  if (assoc_id == ctx->source_assoc) {
    nr_rrc_cuup_reloc_abort(rrc, ue, "the source gNB-CU-UP rejected an E1 procedure");
    return true;
  }
  return false;
}

void nr_cuup_reloc_trigger_telnet(gNB_RRC_INST *rrc, uint32_t rrc_ue_id, int64_t target_cuup_id)
{
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(rrc, rrc_ue_id);
  if (ue_context_p == NULL) {
    LOG_E(NR_RRC, "change of gNB-CU-UP: no UE context for UE ID %u\n", rrc_ue_id);
    return;
  }
  nr_rrc_trigger_cuup_reloc(rrc, &ue_context_p->ue_context, target_cuup_id);
}
