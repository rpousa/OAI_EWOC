/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <stdlib.h> // abort
#include <stdint.h> // uint*_t
#include <stdio.h> // printf
#include <stdbool.h> // bool
#include <string.h> // memcmp
#include <arpa/inet.h> // inet_pton

#include "common/utils/assertions.h" // AssertFatal, DevAssert
#include "common/utils/utils.h" // malloc_or_fail, calloc_or_fail
#include "common/utils/ds/byte_array.h" // create_byte_array
#include "conversions.h" // TBCD_TO_MCC_MNC, OCTET_STRING_TO_INT*
#include "ngap_common.h" // NGAP_ERROR, NGAP_DEBUG, ngap_messages_types.h
#include "ngap_gNB_paging.h" // encode_ng_paging, decode_ng_paging, free_ng_paging
#include "ngap_gNB_mobility_management.h" // encode/decode of the Modify Indication and Confirm
#include "ngap_msg_includes.h" // NGAP_NGAP_PDU_t, NGAP_Paging_t, etc.
#include "oai_asn1.h" // ASN_STRUCT_FREE, asn_DEF_NGAP_NGAP_PDU
#include "config/config_load_configmodule.h" // configmodule_interface_t
#include "common/utils/LOG/log.h" // log_init

void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  UNUSED(assert);
  printf("detected error at %s:%d:%s: %s\n", file, line, function, s);
  abort();
}

static NGAP_NGAP_PDU_t *ngap_encode_decode(const NGAP_NGAP_PDU_t *enc_pdu)
{
  DevAssert(enc_pdu != NULL);
  char errbuf[1024];
  size_t errlen = sizeof(errbuf);
  int ret = asn_check_constraints(&asn_DEF_NGAP_NGAP_PDU, enc_pdu, errbuf, &errlen);
  AssertFatal(ret == 0, "asn_check_constraints() failed: %s\n", errbuf);

  uint8_t msgbuf[16384];
  asn_enc_rval_t enc = aper_encode_to_buffer(&asn_DEF_NGAP_NGAP_PDU, NULL, enc_pdu, msgbuf, sizeof(msgbuf));
  AssertFatal(enc.encoded > 0, "aper_encode_to_buffer() failed\n");

  NGAP_NGAP_PDU_t *dec_pdu = NULL;
  asn_codec_ctx_t st = {.max_stack_size = 100 * 1000};
  asn_dec_rval_t dec = aper_decode(&st, &asn_DEF_NGAP_NGAP_PDU, (void **)&dec_pdu, msgbuf, enc.encoded, 0, 0);
  AssertFatal(dec.code == RC_OK, "aper_decode() failed\n");

  return dec_pdu;
}

static void ngap_msg_free(NGAP_NGAP_PDU_t *pdu)
{
  ASN_STRUCT_FREE(asn_DEF_NGAP_NGAP_PDU, pdu);
}

/** @brief Test NGAP Paging encoding/decoding roundtrip */
static void test_ngap_paging_encode_decode(void)
{
  // Create test paging indication
  ngap_paging_ind_t orig = {0};
  orig.ue_paging_identity.s_tmsi.amf_set_id = 123; // Valid range: 0-1023 (10 bits)
  orig.ue_paging_identity.s_tmsi.amf_pointer = 45; // Valid range: 0-63 (6 bits)
  orig.ue_paging_identity.s_tmsi.m_tmsi = 0x12345678;
  orig.n_tai = 2;
  orig.tai_list[0].plmn.mcc = 208;
  orig.tai_list[0].plmn.mnc = 95;
  orig.tai_list[0].plmn.mnc_digit_length = 2;
  orig.tai_list[0].tac = 12345;
  orig.tai_list[1].plmn.mcc = 001;
  orig.tai_list[1].plmn.mnc = 01;
  orig.tai_list[1].plmn.mnc_digit_length = 2;
  orig.tai_list[1].tac = 23456;
  orig.paging_drx = malloc_or_fail(sizeof(ngap_paging_drx_t));
  *orig.paging_drx = NGAP_PAGING_DRX_128;
  orig.paging_priority = malloc_or_fail(sizeof(ngap_paging_priority_t));
  *orig.paging_priority = NGAP_PAGING_PRIO_LEVEL2;
  uint8_t ue_cap_data[] = {0x01, 0x02, 0x03, 0x04};
  orig.ue_radio_capability = malloc_or_fail(sizeof(byte_array_t));
  *orig.ue_radio_capability = create_byte_array(sizeof(ue_cap_data), ue_cap_data);
  orig.origin = calloc_or_fail(1, sizeof(*orig.origin));
  *orig.origin = NGAP_PAGING_ORIGIN_NON_3GPP;

  // Encode
  NGAP_NGAP_PDU_t *enc_pdu = encode_ng_paging(&orig);
  AssertFatal(enc_pdu != NULL, "encode_ng_paging(): failed to encode message\n");
  LOG_A(NGAP, "encoded: m_tmsi=0x%x, n_tai=%d\n", orig.ue_paging_identity.s_tmsi.m_tmsi, orig.n_tai);

  // Encode/decode roundtrip
  NGAP_NGAP_PDU_t *dec_pdu = ngap_encode_decode(enc_pdu);
  ngap_msg_free(enc_pdu);

  // Decode
  ngap_paging_ind_t decoded = {0};
  bool decode_ret = decode_ng_paging(&decoded, dec_pdu);
  AssertFatal(decode_ret, "decode_ng_paging(): could not decode message\n");
  ngap_msg_free(dec_pdu);
  LOG_A(NGAP, "decoded: m_tmsi=0x%x, n_tai=%d\n", decoded.ue_paging_identity.s_tmsi.m_tmsi, decoded.n_tai);

  // Verify decoded values using equality function
  bool eq_ret = eq_ng_paging(&orig, &decoded);
  AssertFatal(eq_ret, "eq_ng_paging(): decoded message doesn't match original\n");
  LOG_A(NGAP, "eq_ng_paging(): decoded message matches original\n");

  free_ng_paging(&decoded);
  free_ng_paging(&orig);
}

/** @brief The NG-RAN node announcing a new downlink endpoint: check that the
 * mandatory IEs of the Modify Indication survive an encode/decode roundtrip.
 *
 * There is no decoder for this message -- an NG-RAN node only ever sends it --
 * so the decoded ASN.1 is inspected directly. */
static void test_ngap_pdusession_modify_indication_encode(void)
{
  ngap_pdusession_modify_ind_t orig = {
      .amf_ue_ngap_id = 0x1234567,
      .gNB_ue_ngap_id = 0x89ab,
      .nb_of_pdusessions = 1,
  };
  orig.pdusessions[0].pdusession_id = 5;
  orig.pdusessions[0].n3_outgoing.teid = 0xdeadbeef;
  orig.pdusessions[0].n3_outgoing.addr.length = 32;
  inet_pton(AF_INET, "192.168.71.161", orig.pdusessions[0].n3_outgoing.addr.buffer);
  orig.pdusessions[0].nb_of_qos_flow = 2;
  orig.pdusessions[0].qfi[0] = 1;
  orig.pdusessions[0].qfi[1] = 9;

  NGAP_NGAP_PDU_t *enc_pdu = encode_ngap_pdusession_modify_indication(&orig);
  AssertFatal(enc_pdu != NULL, "encode_ngap_pdusession_modify_indication(): failed to encode message\n");

  NGAP_NGAP_PDU_t *dec_pdu = ngap_encode_decode(enc_pdu);
  ngap_msg_free(enc_pdu);

  AssertFatal(dec_pdu->present == NGAP_NGAP_PDU_PR_initiatingMessage, "not an initiating message\n");
  NGAP_PDUSessionResourceModifyIndication_t *container =
      &dec_pdu->choice.initiatingMessage->value.choice.PDUSessionResourceModifyIndication;
  NGAP_PDUSessionResourceModifyIndicationIEs_t *ie = NULL;
  NGAP_FIND_PROTOCOLIE_BY_ID(NGAP_PDUSessionResourceModifyIndicationIEs_t,
                             ie,
                             container,
                             NGAP_ProtocolIE_ID_id_PDUSessionResourceModifyListModInd,
                             true);
  AssertFatal(ie != NULL, "no PDU Session Resource Modify List in the encoded message\n");
  AssertFatal(ie->value.choice.PDUSessionResourceModifyListModInd.list.count == 1, "expected one PDU session\n");

  NGAP_PDUSessionResourceModifyItemModInd_t *item = ie->value.choice.PDUSessionResourceModifyListModInd.list.array[0];
  AssertFatal(item->pDUSessionID == orig.pdusessions[0].pdusession_id, "PDU session ID does not match\n");

  NGAP_PDUSessionResourceModifyIndicationTransfer_t *transfer = NULL;
  asn_dec_rval_t dec = aper_decode(NULL,
                                   &asn_DEF_NGAP_PDUSessionResourceModifyIndicationTransfer,
                                   (void **)&transfer,
                                   item->pDUSessionResourceModifyIndicationTransfer.buf,
                                   item->pDUSessionResourceModifyIndicationTransfer.size,
                                   0,
                                   0);
  AssertFatal(dec.code == RC_OK, "could not decode the Modify Indication transfer\n");

  const NGAP_UPTransportLayerInformation_t *up = &transfer->dLQosFlowPerTNLInformation.uPTransportLayerInformation;
  AssertFatal(up->present == NGAP_UPTransportLayerInformation_PR_gTPTunnel, "downlink endpoint is not a GTP tunnel\n");
  uint32_t teid = 0;
  OCTET_STRING_TO_INT32(&up->choice.gTPTunnel->gTP_TEID, teid);
  AssertFatal(teid == orig.pdusessions[0].n3_outgoing.teid, "TEID 0x%x does not match 0x%x\n", teid, orig.pdusessions[0].n3_outgoing.teid);

  int n_flows = transfer->dLQosFlowPerTNLInformation.associatedQosFlowList.list.count;
  AssertFatal(n_flows == orig.pdusessions[0].nb_of_qos_flow, "expected %d QoS flows, got %d\n", orig.pdusessions[0].nb_of_qos_flow, n_flows);
  for (int i = 0; i < n_flows; i++)
    AssertFatal(transfer->dLQosFlowPerTNLInformation.associatedQosFlowList.list.array[i]->qosFlowIdentifier == orig.pdusessions[0].qfi[i],
                "QoS flow %d does not match\n",
                i);

  ASN_STRUCT_FREE(asn_DEF_NGAP_PDUSessionResourceModifyIndicationTransfer, transfer);
  ngap_msg_free(dec_pdu);
  LOG_A(NGAP, "encode_ngap_pdusession_modify_indication(): mandatory IEs survive the roundtrip\n");
}

/** @brief The core answering with the uplink endpoint to use, which is a new one
 * when the SMF relocated the UPF. Builds the Confirm the way an AMF would and
 * checks that the decoder picks the endpoint up. */
static void test_ngap_pdusession_modify_confirm_decode(void)
{
  const uint32_t ul_teid = 0x0badf00d;
  const int pdusession_id = 5;
  const uint32_t gnb_ue_ngap_id = 0x89ab;
  const uint64_t amf_ue_ngap_id = 0x1234567;

  /* the transfer the AMF puts in the Confirm */
  NGAP_PDUSessionResourceModifyConfirmTransfer_t transfer = {0};
  transfer.uLNGU_UP_TNLInformation.present = NGAP_UPTransportLayerInformation_PR_gTPTunnel;
  asn1cCalloc(transfer.uLNGU_UP_TNLInformation.choice.gTPTunnel, tunnel);
  GTP_TEID_TO_ASN1(ul_teid, &tunnel->gTP_TEID);
  transport_layer_addr_t upf = {.length = 32};
  inet_pton(AF_INET, "192.168.71.134", upf.buffer);
  tnl_to_bitstring(&tunnel->transportLayerAddress, upf);
  asn1cSequenceAdd(transfer.qosFlowModifyConfirmList.list, NGAP_QosFlowModifyConfirmItem_t, flow);
  flow->qosFlowIdentifier = 1;

  void *tbuf = NULL;
  ssize_t tlen = aper_encode_to_new_buffer(&asn_DEF_NGAP_PDUSessionResourceModifyConfirmTransfer, NULL, &transfer, &tbuf);
  AssertFatal(tlen > 0, "could not encode the Modify Confirm transfer\n");
  ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDUSessionResourceModifyConfirmTransfer, &transfer);

  /* the Confirm itself */
  NGAP_NGAP_PDU_t pdu = {0};
  pdu.present = NGAP_NGAP_PDU_PR_successfulOutcome;
  asn1cCalloc(pdu.choice.successfulOutcome, head);
  head->procedureCode = NGAP_ProcedureCode_id_PDUSessionResourceModifyIndication;
  head->criticality = NGAP_Criticality_reject;
  head->value.present = NGAP_SuccessfulOutcome__value_PR_PDUSessionResourceModifyConfirm;
  NGAP_PDUSessionResourceModifyConfirm_t *out = &head->value.choice.PDUSessionResourceModifyConfirm;
  {
    asn1cSequenceAdd(out->protocolIEs.list, NGAP_PDUSessionResourceModifyConfirmIEs_t, ie);
    ie->id = NGAP_ProtocolIE_ID_id_AMF_UE_NGAP_ID;
    ie->criticality = NGAP_Criticality_ignore;
    ie->value.present = NGAP_PDUSessionResourceModifyConfirmIEs__value_PR_AMF_UE_NGAP_ID;
    asn_uint642INTEGER(&ie->value.choice.AMF_UE_NGAP_ID, amf_ue_ngap_id);
  }
  {
    asn1cSequenceAdd(out->protocolIEs.list, NGAP_PDUSessionResourceModifyConfirmIEs_t, ie);
    ie->id = NGAP_ProtocolIE_ID_id_RAN_UE_NGAP_ID;
    ie->criticality = NGAP_Criticality_ignore;
    ie->value.present = NGAP_PDUSessionResourceModifyConfirmIEs__value_PR_RAN_UE_NGAP_ID;
    ie->value.choice.RAN_UE_NGAP_ID = gnb_ue_ngap_id;
  }
  {
    asn1cSequenceAdd(out->protocolIEs.list, NGAP_PDUSessionResourceModifyConfirmIEs_t, ie);
    ie->id = NGAP_ProtocolIE_ID_id_PDUSessionResourceModifyListModCfm;
    ie->criticality = NGAP_Criticality_ignore;
    ie->value.present = NGAP_PDUSessionResourceModifyConfirmIEs__value_PR_PDUSessionResourceModifyListModCfm;
    asn1cSequenceAdd(ie->value.choice.PDUSessionResourceModifyListModCfm.list, NGAP_PDUSessionResourceModifyItemModCfm_t, item);
    item->pDUSessionID = pdusession_id;
    item->pDUSessionResourceModifyConfirmTransfer.buf = tbuf;
    item->pDUSessionResourceModifyConfirmTransfer.size = tlen;
  }

  NGAP_NGAP_PDU_t *dec_pdu = ngap_encode_decode(&pdu);
  ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_NGAP_PDU, &pdu);

  ngap_pdusession_modify_confirm_t decoded = {0};
  bool ret = decode_ngap_pdusession_modify_confirm(&decoded, dec_pdu);
  AssertFatal(ret, "decode_ngap_pdusession_modify_confirm(): could not decode message\n");
  ngap_msg_free(dec_pdu);

  AssertFatal(decoded.gNB_ue_ngap_id == gnb_ue_ngap_id, "RAN UE NGAP ID does not match\n");
  AssertFatal(decoded.amf_ue_ngap_id == amf_ue_ngap_id, "AMF UE NGAP ID does not match\n");
  AssertFatal(decoded.nb_of_pdusessions == 1, "expected one PDU session, got %d\n", decoded.nb_of_pdusessions);
  AssertFatal(decoded.pdusessions[0].pdusession_id == pdusession_id, "PDU session ID does not match\n");
  AssertFatal(decoded.pdusessions[0].n3_incoming.teid == ul_teid,
              "uplink TEID 0x%x does not match 0x%x\n",
              decoded.pdusessions[0].n3_incoming.teid,
              ul_teid);
  AssertFatal(memcmp(decoded.pdusessions[0].n3_incoming.addr.buffer, upf.buffer, 4) == 0, "uplink address does not match\n");
  LOG_A(NGAP, "decode_ngap_pdusession_modify_confirm(): the uplink endpoint is picked up\n");
}

// Stub for uniqCfg required by logging/config system
configmodule_interface_t *uniqCfg = NULL;

int main(void)
{
  // Initialize logging system
  logInit();
  set_log(NGAP, OAILOG_INFO);

  test_ngap_paging_encode_decode();
  test_ngap_pdusession_modify_indication_encode();
  test_ngap_pdusession_modify_confirm_decode();
  LOG_A(NGAP, "All NGAP tests passed!\n");
  return 0;
}
