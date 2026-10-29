/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common/utils/time_stat.h"
#include "nr_pdcp_oai_api.h"
#include <errno.h>
#include <fcntl.h>
#include <openair3/ocp-gtpu/gtp_itf.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "NR_DRB-ToAddMod.h"
#include "NR_QFI.h"
#include "NR_SDAP-Config.h"
#include "NR_SRB-ToAddMod.h"
#include "SDAP/nr_sdap/nr_sdap_entity.h"
#include "assertions.h"
#include "common/ngran_types.h"
#include "common/platform_constants.h"
#include "common/ran_context.h"
#include "common/utils/T/T.h"
#include "common/utils/tuntap_if.h"
#include "cuup_cucp_if.h"
#include "executables/lte-softmodem.h"
#include "executables/softmodem-common.h"
#include "f1ap_messages_types.h"
#include "gnb_config.h"
#include "gtpv1_u_messages_types.h"
#include "ds/hashtable.h"
#include "intertask_interface.h"
#include "common/utils/LOG/log.h"
#include "nfapi/oai_integration/vendor_ext.h"
#include "nr_pdcp_asn1_utils.h"
#include "nr_pdcp_timer_thread.h"
#include "nr_pdcp_ue_manager.h"
#include "openair2/F1AP/f1ap_common.h"
#include "openair2/F1AP/f1ap_ids.h"
#include "openair2/SDAP/nr_sdap/nr_sdap.h"
#include "pdcp.h"
#include "pdcp_messages_types.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "utils.h"

#define TODO do { \
    printf("%s:%d:%s: todo\n", __FILE__, __LINE__, __FUNCTION__); \
    exit(1); \
  } while (0)

static nr_pdcp_ue_manager_t *nr_pdcp_ue_manager;

/* necessary globals for OAI, not used internally */
hash_table_t  *pdcp_coll_p;
static uint64_t pdcp_optmask;

static ngran_node_t node_type;

nr_pdcp_entity_t *nr_pdcp_get_rb(nr_pdcp_ue_t *ue, int rb_id, bool srb_flag)
{
  nr_pdcp_entity_t *rb;

  if (srb_flag) {
    if (rb_id < 1 || rb_id > 2)
      rb = NULL;
    else
      rb = ue->srb[rb_id - 1];
  } else {
    if (rb_id < 1 || rb_id > MAX_DRBS_PER_UE)
      rb = NULL;
    else
      rb = ue->drb[rb_id - 1];
  }

  return rb;
}

/****************************************************************************/
/* rlc_data_req queue - begin                                               */
/****************************************************************************/


#include <pthread.h>

/* NR PDCP and RLC both use "big locks". In some cases a thread may do
 * lock(rlc) followed by lock(pdcp) (typically when running 'rx_sdu').
 * Another thread may first do lock(pdcp) and then lock(rlc) (typically
 * the GTP module calls 'nr_pdcp_data_req' that, in a previous implementation
 * was indirectly calling 'rlc_data_req' which does lock(rlc)).
 * To avoid the resulting deadlock it is enough to ensure that a call
 * to lock(pdcp) will never be followed by a call to lock(rlc). So,
 * here we chose to have a separate thread that deals with rlc_data_req,
 * out of the PDCP lock. Other solutions may be possible.
 * So instead of calling 'rlc_data_req' directly we have a queue and a
 * separate thread emptying it.
 */

typedef struct {
  protocol_ctxt_t ctxt_pP;
  srb_flag_t      srb_flagP;
  rb_id_t         rb_idP;
  mui_t           muiP;
  confirm_t       confirmP;
  sdu_size_t      sdu_sizeP;
  uint8_t *sdu_pP;
} rlc_data_req_queue_item;

#define RLC_DATA_REQ_QUEUE_SIZE 10000

typedef struct {
  rlc_data_req_queue_item q[RLC_DATA_REQ_QUEUE_SIZE];
  volatile int start;
  volatile int length;
  pthread_mutex_t m;
  pthread_cond_t c;
} rlc_data_req_queue;

static rlc_data_req_queue q;

static void *rlc_data_req_thread(void *_)
{
  int i;

  pthread_setname_np(pthread_self(), "RLC queue");
  while (1) {
    if (pthread_mutex_lock(&q.m) != 0) abort();
    while (q.length == 0)
      if (pthread_cond_wait(&q.c, &q.m) != 0) abort();
    i = q.start;
    if (pthread_mutex_unlock(&q.m) != 0) abort();

    nr_rlc_data_req(&q.q[i].ctxt_pP,
                    q.q[i].srb_flagP,
                    q.q[i].rb_idP,
                    q.q[i].muiP,
                    q.q[i].sdu_sizeP,
                    q.q[i].sdu_pP);

    if (pthread_mutex_lock(&q.m) != 0) abort();

    q.length--;
    q.start = (q.start + 1) % RLC_DATA_REQ_QUEUE_SIZE;

    if (pthread_cond_signal(&q.c) != 0) abort();
    if (pthread_mutex_unlock(&q.m) != 0) abort();
  }
}

static void init_nr_rlc_data_req_queue(void)
{
  pthread_t t;

  pthread_mutex_init(&q.m, NULL);
  pthread_cond_init(&q.c, NULL);

  if (pthread_create(&t, NULL, rlc_data_req_thread, NULL) != 0) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
}

static void enqueue_rlc_data_req(const protocol_ctxt_t *const ctxt_pP,
                                 const srb_flag_t srb_flagP,
                                 const rb_id_t rb_idP,
                                 const mui_t muiP,
                                 confirm_t confirmP,
                                 sdu_size_t sdu_sizeP,
                                 uint8_t *sdu_pP)
{
  int i;
  int logged = 0;

  if (pthread_mutex_lock(&q.m) != 0) abort();
  while (q.length == RLC_DATA_REQ_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      LOG_W(PDCP, "%s: rlc_data_req queue is full\n", __FUNCTION__);
    }
    if (pthread_cond_wait(&q.c, &q.m) != 0) abort();
  }

  i = (q.start + q.length) % RLC_DATA_REQ_QUEUE_SIZE;
  q.length++;

  q.q[i].ctxt_pP    = *ctxt_pP;
  q.q[i].srb_flagP  = srb_flagP;
  q.q[i].rb_idP     = rb_idP;
  q.q[i].muiP       = muiP;
  q.q[i].confirmP   = confirmP;
  q.q[i].sdu_sizeP  = sdu_sizeP;
  q.q[i].sdu_pP     = sdu_pP;

  if (pthread_cond_signal(&q.c) != 0) abort();
  if (pthread_mutex_unlock(&q.m) != 0) abort();
}

/****************************************************************************/
/* rlc_data_req queue - end                                                 */
/****************************************************************************/

/****************************************************************************/
/* pdcp_data_ind thread - begin                                             */
/****************************************************************************/

typedef struct {
  protocol_ctxt_t ctxt_pP;
  srb_flag_t      srb_flagP;
  rb_id_t         rb_id;
  sdu_size_t      sdu_buffer_size;
  uint8_t *sdu_buffer;
} pdcp_data_ind_queue_item;

#define PDCP_DATA_IND_QUEUE_SIZE 10000

typedef struct {
  pdcp_data_ind_queue_item q[PDCP_DATA_IND_QUEUE_SIZE];
  volatile int start;
  volatile int length;
  pthread_mutex_t m;
  pthread_cond_t c;
} pdcp_data_ind_queue;

static pdcp_data_ind_queue pq;

static void do_pdcp_data_ind(const protocol_ctxt_t *const ctxt_pP,
                             const srb_flag_t srb_flagP,
                             const rb_id_t rb_id,
                             const sdu_size_t sdu_buffer_size,
                             uint8_t *const sdu_buffer)
{
  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;
  ue_id_t UEid = ctxt_pP->rntiMaybeUEid;

  if (ctxt_pP->module_id != 0 ||
      //ctxt_pP->enb_flag != 1 ||
      ctxt_pP->instance != 0 ||
      ctxt_pP->eNB_index != 0 ||
      ctxt_pP->brOption != 0) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  if (ctxt_pP->enb_flag)
    T(T_ENB_PDCP_UL, T_INT(ctxt_pP->module_id), T_INT(UEid), T_INT(rb_id), T_INT(sdu_buffer_size));

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, UEid);
  rb = nr_pdcp_get_rb(ue, rb_id, srb_flagP);

  if (rb != NULL) {
    rb->recv_pdu(rb, (char *)sdu_buffer, sdu_buffer_size);
  } else {
    LOG_E(PDCP, "pdcp_data_ind: no RB found (rb_id %ld, srb_flag %d)\n", rb_id, srb_flagP);
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

static void *pdcp_data_ind_thread(void *_)
{
  int i;

  pthread_setname_np(pthread_self(), "PDCP data ind");
  while (1) {
    if (pthread_mutex_lock(&pq.m) != 0) abort();
    while (pq.length == 0)
      if (pthread_cond_wait(&pq.c, &pq.m) != 0) abort();
    i = pq.start;
    if (pthread_mutex_unlock(&pq.m) != 0) abort();

    do_pdcp_data_ind(&pq.q[i].ctxt_pP,
                     pq.q[i].srb_flagP,
                     pq.q[i].rb_id,
                     pq.q[i].sdu_buffer_size,
                     pq.q[i].sdu_buffer);
    free(pq.q[i].sdu_buffer);

    if (pthread_mutex_lock(&pq.m) != 0) abort();

    pq.length--;
    pq.start = (pq.start + 1) % PDCP_DATA_IND_QUEUE_SIZE;

    if (pthread_cond_signal(&pq.c) != 0) abort();
    if (pthread_mutex_unlock(&pq.m) != 0) abort();
  }
}

static void init_nr_pdcp_data_ind_queue(void)
{
  pthread_t t;

  pthread_mutex_init(&pq.m, NULL);
  pthread_cond_init(&pq.c, NULL);

  if (pthread_create(&t, NULL, pdcp_data_ind_thread, NULL) != 0) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }
}

static void enqueue_pdcp_data_ind(const protocol_ctxt_t *const ctxt_pP,
                                  const srb_flag_t srb_flagP,
                                  const rb_id_t rb_id,
                                  const sdu_size_t sdu_buffer_size,
                                  uint8_t *const sdu_buffer)
{
  int i;
  int logged = 0;

  if (pthread_mutex_lock(&pq.m) != 0) abort();
  while (pq.length == PDCP_DATA_IND_QUEUE_SIZE) {
    if (!logged) {
      logged = 1;
      static unsigned int queue_full_count = 0;
      static uint64_t t_log_queue_full = 0;
      uint64_t now = nr_pdcp_current_time();

      /* 1280 ms = 128 frames @ 10 ms, to match MAC cadence */
      if (queue_full_count != 0 && now > t_log_queue_full + 1280) {
        LOG_W(PDCP, "%s: pdcp_data_ind queue is full (%u times)\n", __FUNCTION__, queue_full_count);
        queue_full_count = 0;
        t_log_queue_full = now;
      }
      queue_full_count++;
    }
    if (pthread_cond_wait(&pq.c, &pq.m) != 0) abort();
  }

  i = (pq.start + pq.length) % PDCP_DATA_IND_QUEUE_SIZE;
  pq.length++;

  pq.q[i].ctxt_pP         = *ctxt_pP;
  pq.q[i].srb_flagP       = srb_flagP;
  pq.q[i].rb_id           = rb_id;
  pq.q[i].sdu_buffer_size = sdu_buffer_size;
  pq.q[i].sdu_buffer      = sdu_buffer;

  if (pthread_cond_signal(&pq.c) != 0) abort();
  if (pthread_mutex_unlock(&pq.m) != 0) abort();
}

bool nr_pdcp_data_ind(const protocol_ctxt_t *const ctxt_pP,
                      const srb_flag_t srb_flagP,
                      const rb_id_t rb_id,
                      const sdu_size_t sdu_buffer_size,
                      uint8_t *const sdu_buffer)
{
  enqueue_pdcp_data_ind(ctxt_pP, srb_flagP, rb_id, sdu_buffer_size, sdu_buffer);
  return true;
}

/****************************************************************************/
/* pdcp_data_ind thread - end                                               */
/****************************************************************************/

int pdcp_fifo_flush_sdus(const protocol_ctxt_t *const ctxt_pP)
{
  return 0;
}

static void set_node_type() {
  node_type = get_node_type();
}

void nr_pdcp_layer_init(void)
{
  /* hack: be sure to initialize only once */
  static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  static int initialized = 0;
  if (pthread_mutex_lock(&m) != 0) abort();
  if (initialized) {
    if (pthread_mutex_unlock(&m) != 0) abort();
    return;
  }
  initialized = 1;
  if (pthread_mutex_unlock(&m) != 0) abort();

  nr_pdcp_ue_manager = new_nr_pdcp_ue_manager(1);

  set_node_type();

  if ((RC.nrrrc == NULL) || (!NODE_IS_CU(node_type))) {
    init_nr_rlc_data_req_queue();
  }
#ifdef PDCP_CUCP_CUUP
  nr_pdcp_e1_if_init(node_type == ngran_gNB_CUUP || node_type == ngran_gNB_CUCP);
#endif
  init_nr_pdcp_data_ind_queue();
  nr_pdcp_init_timer_thread(nr_pdcp_ue_manager);
}

#include "nfapi/oai_integration/vendor_ext.h"
#include "executables/lte-softmodem.h"
#include "common/utils/tuntap_if.h"
#include "openair2/SDAP/nr_sdap/nr_sdap.h"

static void deliver_sdu_drb(void *_ue, nr_pdcp_entity_t *entity,
                            char *buf, int size,
                            const nr_pdcp_integrity_data_t *msg_integrity)
{
  nr_pdcp_ue_t *ue = _ue;
  int rb_id;
  int i;

  if (IS_SOFTMODEM_NOS1 || UE_NAS_USE_TUN) {
    LOG_D(PDCP, "IP packet received with size %d, to be sent to SDAP interface, UE ID/RNTI: %ld\n", size, ue->ue_id);
    // in NoS1 mode: the SDAP should write() packets to an FD (TUN interface),
    // so below, set is_gnb == 0 to do that
    sdap_data_ind(entity->rb_id, 0, entity->pdusession_id, ue->ue_id, buf, size);
  }
  else{
    for (i = 0; i < MAX_DRBS_PER_UE; i++) {
        if (entity == ue->drb[i]) {
          rb_id = i+1;
          goto rb_found;
        }
      }

      LOG_E(PDCP, "%s:%d:%s: fatal, no RB found for UE ID/RNTI %ld\n", __FILE__, __LINE__, __FUNCTION__, ue->ue_id);
      exit(1);

    rb_found:
    {
      LOG_D(PDCP, "%s() (drb %d) sending message to SDAP size %d\n", __func__, rb_id, size);
      sdap_data_ind(rb_id, ue->drb[rb_id - 1]->is_gnb, ue->drb[rb_id - 1]->pdusession_id, ue->ue_id, buf, size);
    }
  }
}

static void deliver_pdu_drb_ue(void *deliver_pdu_data, ue_id_t ue_id, int rb_id,
                               char *buf, int size, int sdu_id)
{
  DevAssert(deliver_pdu_data == NULL);
  protocol_ctxt_t ctxt = { .enb_flag = 0, .rntiMaybeUEid = ue_id };

  uint8_t *memblock = malloc16(size);
  memcpy(memblock, buf, size);
  LOG_D(PDCP, "%s(): (drb %d) calling rlc_data_req size %d UE %ld/%04lx\n", __func__, rb_id, size, ctxt.rntiMaybeUEid, ctxt.rntiMaybeUEid);
  enqueue_rlc_data_req(&ctxt, 0, rb_id, sdu_id, 0, size, memblock);
}

static void deliver_pdu_drb_gnb(void *deliver_pdu_data, ue_id_t ue_id, int rb_id,
                                char *buf, int size, int sdu_id)
{
  DevAssert(deliver_pdu_data == NULL);
  f1_ue_data_t ue_data = cu_get_f1_ue_data(ue_id);
  protocol_ctxt_t ctxt = { .enb_flag = 1, .rntiMaybeUEid = ue_data.secondary_ue };

  if (NODE_IS_CU(node_type)) {
    LOG_D(PDCP, "%s() (drb %d) sending message to gtp size %d\n", __func__, rb_id, size);
    const f1ap_cudu_inst_t *inst = getCxt(0);
    DevAssert(inst);
    gtpv1uSendDirectWithNRUSeqNum(inst->gtpInst, ue_id, rb_id, (uint8_t *)buf, size);
  } else {
    uint8_t *memblock = malloc16(size);
    memcpy(memblock, buf, size);
    LOG_D(PDCP, "%s(): (drb %d) calling rlc_data_req size %d\n", __func__, rb_id, size);
    enqueue_rlc_data_req(&ctxt, 0, rb_id, sdu_id, 0, size, memblock);
  }
}

static void deliver_sdu_srb(void *_ue, nr_pdcp_entity_t *entity,
                            char *buf, int size,
                            const nr_pdcp_integrity_data_t *msg_integrity)
{
  nr_pdcp_ue_t *ue = _ue;
  int srb_id;
  int i;

  for (i = 0; i < sizeofArray(ue->srb) ; i++) {
    if (entity == ue->srb[i]) {
      srb_id = i+1;
      goto srb_found;
    }
  }

  LOG_E(PDCP, "%s:%d:%s: fatal, no SRB found for UE ID/RNTI %ld\n", __FILE__, __LINE__, __FUNCTION__, ue->ue_id);
  exit(1);

srb_found:
  if (entity->is_gnb) {
    MessageDef *message_p = itti_alloc_new_message(TASK_PDCP_GNB, 0, F1AP_UL_RRC_MESSAGE);
    AssertFatal(message_p != NULL, "OUT OF MEMORY\n");
    f1ap_ul_rrc_message_t *ul_rrc = &F1AP_UL_RRC_MESSAGE(message_p);
    ul_rrc->gNB_CU_ue_id = ue->ue_id;
    /* look up the correct secondary UE ID to provide complete information to
     * RRC, the RLC-PDCP interface does not transport this information */
    f1_ue_data_t ue_data = cu_get_f1_ue_data(ue->ue_id);
    ul_rrc->gNB_DU_ue_id = ue_data.secondary_ue;
    message_p->ittiMsgHeader.originInstance = ue_data.du_assoc_id;
    ul_rrc->srb_id = srb_id;
    ul_rrc->rrc_container = malloc(size);
    AssertFatal(ul_rrc->rrc_container != NULL, "OUT OF MEMORY\n");
    memcpy(ul_rrc->rrc_container, buf, size);
    ul_rrc->rrc_container_length = size;
    itti_send_msg_to_task(TASK_RRC_GNB, 0, message_p);
  } else {
    uint8_t *rrc_buffer_p = itti_malloc(TASK_PDCP_UE, TASK_RRC_NRUE, size);
    AssertFatal(rrc_buffer_p != NULL, "OUT OF MEMORY\n");
    memcpy(rrc_buffer_p, buf, size);
    MessageDef *message_p = itti_alloc_new_message(TASK_PDCP_UE, 0, NR_RRC_DCCH_DATA_IND);
    AssertFatal(message_p != NULL, "OUT OF MEMORY\n");
    NR_RRC_DCCH_DATA_IND(message_p).dcch_index = srb_id;
    NR_RRC_DCCH_DATA_IND(message_p).sdu_p = rrc_buffer_p;
    NR_RRC_DCCH_DATA_IND(message_p).sdu_size = size;
    memcpy(&NR_RRC_DCCH_DATA_IND(message_p).msg_integrity, msg_integrity, sizeof(*msg_integrity));
    ue_id_t ue_id = ue->ue_id;
    itti_send_msg_to_task(TASK_RRC_NRUE, ue_id, message_p);
  }
}

void deliver_pdu_srb_rlc(void *deliver_pdu_data, ue_id_t ue_id, int srb_id,
                         char *buf, int size, int sdu_id)
{
  protocol_ctxt_t ctxt = { .enb_flag = 1, .rntiMaybeUEid = ue_id };
  uint8_t *memblock = malloc16(size);
  memcpy(memblock, buf, size);
  enqueue_rlc_data_req(&ctxt, 1, srb_id, sdu_id, 0, size, memblock);
}

void add_srb(int is_gnb,
             ue_id_t UEid,
             struct NR_SRB_ToAddMod *s,
             const nr_pdcp_entity_security_keys_and_algos_t *security_parameters)
{
  nr_pdcp_entity_t *pdcp_srb;
  nr_pdcp_ue_t *ue;

  int srb_id = s->srb_Identity;
  int t_Reordering = -1; // infinity as per default SRB configuration in 9.2.1 of 38.331
  if (s->pdcp_Config != NULL && s->pdcp_Config->t_Reordering != NULL)
    t_Reordering = decode_t_reordering(*s->pdcp_Config->t_Reordering);

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, UEid);
  if (nr_pdcp_get_rb(ue, srb_id, true) != NULL) {
    LOG_E(PDCP, "warning SRB %d already exist for UE ID %ld, do nothing\n", srb_id, UEid);
  } else {
    pdcp_srb = new_nr_pdcp_entity(NR_PDCP_SRB,
                                  is_gnb,
                                  srb_id,
                                  0,      // PDU session ID (not relevant)
                                  false,  // has SDAP RX (not relevant)
                                  false,  // has SDAP TX (not relevant)
                                  deliver_sdu_srb,
                                  ue,
                                  NULL,
                                  ue,
                                  SHORT_SN_SIZE,
                                  t_Reordering,
                                  -1,
                                  security_parameters);
    nr_pdcp_ue_add_srb_pdcp_entity(ue, srb_id, pdcp_srb);

    LOG_D(PDCP, "added srb %d to UE ID %ld\n", srb_id, UEid);
  }
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_add_drb(int is_gnb,
                     const ue_id_t UEid,
                     const NR_PDCP_Config_t *pdcp,
                     const sdap_config_t *sdap,
                     const nr_pdcp_entity_security_keys_and_algos_t *security_parameters)
{
  nr_pdcp_ue_t *ue;

  if (!pdcp->drb) {
    LOG_E(PDCP, "Missing PDCP config. DRB %d not configured.\n", sdap->drb_id);
    return;
  }

  struct NR_PDCP_Config__drb *drb = pdcp->drb;
  int sn_size_ul = decode_sn_size_ul(*drb->pdcp_SN_SizeUL);
  int sn_size_dl = decode_sn_size_dl(*drb->pdcp_SN_SizeDL);
  int discard_timer = decode_discard_timer(*drb->discardTimer);

  /* if pdcp_Config->t_Reordering is not present, it means infinity (-1) */
  int t_reordering = -1;
  if (pdcp->t_Reordering != NULL) {
    t_reordering = decode_t_reordering(*pdcp->t_Reordering);
  }

  int has_integrity = (drb->integrityProtection != NULL) ? 1 : 0;
  int has_ciphering = (pdcp->ext1 != NULL && pdcp->ext1->cipheringDisabled != NULL) ? 0 : 1;

  /* get actual ciphering and integrity algorithm based on pdcp_Config */
  nr_pdcp_entity_security_keys_and_algos_t actual_security_parameters = *security_parameters;
  actual_security_parameters.ciphering_algorithm = has_ciphering ? security_parameters->ciphering_algorithm : 0;
  actual_security_parameters.integrity_algorithm = has_integrity ? security_parameters->integrity_algorithm : 0;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, UEid);
  if (nr_pdcp_get_rb(ue, sdap->drb_id, false) != NULL) {
    LOG_W(PDCP, "warning DRB %d already exist for UE ID %ld, do nothing\n", sdap->drb_id, UEid);
  } else {
    // code assumption: same SN size for both DL and UL
    if (sn_size_ul != sn_size_dl) {
      LOG_E(PDCP, "fatal, bad SN sizes, must be same. ul=%d, dl=%d\n", sn_size_ul, sn_size_dl);
      exit(1);
    }

    // add PDCP entity
    nr_pdcp_entity_t *pdcp_drb = new_nr_pdcp_entity(NR_PDCP_DRB_AM,
                                                    is_gnb,
                                                    sdap->drb_id,
                                                    sdap->pdusession_id,
                                                    (sdap->role & (SDAP_UL_RX | SDAP_DL_RX)) != 0,
                                                    (sdap->role & (SDAP_UL_TX | SDAP_DL_TX)) != 0,
                                                    deliver_sdu_drb,
                                                    ue,
                                                    is_gnb ? deliver_pdu_drb_gnb : deliver_pdu_drb_ue,
                                                    ue,
                                                    sn_size_dl,
                                                    t_reordering,
                                                    discard_timer,
                                                    &actual_security_parameters);
    nr_pdcp_ue_add_drb_pdcp_entity(ue, sdap->drb_id, pdcp_drb);

    LOG_I(PDCP, "Added DRB %d to UE ID %ld\n", sdap->drb_id, UEid);
  }
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_add_srbs(eNB_flag_t enb_flag,
                      ue_id_t UEid,
                      NR_SRB_ToAddModList_t *const srb2add_list,
                      const nr_pdcp_entity_security_keys_and_algos_t *security_parameters)
{
  if (srb2add_list != NULL) {
    for (int i = 0; i < srb2add_list->list.count; i++) {
      add_srb(enb_flag, UEid, srb2add_list->list.array[i], security_parameters);
    }
  } else
    LOG_W(PDCP, "nr_pdcp_add_srbs() with void list\n");
}

/** @brief Get DRB IDs for a specific PDU session */
int nr_pdcp_get_drb_ids_for_pdusession(ue_id_t ue_id, long pdusession_id, int *drb_ids)
{
  int num_found = 0;
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  if (ue) {
    for (int i = 0; i < MAX_DRBS_PER_UE; i++) {
      nr_pdcp_entity_t *drb = ue->drb[i];
      if (drb && drb->pdusession_id == pdusession_id) {
        if (num_found < MAX_DRBS_PER_UE) {
          drb_ids[num_found] = drb->rb_id;
          num_found++;
        } else {
          LOG_W(PDCP, "Found more DRBs than MAX_DRBS_PER_UE=%d\n", MAX_DRBS_PER_UE);
          break;
        }
      }
    }
  }
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  return num_found;
}

void nr_pdcp_set_pdusession_nssai(ue_id_t ue_id, long pdusession_id, uint8_t sst, uint32_t sd)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  int i = 0;
  while (i < ue->n_pdu_nssai && ue->pdu_nssai[i].pdusession_id != pdusession_id)
    i++;
  if (i == ue->n_pdu_nssai) {
    if (ue->n_pdu_nssai == NR_PDCP_MAX_PDU_NSSAI) {
      LOG_W(PDCP, "UE %lx: no room for the S-NSSAI of PDU session %ld\n", ue_id, pdusession_id);
      nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
      return;
    }
    ue->n_pdu_nssai++;
  }
  ue->pdu_nssai[i].pdusession_id = pdusession_id;
  ue->pdu_nssai[i].sst = sst;
  ue->pdu_nssai[i].sd = sd;
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

int nr_pdcp_get_drb_for_nssai(ue_id_t ue_id, uint8_t sst, uint32_t sd)
{
  int found = -1;
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  for (int i = 0; i < ue->n_pdu_nssai; i++) {
    if (ue->pdu_nssai[i].sst != sst || ue->pdu_nssai[i].sd != sd)
      continue;
    for (int d = 0; d < MAX_DRBS_PER_UE; d++) {
      nr_pdcp_entity_t *drb = ue->drb[d];
      if (drb != NULL && drb->pdusession_id == ue->pdu_nssai[i].pdusession_id && (found < 0 || drb->rb_id < found))
        found = drb->rb_id;
    }
  }
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  return found;
}

uint64_t get_pdcp_optmask(void)
{
  return pdcp_optmask;
}

void nr_pdcp_remove_UE(ue_id_t ue_id)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_manager_remove_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_config_set_security(ue_id_t ue_id,
                                 rb_id_t rb_id,
                                 bool is_srb,
                                 const nr_pdcp_entity_security_keys_and_algos_t *parameters)
{
  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);

  rb = nr_pdcp_get_rb(ue, rb_id, is_srb);

  if (rb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "no %s found (ue_id %ld, rb_id %ld)\n", is_srb ? "SRB" : "DRB", ue_id, rb_id);
    return;
  }

  rb->set_security(rb, parameters);

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

bool nr_pdcp_check_integrity_srb(ue_id_t ue_id,
                                 int srb_id,
                                 const uint8_t *msg,
                                 int msg_size,
                                 const nr_pdcp_integrity_data_t *msg_integrity)
{
  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);

  rb = nr_pdcp_get_rb(ue, srb_id, true);

  if (rb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "no SRB found (ue_id %ld, rb_id %d)\n", ue_id, srb_id);
    return false;
  }

  bool ret = rb->check_integrity(rb, msg, msg_size, msg_integrity);

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  return ret;
}

bool nr_pdcp_data_req_srb(ue_id_t ue_id,
                          const rb_id_t rb_id,
                          const mui_t muiP,
                          const sdu_size_t sdu_buffer_size,
                          unsigned char *const sdu_buffer,
                          deliver_pdu deliver_pdu_cb,
                          void *data)
{
  LOG_D(PDCP, "%s() called, size %d\n", __func__, sdu_buffer_size);
  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  rb = nr_pdcp_get_rb(ue, rb_id, true);

  if (rb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "no SRB found (ue_id %ld, rb_id %ld)\n", ue_id, rb_id);
    return 0;
  }

  int max_size = nr_max_pdcp_pdu_size(sdu_buffer_size);
  char pdu_buf[max_size];
  int pdu_size = rb->process_sdu(rb, (char *)sdu_buffer, sdu_buffer_size, muiP, pdu_buf, max_size);
  if (pdu_size == -1) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return 0;
  }
  AssertFatal(rb->deliver_pdu == NULL, "SRB callback should be NULL, to be provided on every invocation\n");

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  deliver_pdu_cb(data, ue_id, rb_id, pdu_buf, pdu_size, muiP);

  return 1;
}

void nr_pdcp_suspend_srb(ue_id_t ue_id, int srb_id)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *srb = nr_pdcp_get_rb(ue, srb_id, true);
  if (srb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "Trying to suspend SRB with ID %d but it is not established\n", srb_id);
    return;
  }
  srb->suspend_entity(srb);
  LOG_D(PDCP, "SRB %d suspended\n", srb_id);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_suspend_drb(ue_id_t ue_id, int drb_id)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *drb = nr_pdcp_get_rb(ue, drb_id, false);
  if (drb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "Trying to suspend DRB with ID %d but it is not established\n", drb_id);
    return;
  }
  drb->suspend_entity(drb);
  LOG_D(PDCP, "DRB %d suspended\n", drb_id);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_reconfigure_srb(ue_id_t ue_id, int srb_id, long t_Reordering)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *srb = nr_pdcp_get_rb(ue, srb_id, true);
  if (srb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "Trying to reconfigure SRB with ID %d but it is not established\n", srb_id);
    return;
  }
  int decoded_t_reordering = decode_t_reordering(t_Reordering);
  srb->t_reordering = decoded_t_reordering;
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_reconfigure_drb(ue_id_t ue_id, int drb_id, NR_PDCP_Config_t *pdcp_config)
{
  // The enabling/disabling of ciphering or integrity protection
  // can be changed only by releasing and adding the DRB
  // (so not by reconfiguring).
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *drb = nr_pdcp_get_rb(ue, drb_id, false);
  if (drb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "Trying to reconfigure DRB with ID %d but it is not established\n", drb_id);
    return;
  }
  if (pdcp_config) {
    if (pdcp_config->t_Reordering)
      drb->t_reordering = decode_t_reordering(*pdcp_config->t_Reordering);
    else
      drb->t_reordering = -1;
    struct NR_PDCP_Config__drb *drb_config = pdcp_config->drb;
    if (drb_config) {
      if (drb_config->discardTimer)
        drb->discard_timer = decode_discard_timer(*drb_config->discardTimer);
      bool size_set = false;
      if (drb_config->pdcp_SN_SizeUL) {
        drb->sn_size = decode_sn_size_ul(*drb_config->pdcp_SN_SizeUL);
        size_set = true;
      }
      if (drb_config->pdcp_SN_SizeDL) {
        int size = decode_sn_size_dl(*drb_config->pdcp_SN_SizeDL);
        AssertFatal(!size_set || (size == drb->sn_size),
                    "SN sizes must be the same. dl=%d, ul=%d",
                    size, drb->sn_size);
        drb->sn_size = size;
      }
    }
  }
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_release_srb(ue_id_t ue_id, int srb_id)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  if (ue->srb[srb_id - 1] != NULL) {
    ue->srb[srb_id - 1]->delete_entity(ue->srb[srb_id - 1]);
    ue->srb[srb_id - 1] = NULL;
  }
  else
    LOG_E(PDCP, "Attempting to release SRB%d but it is not configured\n", srb_id);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_release_drb(ue_id_t ue_id, int drb_id)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *drb = ue->drb[drb_id - 1];
  if (drb) {
    nr_sdap_release_drb(ue_id, drb_id, drb->pdusession_id);
    drb->release_entity(drb);
    drb->delete_entity(drb);
    ue->drb[drb_id - 1] = NULL;
    LOG_I(PDCP, "release DRB %d of UE %ld\n", drb_id, ue_id);
  }
  else
    LOG_E(PDCP, "Attempting to release DRB%d but it is not configured\n", drb_id);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

void nr_pdcp_reestablishment(ue_id_t ue_id,
                             int rb_id,
                             bool srb_flag,
                             const nr_pdcp_entity_security_keys_and_algos_t *security_parameters)
{
  nr_pdcp_ue_t     *ue;
  nr_pdcp_entity_t *rb;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  rb = nr_pdcp_get_rb(ue, rb_id, srb_flag);

  if (rb != NULL) {
    LOG_D(PDCP, "UE %4.4lx re-establishment of %sRB %d\n", ue_id, srb_flag ? "S" : "D", rb_id);
    rb->reestablish_entity(rb, security_parameters);
    LOG_I(PDCP, "%s %d re-established\n", srb_flag ? "SRB" : "DRB" , rb_id);
  } else {
    LOG_W(PDCP, "UE %4.4lx cannot re-establish %sRB %d, RB not found\n", ue_id, srb_flag ? "S" : "D", rb_id);
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}

/** @brief Run one downlink SDU through PDCP and hand the PDU to the lower
 * layer (F1-U, or RLC in a monolithic gNB).
 *
 * Called with the manager lock held, and releases it: the PDU is delivered
 * outside the lock. The COUNT is assigned under the lock, so SDUs processed in
 * the order they were taken get consecutive COUNTs. */
static bool dl_process_and_deliver_locked(ue_id_t ue_id,
                                          nr_pdcp_entity_t *rb,
                                          rb_id_t rb_id,
                                          mui_t mui,
                                          const unsigned char *sdu,
                                          int size,
                                          uint64_t t_in_us)
{
  int max_size = nr_max_pdcp_pdu_size(size);
  char pdu_buf[max_size];
  int pdu_size = rb->process_sdu(rb, (char *)sdu, size, mui, pdu_buf, max_size);
  if (pdu_size == -1) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return false;
  }

  deliver_pdu deliver_pdu_cb = rb->deliver_pdu;

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  deliver_pdu_cb(NULL, ue_id, rb_id, pdu_buf, pdu_size, mui);

  /* The SDU has now left towards the DU (F1-U send returned, or queued to RLC in
   * a monolithic gNB): record its delay, TS 28.552 5.1.3.3.1. The entity is
   * looked up again because the lock was dropped for the send. */
  const uint64_t t_out_us = time_average_now();
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  rb = nr_pdcp_get_rb(ue, rb_id, false);
  if (rb != NULL && rb->txsdu_delay != NULL && t_out_us >= t_in_us)
    time_average_add(rb->txsdu_delay, t_out_us, t_out_us - t_in_us);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  return true;
}

/* ---- change of gNB-CU-UP with data forwarding (TS 38.401 8.9.5 step 9) ---- */

static void held_push(nr_pdcp_held_queue_t *q, const unsigned char *sdu, int size, int sdu_id, uint64_t t_in_us)
{
  nr_pdcp_held_sdu_t *e = malloc_or_fail(sizeof(*e) + size);
  e->next = NULL;
  e->t_in_us = t_in_us;
  e->sdu_id = sdu_id;
  e->size = size;
  memcpy(e->data, sdu, size);
  if (q->tail != NULL)
    q->tail->next = e;
  else
    q->head = e;
  q->tail = e;
  q->count++;
}

static nr_pdcp_held_sdu_t *held_pop(nr_pdcp_held_queue_t *q)
{
  nr_pdcp_held_sdu_t *e = q->head;
  if (e == NULL)
    return NULL;
  q->head = e->next;
  if (q->head == NULL)
    q->tail = NULL;
  q->count--;
  return e;
}

static int env_ms(const char *name, int def)
{
  const char *v = getenv(name);
  if (v == NULL || *v == 0)
    return def;
  int ms = atoi(v);
  return ms > 0 ? ms : def;
}

/** Release the new-path SDUs once the forwarding tunnel has been idle this long
 * after the new N3 path came up. Stands in for the End Marker, which the UPF
 * only sends when the SMF asks for it. */
static uint64_t dl_fwd_idle_us(void)
{
  static int ms = -1;
  if (ms < 0)
    ms = env_ms("OAI_CUUP_FWD_IDLE_MS", 5);
  return (uint64_t)ms * 1000;
}

/** Upper bound on holding new-path SDUs, counted from the COUNT arriving. */
static uint64_t dl_fwd_max_hold_us(void)
{
  static int ms = -1;
  if (ms < 0)
    ms = env_ms("OAI_CUUP_FWD_MAX_HOLD_MS", 200);
  return (uint64_t)ms * 1000;
}

/** Upper bound on waiting for the COUNT at all: past this the change of
 * gNB-CU-UP has been given up, and the target sends what it holds. */
#define DL_FWD_WAIT_COUNT_MAX_US (2 * 1000 * 1000)

/** Send what an entity holds, in order: forwarded SDUs first (they are older),
 * then, once forwarding is over, the SDUs held from the new N3 path. */
static void dl_fwd_pump(ue_id_t ue_id, rb_id_t rb_id)
{
  for (;;) {
    nr_pdcp_manager_lock(nr_pdcp_ue_manager);
    nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
    nr_pdcp_entity_t *rb = nr_pdcp_get_rb(ue, rb_id, false);
    if (rb == NULL) {
      nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
      return;
    }
    nr_pdcp_held_sdu_t *e = NULL;
    if (rb->dl_fwd.mode != NR_PDCP_DL_FWD_TARGET_WAIT)
      e = held_pop(&rb->dl_fwd.fwd_q);
    if (e == NULL && rb->dl_fwd.mode == NR_PDCP_DL_FWD_NONE)
      e = held_pop(&rb->dl_fwd.n3_q);
    if (e == NULL) {
      nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
      return;
    }
    dl_process_and_deliver_locked(ue_id, rb, rb_id, e->sdu_id, e->data, e->size, e->t_in_us);
    free(e);
  }
}

/** Entities on the target that are waiting for forwarding to end. */
#define DL_FWD_MAX_WATCH 64
static struct {
  bool used;
  ue_id_t ue_id;
  rb_id_t rb_id;
} dl_fwd_watch[DL_FWD_MAX_WATCH];
static pthread_mutex_t dl_fwd_watch_lock = PTHREAD_MUTEX_INITIALIZER;

/** @brief Check whether forwarding to one target entity is over, and if so go
 * back to normal operation and send what is held.
 * @return true when the entity no longer needs watching */
static bool dl_fwd_check(ue_id_t ue_id, rb_id_t rb_id, uint64_t now)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *rb = nr_pdcp_get_rb(ue, rb_id, false);
  if (rb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return true;
  }
  nr_pdcp_dl_fwd_t *f = &rb->dl_fwd;
  const char *reason = NULL;
  if (f->mode == NR_PDCP_DL_FWD_TARGET_WAIT) {
    if (now - f->t_start_us > DL_FWD_WAIT_COUNT_MAX_US)
      reason = "no COUNT from the source";
  } else if (f->mode == NR_PDCP_DL_FWD_TARGET_DRAIN) {
    const uint64_t last = f->t_last_fwd_us > f->t_count_us ? f->t_last_fwd_us : f->t_count_us;
    if (f->end_marker)
      reason = "End Marker";
    else if (f->t_first_n3_us != 0 && now - last >= dl_fwd_idle_us())
      reason = "forwarding tunnel idle";
    else if (now - f->t_count_us >= dl_fwd_max_hold_us())
      reason = "hold limit reached";
  } else {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return true;
  }
  if (reason == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return false;
  }
  f->mode = NR_PDCP_DL_FWD_NONE;
  LOG_I(PDCP,
        "UE %lx DRB %ld: downlink forwarding over (%s): %u SDUs forwarded by the source, %u held from the new path for %.1f ms\n",
        ue_id,
        rb_id,
        reason,
        f->n_forwarded,
        f->n_held,
        f->t_first_n3_us != 0 && now > f->t_first_n3_us ? (now - f->t_first_n3_us) / 1000.0 : 0.0);
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  dl_fwd_pump(ue_id, rb_id);
  return true;
}

static void *dl_fwd_watch_thread(void *arg)
{
  (void)arg;
  for (;;) {
    usleep(1000);
    ue_id_t ues[DL_FWD_MAX_WATCH];
    rb_id_t rbs[DL_FWD_MAX_WATCH];
    int idx[DL_FWD_MAX_WATCH];
    int n = 0;
    pthread_mutex_lock(&dl_fwd_watch_lock);
    for (int i = 0; i < DL_FWD_MAX_WATCH; i++) {
      if (!dl_fwd_watch[i].used)
        continue;
      ues[n] = dl_fwd_watch[i].ue_id;
      rbs[n] = dl_fwd_watch[i].rb_id;
      idx[n] = i;
      n++;
    }
    pthread_mutex_unlock(&dl_fwd_watch_lock);
    const uint64_t now = time_average_now();
    for (int k = 0; k < n; k++) {
      if (!dl_fwd_check(ues[k], rbs[k], now))
        continue;
      pthread_mutex_lock(&dl_fwd_watch_lock);
      if (dl_fwd_watch[idx[k]].used && dl_fwd_watch[idx[k]].ue_id == ues[k] && dl_fwd_watch[idx[k]].rb_id == rbs[k])
        dl_fwd_watch[idx[k]].used = false;
      pthread_mutex_unlock(&dl_fwd_watch_lock);
    }
  }
  return NULL;
}

static pthread_once_t dl_fwd_thread_once = PTHREAD_ONCE_INIT;
static void dl_fwd_start_thread(void)
{
  pthread_t t;
  threadCreate(&t, dl_fwd_watch_thread, NULL, "pdcp_dl_fwd", -1, OAI_PRIORITY_RT_LOW);
}

static void dl_fwd_watch_add(ue_id_t ue_id, rb_id_t rb_id)
{
  pthread_once(&dl_fwd_thread_once, dl_fwd_start_thread);
  pthread_mutex_lock(&dl_fwd_watch_lock);
  int free_slot = -1;
  for (int i = 0; i < DL_FWD_MAX_WATCH; i++) {
    if (dl_fwd_watch[i].used && dl_fwd_watch[i].ue_id == ue_id && dl_fwd_watch[i].rb_id == rb_id) {
      pthread_mutex_unlock(&dl_fwd_watch_lock);
      return;
    }
    if (!dl_fwd_watch[i].used && free_slot < 0)
      free_slot = i;
  }
  if (free_slot >= 0)
    dl_fwd_watch[free_slot] = (typeof(dl_fwd_watch[0])){.used = true, .ue_id = ue_id, .rb_id = rb_id};
  else
    LOG_E(PDCP, "UE %lx DRB %ld: too many DRBs waiting for downlink forwarding, it will end at the next packet\n", ue_id, rb_id);
  pthread_mutex_unlock(&dl_fwd_watch_lock);
}

/** @brief The UPF closed the old N3 tunnel of a PDU session (End Marker, TS
 * 29.281 clause 7.3.2): pass it on to the target for every DRB being relayed. */
static void dl_fwd_end_marker_from_core(ue_id_t ue_id, long pdusession_id)
{
  int drb_ids[MAX_DRBS_PER_UE];
  nr_pdcp_dl_fwd_send_t sends[MAX_DRBS_PER_UE];
  int n = 0;
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  for (int i = 0; ue != NULL && i < MAX_DRBS_PER_UE; i++) {
    nr_pdcp_entity_t *drb = ue->drb[i];
    if (drb == NULL || drb->pdusession_id != pdusession_id || drb->dl_fwd.mode != NR_PDCP_DL_FWD_SOURCE)
      continue;
    drb_ids[n] = drb->rb_id;
    sends[n] = drb->dl_fwd.send;
    n++;
  }
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  if (n == 0)
    LOG_D(PDCP, "UE %lx: End Marker on PDU session %ld, no DRB is being relayed\n", ue_id, pdusession_id);
  for (int i = 0; i < n; i++) {
    LOG_I(PDCP, "UE %lx DRB %d: End Marker from the core, passing it on to the target gNB-CU-UP\n", ue_id, drb_ids[i]);
    sends[i](ue_id, drb_ids[i], NULL, 0);
  }
}

bool nr_pdcp_dl_fwd_start_source(ue_id_t ue_id,
                                 rb_id_t drb_id,
                                 nr_pdcp_dl_fwd_send_t send,
                                 nr_pdcp_count_t *ul_count,
                                 nr_pdcp_count_t *dl_count)
{
  DevAssert(send != NULL);
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *rb = nr_pdcp_get_rb(ue, drb_id, false);
  if (rb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "UE %lx: no DRB %ld to forward downlink from\n", ue_id, drb_id);
    return false;
  }
  /* Read the COUNT and switch to relaying in one go: no SDU may be numbered
   * here after the COUNT the target will continue from. */
  *dl_count = rb->get_pdcp_count_dl(rb);
  *ul_count = rb->get_pdcp_count_ul(rb);
  rb->dl_fwd.mode = NR_PDCP_DL_FWD_SOURCE;
  rb->dl_fwd.send = send;
  rb->dl_fwd.t_start_us = time_average_now();
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  LOG_I(PDCP,
        "UE %lx DRB %ld: downlink stops at COUNT %u/%u, relaying what still arrives to the target gNB-CU-UP\n",
        ue_id,
        drb_id,
        dl_count->hfn,
        dl_count->sn);
  return true;
}

void nr_pdcp_dl_fwd_expect(ue_id_t ue_id, rb_id_t drb_id)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *rb = nr_pdcp_get_rb(ue, drb_id, false);
  if (rb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "UE %lx: no DRB %ld to receive forwarded downlink on\n", ue_id, drb_id);
    return;
  }
  rb->dl_fwd.mode = NR_PDCP_DL_FWD_TARGET_WAIT;
  rb->dl_fwd.t_start_us = time_average_now();
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  dl_fwd_watch_add(ue_id, drb_id);
}

bool nr_pdcp_dl_fwd_rx(protocol_ctxt_t *ctxt_pP,
                       const srb_flag_t srb_flagP,
                       const rb_id_t rb_id,
                       const mui_t muiP,
                       const confirm_t confirmP,
                       const sdu_size_t sdu_buffer_size,
                       unsigned char *const sdu_buffer,
                       const pdcp_transmission_mode_t mode,
                       const uint32_t *const sourceL2Id,
                       const uint32_t *const destinationL2Id)
{
  DevAssert(srb_flagP == SRB_FLAG_NO);
  const ue_id_t ue_id = ctxt_pP->rntiMaybeUEid;
  uint64_t t_in_us = gtpv1u_current_rx_time_us();
  const uint64_t now = time_average_now();
  if (t_in_us == 0)
    t_in_us = now;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  nr_pdcp_entity_t *rb = nr_pdcp_get_rb(ue, rb_id, false);
  if (rb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_W(PDCP, "UE %lx: forwarded downlink for unknown DRB %ld dropped\n", ue_id, rb_id);
    return false;
  }
  nr_pdcp_dl_fwd_t *f = &rb->dl_fwd;

  if (sdu_buffer_size == 0) { /* End Marker relayed by the source */
    f->end_marker = true;
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    dl_fwd_check(ue_id, rb_id, now);
    return true;
  }

  f->n_forwarded++;
  f->t_last_fwd_us = now;
  if (f->mode == NR_PDCP_DL_FWD_TARGET_WAIT || f->fwd_q.count > 0) {
    held_push(&f->fwd_q, sdu_buffer, sdu_buffer_size, muiP, t_in_us);
    const bool can_send = f->mode != NR_PDCP_DL_FWD_TARGET_WAIT;
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    if (can_send)
      dl_fwd_pump(ue_id, rb_id);
    return true;
  }
  /* forwarding is under way (or a late straggler after it ended): send now */
  return dl_process_and_deliver_locked(ue_id, rb, rb_id, muiP, sdu_buffer, sdu_buffer_size, t_in_us);
}

bool nr_pdcp_data_req_drb(protocol_ctxt_t *ctxt_pP,
                          const srb_flag_t srb_flagP,
                          const rb_id_t rb_id,
                          const mui_t muiP,
                          const confirm_t confirmP,
                          const sdu_size_t sdu_buffer_size,
                          unsigned char *const sdu_buffer,
                          const pdcp_transmission_mode_t mode,
                          const uint32_t *const sourceL2Id,
                          const uint32_t *const destinationL2Id)
{
  DevAssert(srb_flagP == SRB_FLAG_NO);

  /* Start of this SDU's DL time in the gNB: its NG-U arrival when it came from
   * GTP-U on this thread (kernel receive stamp), otherwise now. */
  uint64_t t_in_us = gtpv1u_current_rx_time_us();
  if (t_in_us == 0)
    t_in_us = time_average_now();

  LOG_D(PDCP, "%s() called, size %d\n", __func__, sdu_buffer_size);
  nr_pdcp_ue_t *ue;
  nr_pdcp_entity_t *rb;
  ue_id_t ue_id = ctxt_pP->rntiMaybeUEid;

  if (ctxt_pP->module_id != 0 ||
      //ctxt_pP->enb_flag != 1 ||
      ctxt_pP->instance != 0 ||
      ctxt_pP->eNB_index != 0 /*||
      ctxt_pP->configured != 1 ||
      ctxt_pP->brOption != 0*/) {
    LOG_E(PDCP, "%s:%d:%s: fatal\n", __FILE__, __LINE__, __FUNCTION__);
    exit(1);
  }

  /* An End Marker on N3 reaches here with no payload, and GTP-U gives the PDU
   * session ID of the tunnel as rb_id. */
  if (sdu_buffer_size == 0) {
    dl_fwd_end_marker_from_core(ue_id, rb_id);
    return 1;
  }

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  rb = nr_pdcp_get_rb(ue, rb_id, false);

  if (rb == NULL) {
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    LOG_E(PDCP, "[UE %lx] DRB %ld not found\n", ue_id, rb_id);
    return 0;
  }

  nr_pdcp_dl_fwd_t *f = &rb->dl_fwd;
  switch (f->mode) {
    case NR_PDCP_DL_FWD_SOURCE: {
      /* source of a change of gNB-CU-UP: relay, the target numbers it */
      nr_pdcp_dl_fwd_send_t send = f->send;
      f->n_relayed++;
      nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
      send(ue_id, rb_id, sdu_buffer, sdu_buffer_size);
      return 1;
    }
    case NR_PDCP_DL_FWD_TARGET_WAIT:
    case NR_PDCP_DL_FWD_TARGET_DRAIN:
      /* target: the new path is live, but what the source forwards goes first */
      if (f->t_first_n3_us == 0)
        f->t_first_n3_us = time_average_now();
      f->n_held++;
      held_push(&f->n3_q, sdu_buffer, sdu_buffer_size, muiP, t_in_us);
      nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
      return 1;
    case NR_PDCP_DL_FWD_NONE:
    default:
      if (f->n3_q.count > 0 || f->fwd_q.count > 0) {
        /* still sending what was held: queue behind it to keep the order */
        held_push(&f->n3_q, sdu_buffer, sdu_buffer_size, muiP, t_in_us);
        nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
        dl_fwd_pump(ue_id, rb_id);
        return 1;
      }
      break;
  }

  return dl_process_and_deliver_locked(ue_id, rb, rb_id, muiP, sdu_buffer, sdu_buffer_size, t_in_us);
}

bool cu_f1u_data_req(protocol_ctxt_t  *ctxt_pP,
                     const srb_flag_t srb_flagP,
                     const rb_id_t rb_id,
                     const mui_t muiP,
                     const confirm_t confirmP,
                     const sdu_size_t sdu_buffer_size,
                     unsigned char *const sdu_buffer,
                     const pdcp_transmission_mode_t mode,
                     const uint32_t *const sourceL2Id,
                     const uint32_t *const destinationL2Id)
{
  do_pdcp_data_ind(ctxt_pP, srb_flagP, rb_id, sdu_buffer_size, sdu_buffer);
  return true;
}

/*
 * For the SDAP API
 */
nr_pdcp_ue_manager_t *nr_pdcp_sdap_get_ue_manager() {
  return nr_pdcp_ue_manager;
}

/* returns false in case of error, true if everything ok */
bool nr_pdcp_get_statistics(ue_id_t ue_id, int srb_flag, int rb_id, nr_pdcp_statistics_t *out)
{
  nr_pdcp_ue_t     *ue;
  nr_pdcp_entity_t *rb;
  bool             ret;

  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  rb = nr_pdcp_get_rb(ue, rb_id, srb_flag);

  if (rb != NULL) {
    rb->get_stats(rb, out);
    ret = true;
  } else {
    ret = false;
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  return ret;
}

int nr_pdcp_get_num_ues(ue_id_t *ue_list, int len)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);
  int num_ues = nr_pdcp_manager_get_ue_count(nr_pdcp_ue_manager);
  nr_pdcp_ue_t **nr_pdcp_ue_list = nr_pdcp_manager_get_ue_list(nr_pdcp_ue_manager);
  for (int i = 0; i < num_ues && i < len; i++)
    ue_list[i] = nr_pdcp_ue_list[i]->ue_id;
  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
  
  return num_ues;
}

void nr_pdcp_count_update(ue_id_t ue_id,
                          rb_id_t drb_id,
                          nr_pdcp_count_t dl_count,
                          nr_pdcp_count_t ul_count,
                          int sn_size)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);
  if (ue == NULL) {
    LOG_E(PDCP, "UE %ld not found in PDCP manager\n", ue_id);
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return;
  }

  nr_pdcp_entity_t *entity = nr_pdcp_get_rb(ue, drb_id, false);
  if (entity == NULL) {
    LOG_E(PDCP, "No PDCP entity found for UE %ld DRB %ld\n", ue_id, drb_id);
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return;
  }

  entity->set_pdcp_count_dl(entity, dl_count, sn_size);
  entity->set_pdcp_count_ul(entity, ul_count, sn_size);

  /* target of a change of gNB-CU-UP: forwarded SDUs can go out from here on */
  const bool fwd_ready = entity->dl_fwd.mode == NR_PDCP_DL_FWD_TARGET_WAIT;
  if (fwd_ready) {
    entity->dl_fwd.mode = NR_PDCP_DL_FWD_TARGET_DRAIN;
    entity->dl_fwd.t_count_us = time_average_now();
    LOG_I(PDCP,
          "UE %lx DRB %ld: downlink continues from COUNT %u/%u, %d forwarded SDU(s) waiting\n",
          ue_id,
          drb_id,
          dl_count.hfn,
          dl_count.sn,
          entity->dl_fwd.fwd_q.count);
  }

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);

  if (fwd_ready)
    dl_fwd_pump(ue_id, drb_id);
}

/** * @brief Get PDCP COUNT values (PDCP-SN and HFN) for UL and DL from the PDCP entity
 * @param[in]  ue_id          Unique UE ID
 * @param[in]  rb_id          Radio Bearer ID
 * @param[out] ul_count       UL PDCP count (SN, HFN)
 * @param[out] dl_count       DL PDCP count (SN, HFN)
 */
void nr_pdcp_get_drb_count_values(ue_id_t ue_id, rb_id_t rb_id, nr_pdcp_count_t *ul_count, nr_pdcp_count_t *dl_count)
{
  nr_pdcp_manager_lock(nr_pdcp_ue_manager);

  nr_pdcp_ue_t *ue = nr_pdcp_manager_get_ue(nr_pdcp_ue_manager, ue_id);

  nr_pdcp_entity_t *entity = nr_pdcp_get_rb(ue, rb_id, false);

  if (!entity) {
    LOG_E(PDCP, "No entity found (DRB %ld)\n", rb_id);
    nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
    return;
  }

  *dl_count = entity->get_pdcp_count_dl(entity);
  *ul_count = entity->get_pdcp_count_ul(entity);

  nr_pdcp_manager_unlock(nr_pdcp_ue_manager);
}
