/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "asn1c/s1ap-message.h"
#include "nas/nas-message.h"
#include "gtp/gtp-xact.h"
#include "fd/fd-lib.h"

#include "mme-event.h"
#include "mme-sm.h"

#include "s1ap-handler.h"
#include "s1ap-path.h"
#include "sgsap-path.h"
#include "nas-security.h"
#include "nas-path.h"
#include "emm-handler.h"
#include "esm-handler.h"
#include "mme-gtp-path.h"
#include "mme-s11-handler.h"
#include "mme-fd-path.h"
#include "mme-s6a-handler.h"
#include "mme-path.h"

void mme_state_initial(ogs_fsm_t *s, mme_event_t *e)
{
    mme_sm_debug(e);

    ogs_assert(s);

    OGS_FSM_TRAN(s, &mme_state_operational);
}

void mme_state_final(ogs_fsm_t *s, mme_event_t *e)
{
    mme_sm_debug(e);

    ogs_assert(s);
}

void mme_state_operational(ogs_fsm_t *s, mme_event_t *e)
{
    int rv;
    char buf[OGS_ADDRSTRLEN];

    ogs_sock_t *sock = NULL;
    ogs_sockaddr_t *addr = NULL;
    mme_enb_t *enb = NULL;
    uint16_t max_num_of_ostreams = 0;

    s1ap_message_t s1ap_message;
    ogs_pkbuf_t *pkbuf = NULL;
    int rc;

    nas_message_t nas_message;
    enb_ue_t *enb_ue = NULL;
    mme_ue_t *mme_ue = NULL;

    mme_bearer_t *bearer = NULL;
    mme_bearer_t *default_bearer = NULL;
    mme_sess_t *sess = NULL;

    ogs_pkbuf_t *s6abuf = NULL;
    s6a_message_t *s6a_message = NULL;

    gtp_xact_t *xact = NULL;
    gtp_message_t gtp_message;

    mme_vlr_t *vlr = NULL;

    ogs_assert(e);
    mme_sm_debug(e);

    ogs_assert(s);

    switch (e->id) {
    case OGS_FSM_ENTRY_SIG:
        rv = mme_gtp_open();
        if (rv != OGS_OK) {
            ogs_error("Can't establish S11-GTP path");
            break;
        }
        rv = sgsap_open();
        if (rv != OGS_OK) {
            ogs_error("Can't establish SGsAP path");
            break;
        }
        rv = s1ap_open();
        if (rv != OGS_OK) {
            ogs_error("Can't establish S1AP path");
            break;
        }

        break;

    case OGS_FSM_EXIT_SIG:
        mme_gtp_close();
        sgsap_close();
        s1ap_close();

        break;

    case MME_EVT_S1AP_LO_ACCEPT:
        sock = e->sctp_sock;
        ogs_assert(sock);
        addr = e->sctp_addr;
        ogs_assert(addr);

        ogs_info("eNB-S1 accepted[%s] in master_sm module", 
            OGS_ADDR(addr, buf));
                
        enb = mme_enb_find_by_addr(addr);
        if (!enb) {
            enb = mme_enb_add(sock, addr);
            ogs_assert(enb);
        } else {
            ogs_warn("eNB context duplicated with IP-address [%s]!!!", 
                    OGS_ADDR(addr, buf));
            ogs_sock_destroy(sock);
            ogs_warn("S1 Socket Closed");
        }

        break;

    case MME_EVT_S1AP_LO_SCTP_COMM_UP:
        sock = e->sctp_sock;
        ogs_assert(sock);
        addr = e->sctp_addr;
        ogs_assert(addr);

        max_num_of_ostreams = e->max_num_of_ostreams;

        enb = mme_enb_find_by_addr(addr);
        if (!enb) {
            enb = mme_enb_add(sock, addr);
            ogs_assert(enb);
        } else {
            ogs_free(addr);
        }

        enb->max_num_of_ostreams =
                ogs_min(max_num_of_ostreams, enb->max_num_of_ostreams);

        ogs_debug("eNB-S1 SCTP_COMM_UP[%s] Max Num of Outbound Streams[%d]", 
            OGS_ADDR(addr, buf), enb->max_num_of_ostreams);

        break;

    case MME_EVT_S1AP_LO_CONNREFUSED:
        sock = e->sctp_sock;
        ogs_assert(sock);
        addr = e->sctp_addr;
        ogs_assert(addr);

        enb = mme_enb_find_by_addr(addr);
        ogs_free(addr);

        if (enb) {
            ogs_info("eNB-S1[%s] connection refused!!!", 
                    OGS_ADDR(addr, buf));
            mme_enb_remove(enb);
        } else {
            ogs_warn("eNB-S1[%s] connection refused, Already Removed!",
                    OGS_ADDR(addr, buf));
        }

        break;
    case MME_EVT_S1AP_MESSAGE:
        sock = e->sctp_sock;
        ogs_assert(sock);
        addr = e->sctp_addr;
        ogs_assert(addr);
        pkbuf = e->pkbuf;
        ogs_assert(pkbuf);

        enb = mme_enb_find_by_addr(addr);
        ogs_free(addr);

        ogs_assert(enb);
        ogs_assert(OGS_FSM_STATE(&enb->sm));

        rc = s1ap_decode_pdu(&s1ap_message, pkbuf);
        if (rc == OGS_OK) {
            e->enb = enb;
            e->s1ap_message = &s1ap_message;
            ogs_fsm_dispatch(&enb->sm, e);
        } else {
            ogs_warn("Cannot process S1AP message");
            rv = s1ap_send_error_indication(
                    enb, NULL, NULL, S1AP_Cause_PR_protocol, 
                    S1AP_CauseProtocol_abstract_syntax_error_falsely_constructed_message);
            ogs_assert(rv == OGS_OK);
        }

        s1ap_free_pdu(&s1ap_message);
        ogs_pkbuf_free(pkbuf);
        break;

    case MME_EVT_S1AP_TIMER:
        enb_ue = e->enb_ue;
        ogs_assert(enb_ue);
        enb = e->enb;
        ogs_assert(enb);
        ogs_assert(OGS_FSM_STATE(&enb->sm));

        ogs_fsm_dispatch(&enb->sm, e);
        break;

    case MME_EVT_EMM_MESSAGE:
        enb_ue = e->enb_ue;
        ogs_assert(enb_ue);
        pkbuf = e->pkbuf;
        ogs_assert(pkbuf);
        ogs_assert(nas_emm_decode(&nas_message, pkbuf) == OGS_OK);

        mme_ue = enb_ue->mme_ue;
        if (!mme_ue) {
            mme_ue = mme_ue_find_by_message(&nas_message);
            if (!mme_ue) {
                mme_ue = mme_ue_add(enb_ue);
                ogs_assert(mme_ue);
            } else {
                /* Here, if the MME_UE Context is found,
                 * the integrity check is not performed
                 * For example, ATTACH_REQUEST, 
                 * TRACKING_AREA_UPDATE_REQUEST message
                 *
                 * Now, We will check the MAC in the NAS message*/
                nas_security_header_type_t h;
                h.type = e->nas_type;
                if (h.integrity_protected) {
                    /* Decryption was performed in S1AP handler.
                     * So, we disabled 'ciphered' 
                     * not to decrypt NAS message */
                    h.ciphered = 0;
                    ogs_assert(nas_security_decode(mme_ue, h, pkbuf) == OGS_OK);
                }
            }

            /* If NAS(mme_ue_t) has already been associated with
             * older S1(enb_ue_t) context */
            if (ECM_CONNECTED(mme_ue)) {
               /* Implcit S1 release */
                ogs_debug("Implicit S1 release");
                ogs_debug("    ENB_UE_S1AP_ID[%d] MME_UE_S1AP_ID[%d]",
                      mme_ue->enb_ue->enb_ue_s1ap_id,
                      mme_ue->enb_ue->mme_ue_s1ap_id);
                enb_ue_remove(mme_ue->enb_ue);
            }
            mme_ue_associate_enb_ue(mme_ue, enb_ue);
        }

        ogs_assert(mme_ue);
        ogs_assert(OGS_FSM_STATE(&mme_ue->sm));

        e->mme_ue = mme_ue;
        e->nas_message = &nas_message;

        ogs_fsm_dispatch(&mme_ue->sm, e);
        if (OGS_FSM_CHECK(&mme_ue->sm, emm_state_exception)) {
            mme_send_delete_session_or_ue_context_release(mme_ue, enb_ue);
        }

        ogs_pkbuf_free(pkbuf);
        break;
    case MME_EVT_EMM_TIMER:
        mme_ue = e->mme_ue;
        ogs_assert(mme_ue);
        ogs_assert(OGS_FSM_STATE(&mme_ue->sm));

        ogs_fsm_dispatch(&mme_ue->sm, e);
        break;

    case MME_EVT_ESM_MESSAGE:
        mme_ue = e->mme_ue;
        ogs_assert(mme_ue);

        pkbuf = e->pkbuf;
        ogs_assert(pkbuf);
        ogs_assert(nas_esm_decode(&nas_message, pkbuf) == OGS_OK);

        bearer = mme_bearer_find_or_add_by_message(mme_ue, &nas_message);
        if (!bearer) {
            ogs_error("mme_bearer_find_or_add_by_message() failed");
            ogs_pkbuf_free(pkbuf);
            break;
        }

        sess = bearer->sess;
        ogs_assert(sess);
        default_bearer = mme_default_bearer_in_sess(sess);
        ogs_assert(default_bearer);

        e->bearer = bearer;
        e->nas_message = &nas_message;

        ogs_fsm_dispatch(&bearer->sm, e);
        if (OGS_FSM_CHECK(&bearer->sm, esm_state_bearer_deactivated)) {
            if (default_bearer->ebi == bearer->ebi) {
                /* if the bearer is a default bearer,
                 * remove all session context linked the default bearer */
                mme_sess_remove(sess);
            } else {
                /* if the bearer is not a default bearer,
                 * just remove the bearer context */
                mme_bearer_remove(bearer);
            }

        } else if (OGS_FSM_CHECK(&bearer->sm, esm_state_pdn_did_disconnect)) {
            ogs_assert(default_bearer->ebi == bearer->ebi);
            mme_sess_remove(sess);

        } else if (OGS_FSM_CHECK(&bearer->sm, esm_state_exception)) {
            /* 
             * [Enhancement] - Probably Invalid APN 
             * At this point, we'll forcely release UE context
             */
            mme_send_delete_session_or_ue_context_release(
                    mme_ue, mme_ue->enb_ue);
        }

        ogs_pkbuf_free(pkbuf);
        break;

    case MME_EVT_ESM_TIMER:
        bearer = e->bearer;
        ogs_assert(bearer);
        ogs_assert(OGS_FSM_STATE(&bearer->sm));

        ogs_fsm_dispatch(&bearer->sm, e);
        break;

    case MME_EVT_S6A_MESSAGE:
        mme_ue = e->mme_ue;
        ogs_assert(mme_ue);
        s6abuf = e->pkbuf;
        ogs_assert(s6abuf);
        s6a_message = s6abuf->data;
        ogs_assert(s6a_message);

        if (s6a_message->result_code != ER_DIAMETER_SUCCESS) {
            enb_ue_t *enb_ue = NULL;

            rv = nas_send_attach_reject(mme_ue,
                EMM_CAUSE_IMSI_UNKNOWN_IN_HSS,
                ESM_CAUSE_PROTOCOL_ERROR_UNSPECIFIED);
            ogs_assert(rv == OGS_OK);
            ogs_warn("EMM_CAUSE : IMSI Unknown in HSS");

            enb_ue = mme_ue->enb_ue;
            ogs_assert(enb_ue);

            CLEAR_ENB_UE_TIMER(enb_ue->t_ue_context_release);
            rv = s1ap_send_ue_context_release_command(enb_ue,
                    S1AP_Cause_PR_nas, S1AP_CauseNas_normal_release,
                    S1AP_UE_CTX_REL_UE_CONTEXT_REMOVE, 0);
            ogs_assert(rv == OGS_OK);

            ogs_pkbuf_free(s6abuf);
            break;
        }

        switch (s6a_message->cmd_code) {
        case S6A_CMD_CODE_AUTHENTICATION_INFORMATION:
            mme_s6a_handle_aia(mme_ue, &s6a_message->aia_message);
            break;
        case S6A_CMD_CODE_UPDATE_LOCATION:
            mme_s6a_handle_ula(mme_ue, &s6a_message->ula_message);

            if (OGS_FSM_CHECK(&mme_ue->sm, emm_state_initial_context_setup)) {
                if (mme_ue->nas_eps.type == MME_EPS_TYPE_ATTACH_REQUEST) {
                    rv = nas_send_emm_to_esm(mme_ue,
                            &mme_ue->pdn_connectivity_request);
                    ogs_assert(rv == OGS_OK);
                } else {
                    ogs_fatal("Invalid Type[%d]", mme_ue->nas_eps.type);
                    ogs_assert_if_reached();
                }
            }
            else if (OGS_FSM_CHECK(&mme_ue->sm, emm_state_registered)) {
                if (mme_ue->nas_eps.type == MME_EPS_TYPE_TAU_REQUEST) {
                    rv = nas_send_tau_accept(mme_ue,
                            S1AP_ProcedureCode_id_InitialContextSetup);
                    ogs_assert(rv == OGS_OK);
                } else if (mme_ue->nas_eps.type ==
                    MME_EPS_TYPE_SERVICE_REQUEST) {
                    rv = s1ap_send_initial_context_setup_request(
                            mme_ue);
                    ogs_assert(rv == OGS_OK);
                } else {
                    ogs_fatal("Invalid Type[%d]", mme_ue->nas_eps.type);
                    ogs_assert_if_reached();
                }
            } else
                ogs_assert_if_reached();
            break;
        default:
            ogs_error("Invalid Type[%d]", s6a_message->cmd_code);
            break;
        }
        ogs_pkbuf_free(s6abuf);
        break;

    case MME_EVT_S11_MESSAGE:
        pkbuf = e->pkbuf;
        ogs_assert(pkbuf);
        rv = gtp_parse_msg(&gtp_message, pkbuf);
        ogs_assert(rv == OGS_OK);

        mme_ue = mme_ue_find_by_teid(gtp_message.h.teid);
        if (!mme_ue) {
            /* message received for TEID we know nothing about. Mabe the MME
             * has been restarted without the S-GW knowing yet */
            switch (gtp_message.h.type) {
            case GTP_CREATE_BEARER_REQUEST_TYPE:
            case GTP_UPDATE_BEARER_REQUEST_TYPE:
            case GTP_DELETE_BEARER_REQUEST_TYPE:
            case GTP_DOWNLINK_DATA_NOTIFICATION_TYPE:
                /* FIXME: respond with cause = "Context not found" */
            default:
                ogs_warn("Dropping S11 GTP-C [%u] for unknown TEID [%08x]",
                         gtp_message.h.type, gtp_message.h.teid);
                ogs_pkbuf_free(pkbuf);
                return;
            }
        }

        rv = gtp_xact_receive(mme_ue->gnode, &gtp_message.h, &xact);
        if (rv != OGS_OK) {
            ogs_pkbuf_free(pkbuf);
            break;
        }

        switch (gtp_message.h.type) {
        case GTP_CREATE_SESSION_RESPONSE_TYPE:
            mme_s11_handle_create_session_response(
                xact, mme_ue, &gtp_message.create_session_response);
            break;
        case GTP_MODIFY_BEARER_RESPONSE_TYPE:
            mme_s11_handle_modify_bearer_response(
                xact, mme_ue, &gtp_message.modify_bearer_response);
            break;
        case GTP_DELETE_SESSION_RESPONSE_TYPE:
            mme_s11_handle_delete_session_response(
                xact, mme_ue, &gtp_message.delete_session_response);
            break;
        case GTP_CREATE_BEARER_REQUEST_TYPE:
            mme_s11_handle_create_bearer_request(
                xact, mme_ue, &gtp_message.create_bearer_request);
            break;
        case GTP_UPDATE_BEARER_REQUEST_TYPE:
            mme_s11_handle_update_bearer_request(
                xact, mme_ue, &gtp_message.update_bearer_request);
            break;
        case GTP_DELETE_BEARER_REQUEST_TYPE:
            mme_s11_handle_delete_bearer_request(
                xact, mme_ue, &gtp_message.delete_bearer_request);
            break;
        case GTP_RELEASE_ACCESS_BEARERS_RESPONSE_TYPE:
            mme_s11_handle_release_access_bearers_response(
                xact, mme_ue, &gtp_message.release_access_bearers_response);
            break;
        case GTP_DOWNLINK_DATA_NOTIFICATION_TYPE:
            mme_s11_handle_downlink_data_notification(
                xact, mme_ue, &gtp_message.downlink_data_notification);

/*
* 5.3.4.2 in Spec 23.401
* Under certain conditions, the current UE triggered Service Request 
* procedure can cause unnecessary Downlink Packet Notification messages 
* which increase the load of the MME.
*
* This can occur when uplink data sent in step 6 causes a response 
* on the downlink which arrives at the Serving GW before the Modify Bearer 
* Request message, step 8. This data cannot be forwarded from the Serving GW 
* to the eNodeB and hence it triggers a Downlink Data Notification message.
*
* If the MME receives a Downlink Data Notification after step 2 and 
* before step 9, the MME shall not send S1 interface paging messages
*/
            if (ECM_IDLE(mme_ue))
                s1ap_send_paging(mme_ue, S1AP_CNDomain_ps);
            break;
        case GTP_CREATE_INDIRECT_DATA_FORWARDING_TUNNEL_RESPONSE_TYPE:
            mme_s11_handle_create_indirect_data_forwarding_tunnel_response(
                xact, mme_ue,
                &gtp_message.create_indirect_data_forwarding_tunnel_response);
            break;
        case GTP_DELETE_INDIRECT_DATA_FORWARDING_TUNNEL_RESPONSE_TYPE:
            mme_s11_handle_delete_indirect_data_forwarding_tunnel_response(
                xact, mme_ue,
                &gtp_message.delete_indirect_data_forwarding_tunnel_response);
            break;
        default:
            ogs_warn("Not implmeneted(type:%d)", gtp_message.h.type);
            break;
        }
        ogs_pkbuf_free(pkbuf);
        break;

    case MME_EVT_SGSAP_LO_SCTP_COMM_UP:
        sock = e->sctp_sock;
        ogs_assert(sock);
        addr = e->sctp_addr;
        ogs_assert(addr);

        max_num_of_ostreams = e->max_num_of_ostreams;

        vlr = mme_vlr_find_by_addr(addr);
        ogs_free(addr);

        ogs_assert(vlr);
        ogs_assert(OGS_FSM_STATE(&vlr->sm));

        vlr->max_num_of_ostreams =
                ogs_min(max_num_of_ostreams, vlr->max_num_of_ostreams);

        ogs_debug("VLR-SGs SCTP_COMM_UP[%s] Max Num of Outbound Streams[%d]", 
            OGS_ADDR(addr, buf), vlr->max_num_of_ostreams);

        e->vlr = vlr;
        ogs_fsm_dispatch(&vlr->sm, e);
        break;

    case MME_EVT_SGSAP_LO_CONNREFUSED:
        sock = e->sctp_sock;
        ogs_assert(sock);
        addr = e->sctp_addr;
        ogs_assert(addr);

        vlr = mme_vlr_find_by_addr(addr);
        ogs_free(addr);

        ogs_assert(vlr);
        ogs_assert(OGS_FSM_STATE(&vlr->sm));

        if (OGS_FSM_CHECK(&vlr->sm, sgsap_state_connected)) {
            e->vlr = vlr;
            ogs_fsm_dispatch(&vlr->sm, e);

            ogs_info("VLR-SGs[%s] connection refused!!!", 
                    OGS_ADDR(addr, buf));

        } else {
            ogs_warn("VLR-SGs[%s] connection refused, Already Removed!",
                    OGS_ADDR(addr, buf));
        }

        break;
    case MME_EVT_SGSAP_MESSAGE:
        sock = e->sctp_sock;
        ogs_assert(sock);
        addr = e->sctp_addr;
        ogs_assert(addr);
        pkbuf = e->pkbuf;
        ogs_assert(pkbuf);

        vlr = mme_vlr_find_by_addr(addr);
        ogs_assert(vlr);
        ogs_free(addr);

        ogs_assert(vlr);
        ogs_assert(OGS_FSM_STATE(&vlr->sm));

        e->vlr = vlr;
        ogs_fsm_dispatch(&vlr->sm, e);

        ogs_pkbuf_free(pkbuf);
        break;

    case MME_EVT_SGSAP_TIMER:
        vlr = e->vlr;
        ogs_assert(vlr);
        ogs_assert(OGS_FSM_STATE(&vlr->sm));

        ogs_fsm_dispatch(&vlr->sm, e);
        break;

    default:
        ogs_error("No handler for event %s", mme_event_get_name(e));
        break;
    }
}
