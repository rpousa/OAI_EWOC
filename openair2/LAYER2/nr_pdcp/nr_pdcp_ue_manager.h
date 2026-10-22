/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef _NR_PDCP_UE_MANAGER_H_
#define _NR_PDCP_UE_MANAGER_H_

#include "nr_pdcp_entity.h"
#include "common/platform_constants.h"
#include "common/platform_types.h"

typedef void nr_pdcp_ue_manager_t;

#define NR_PDCP_MAX_PDU_NSSAI 16

typedef struct nr_pdcp_ue_t {
  ue_id_t ue_id;
  nr_pdcp_entity_t *srb[3];
  nr_pdcp_entity_t *drb[MAX_DRBS_PER_UE];
  /* S-NSSAI of each PDU session, as given by the CU-CP in E1 Bearer Context
   * Setup, so per-slice measurements (E2SM-KPM) can find the slice's DRBs.
   * PDCP itself does not use it. sd 0xffffff = no SD. */
  struct {
    long pdusession_id;
    uint8_t sst;
    uint32_t sd;
  } pdu_nssai[NR_PDCP_MAX_PDU_NSSAI];
  int n_pdu_nssai;
} nr_pdcp_ue_t;

/***********************************************************************/
/* manager functions                                                   */
/***********************************************************************/

nr_pdcp_ue_manager_t *new_nr_pdcp_ue_manager(int enb_flag);

int nr_pdcp_manager_get_enb_flag(nr_pdcp_ue_manager_t *m);

void nr_pdcp_manager_lock(nr_pdcp_ue_manager_t *m);
void nr_pdcp_manager_unlock(nr_pdcp_ue_manager_t *m);

nr_pdcp_ue_t *nr_pdcp_manager_get_ue(nr_pdcp_ue_manager_t *m, ue_id_t UEid);
void nr_pdcp_manager_remove_ue(nr_pdcp_ue_manager_t *m, ue_id_t UEid);

nr_pdcp_ue_t **nr_pdcp_manager_get_ue_list(nr_pdcp_ue_manager_t *_m);
int nr_pdcp_manager_get_ue_count(nr_pdcp_ue_manager_t *_m);

/***********************************************************************/
/* ue functions                                                        */
/***********************************************************************/

void nr_pdcp_ue_add_srb_pdcp_entity(nr_pdcp_ue_t *ue, int srb_id,
                                    nr_pdcp_entity_t *entity);
void nr_pdcp_ue_add_drb_pdcp_entity(nr_pdcp_ue_t *ue, int drb_id,
                                    nr_pdcp_entity_t *entity);

/***********************************************************************/
/* hacks                                                               */
/***********************************************************************/

/* returns 0 if no UE, 1 otherwise */
bool nr_pdcp_get_first_ue_id(nr_pdcp_ue_manager_t *m, ue_id_t *ret);

#endif /* _NR_PDCP_UE_MANAGER_H_ */
