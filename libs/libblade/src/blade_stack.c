/*
 * Copyright (c) 2007-2014, Anthony Minessale II
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of the original author; nor the names of any contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "blade.h"

struct blade_handle_s {
	ks_pool_t *pool;
	ks_thread_pool_t *tpool;

	blade_transportmgr_t *transportmgr;
	blade_rpcmgr_t *rpcmgr;
	blade_routemgr_t *routemgr;
	blade_subscriptionmgr_t *subscriptionmgr;
	blade_upstreammgr_t *upstreammgr;
	blade_mastermgr_t *mastermgr;
	blade_connectionmgr_t *connectionmgr;
	blade_sessionmgr_t *sessionmgr;
};


ks_bool_t blade_protocol_register_request_handler(blade_rpc_request_t *brpcreq, void *data);
ks_bool_t blade_protocol_publish_request_handler(blade_rpc_request_t *brpcreq, void *data);
ks_bool_t blade_protocol_locate_request_handler(blade_rpc_request_t *brpcreq, void *data);
ks_bool_t blade_protocol_execute_request_handler(blade_rpc_request_t *brpcreq, void *data);
ks_bool_t blade_protocol_subscribe_request_handler(blade_rpc_request_t *brpcreq, void *data);
ks_bool_t blade_protocol_broadcast_request_handler(blade_rpc_request_t *brpcreq, void *data);


static void blade_handle_cleanup(ks_pool_t *pool, void *ptr, void *arg, ks_pool_cleanup_action_t action, ks_pool_cleanup_type_t type)
{
	blade_handle_t *bh = (blade_handle_t *)ptr;

	ks_assert(bh);

	switch (action) {
	case KS_MPCL_ANNOUNCE:
		break;
	case KS_MPCL_TEARDOWN:
		blade_transportmgr_destroy(&bh->transportmgr);
		blade_rpcmgr_destroy(&bh->rpcmgr);
		blade_routemgr_destroy(&bh->routemgr);
		blade_subscriptionmgr_destroy(&bh->subscriptionmgr);
		blade_upstreammgr_destroy(&bh->upstreammgr);
		blade_mastermgr_destroy(&bh->mastermgr);
		blade_connectionmgr_destroy(&bh->connectionmgr);
		blade_sessionmgr_destroy(&bh->sessionmgr);

		ks_thread_pool_destroy(&bh->tpool);
		break;
	case KS_MPCL_DESTROY:
		break;
	}
}

KS_DECLARE(ks_status_t) blade_handle_create(blade_handle_t **bhP)
{
	blade_handle_t *bh = NULL;
	ks_pool_t *pool = NULL;
	ks_thread_pool_t *tpool = NULL;

	ks_assert(bhP);

	ks_pool_open(&pool);
	ks_assert(pool);

	ks_thread_pool_create(&tpool, BLADE_HANDLE_TPOOL_MIN, BLADE_HANDLE_TPOOL_MAX, BLADE_HANDLE_TPOOL_STACK, KS_PRI_NORMAL, BLADE_HANDLE_TPOOL_IDLE);
	ks_assert(tpool);

	bh = ks_pool_alloc(pool, sizeof(blade_handle_t));
	bh->pool = pool;
	bh->tpool = tpool;

	blade_transportmgr_create(&bh->transportmgr, bh);
	ks_assert(bh->transportmgr);

	blade_rpcmgr_create(&bh->rpcmgr, bh);
	ks_assert(bh->rpcmgr);

	blade_routemgr_create(&bh->routemgr, bh);
	ks_assert(bh->routemgr);

	blade_subscriptionmgr_create(&bh->subscriptionmgr, bh);
	ks_assert(bh->subscriptionmgr);

	blade_upstreammgr_create(&bh->upstreammgr, bh);
	ks_assert(bh->upstreammgr);

	blade_mastermgr_create(&bh->mastermgr, bh);
	ks_assert(bh->mastermgr);

	blade_connectionmgr_create(&bh->connectionmgr, bh);
	ks_assert(bh->connectionmgr);

	blade_sessionmgr_create(&bh->sessionmgr, bh);
	ks_assert(bh->sessionmgr);


	ks_pool_set_cleanup(pool, bh, NULL, blade_handle_cleanup);

	*bhP = bh;

	ks_log(KS_LOG_DEBUG, "Created\n");

	return KS_STATUS_SUCCESS;
}

KS_DECLARE(ks_status_t) blade_handle_destroy(blade_handle_t **bhP)
{
	blade_handle_t *bh = NULL;
	ks_pool_t *pool;

	ks_assert(bhP);

	bh = *bhP;
	*bhP = NULL;

	ks_assert(bh);

	pool = bh->pool;

	// shutdown cannot happen inside of the cleanup callback because it'll lock a mutex for the pool during cleanup callbacks which connections and sessions need to finish their cleanup
	// and more importantly, memory needs to remain intact until shutdown is completed to avoid various things hitting teardown before shutdown runs
	blade_handle_shutdown(bh);

	ks_pool_close(&pool);

	return KS_STATUS_SUCCESS;
}

ks_status_t blade_handle_config(blade_handle_t *bh, config_setting_t *config)
{
	config_setting_t *master = NULL;
	config_setting_t *master_nodeid = NULL;
	config_setting_t *master_realms = NULL;
	const char *nodeid = NULL;
	int32_t realms_length = 0;

	ks_assert(bh);

	if (!config) return KS_STATUS_FAIL;
	if (!config_setting_is_group(config)) {
		ks_log(KS_LOG_DEBUG, "!config_setting_is_group(config)\n");
		return KS_STATUS_FAIL;
	}

	master = config_setting_get_member(config, "master");
	if (master) {
		master_nodeid = config_lookup_from(master, "nodeid");
		if (master_nodeid) {
			if (config_setting_type(master_nodeid) != CONFIG_TYPE_STRING) return KS_STATUS_FAIL;
			nodeid = config_setting_get_string(master_nodeid);

			blade_upstreammgr_localid_set(bh->upstreammgr, nodeid);
			blade_upstreammgr_masterid_set(bh->upstreammgr, nodeid);
		}
		master_realms = config_lookup_from(master, "realms");
		if (master_realms) {
			if (config_setting_type(master_realms) != CONFIG_TYPE_LIST) return KS_STATUS_FAIL;
			realms_length = config_setting_length(master_realms);
			if (realms_length > 0) {
				for (int32_t index = 0; index < realms_length; ++index) {
					const char *realm = config_setting_get_string_elem(master_realms, index);
					if (!realm) return KS_STATUS_FAIL;
					blade_upstreammgr_realm_add(bh->upstreammgr, realm);
				}
			}
		}
	}

	return KS_STATUS_SUCCESS;
}

KS_DECLARE(ks_status_t) blade_handle_startup(blade_handle_t *bh, config_setting_t *config)
{
	blade_rpc_t *brpc = NULL;
	blade_transport_t *bt = NULL;

	ks_assert(bh);

    if (blade_handle_config(bh, config) != KS_STATUS_SUCCESS) {
		ks_log(KS_LOG_DEBUG, "blade_handle_config failed\n");
		return KS_STATUS_FAIL;
	}

	// register internal transport for secure websockets
	blade_transport_wss_create(&bt, bh);
	ks_assert(bt);
	blade_transportmgr_default_set(bh->transportmgr, bt);
	blade_transportmgr_transport_add(bh->transportmgr, bt);


	// register internal core rpcs for blade.xxx
	blade_rpc_create(&brpc, bh, "blade.register", NULL, NULL, blade_protocol_register_request_handler, NULL);
	blade_rpcmgr_corerpc_add(bh->rpcmgr, brpc);

	blade_rpc_create(&brpc, bh, "blade.publish", NULL, NULL, blade_protocol_publish_request_handler, NULL);
	blade_rpcmgr_corerpc_add(bh->rpcmgr, brpc);

	blade_rpc_create(&brpc, bh, "blade.locate", NULL, NULL, blade_protocol_locate_request_handler, NULL);
	blade_rpcmgr_corerpc_add(bh->rpcmgr, brpc);

	blade_rpc_create(&brpc, bh, "blade.execute", NULL, NULL, blade_protocol_execute_request_handler, NULL);
	blade_rpcmgr_corerpc_add(bh->rpcmgr, brpc);

	blade_rpc_create(&brpc, bh, "blade.subscribe", NULL, NULL, blade_protocol_subscribe_request_handler, NULL);
	blade_rpcmgr_corerpc_add(bh->rpcmgr, brpc);

	blade_rpc_create(&brpc, bh, "blade.broadcast", NULL, NULL, blade_protocol_broadcast_request_handler, NULL);
	blade_rpcmgr_corerpc_add(bh->rpcmgr, brpc);


	blade_transportmgr_startup(bh->transportmgr, config);

	return KS_STATUS_SUCCESS;
}

KS_DECLARE(ks_status_t) blade_handle_shutdown(blade_handle_t *bh)
{
	ks_assert(bh);

	blade_transportmgr_shutdown(bh->transportmgr);

	blade_connectionmgr_shutdown(bh->connectionmgr);

	blade_sessionmgr_shutdown(bh->sessionmgr);

	return KS_STATUS_SUCCESS;
}

KS_DECLARE(ks_pool_t *) blade_handle_pool_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->pool;
}

KS_DECLARE(ks_thread_pool_t *) blade_handle_tpool_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->tpool;
}

KS_DECLARE(blade_transportmgr_t *) blade_handle_transportmgr_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->transportmgr;
}

KS_DECLARE(blade_rpcmgr_t *) blade_handle_rpcmgr_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->rpcmgr;
}

KS_DECLARE(blade_routemgr_t *) blade_handle_routemgr_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->routemgr;
}

KS_DECLARE(blade_subscriptionmgr_t *) blade_handle_subscriptionmgr_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->subscriptionmgr;
}

KS_DECLARE(blade_upstreammgr_t *) blade_handle_upstreammgr_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->upstreammgr;
}

KS_DECLARE(blade_mastermgr_t *) blade_handle_mastermgr_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->mastermgr;
}

KS_DECLARE(blade_connectionmgr_t *) blade_handle_connectionmgr_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->connectionmgr;
}

KS_DECLARE(blade_sessionmgr_t *) blade_handle_sessionmgr_get(blade_handle_t *bh)
{
	ks_assert(bh);
	return bh->sessionmgr;
}


KS_DECLARE(ks_status_t) blade_handle_connect(blade_handle_t *bh, blade_connection_t **bcP, blade_identity_t *target, const char *session_id)
{
	ks_status_t ret = KS_STATUS_SUCCESS;
	blade_transport_t *bt = NULL;
	blade_transport_callbacks_t *callbacks = NULL;

	ks_assert(bh);
	ks_assert(target);

	// @todo mini state machine to deal with upstream establishment to avoid attempting multiple upstream connects at the same time
	if (blade_upstreammgr_session_established(bh->upstreammgr)) return KS_STATUS_DUPLICATE_OPERATION;

	bt = blade_transportmgr_transport_lookup(bh->transportmgr, blade_identity_parameter_get(target, "transport"), KS_TRUE);
	ks_assert(bt);

	callbacks = blade_transport_callbacks_get(bt);
	ks_assert(callbacks);

	if (callbacks->onconnect) ret = callbacks->onconnect(bcP, bt, target, session_id);

	return ret;
}


// BLADE PROTOCOL HANDLERS

// @todo revisit all error sending. JSONRPC "error" should only be used for json parsing errors, change the rest to internal errors for each of the corerpcs
// @todo all higher level errors should be handled by each of the calls internally so that a normal result response can be sent with an error block inside the result
// which is important for implementation of blade.execute where errors can be relayed back to the requester properly

// blade.register request generator
KS_DECLARE(ks_status_t) blade_protocol_register(blade_handle_t *bh, const char *nodeid, ks_bool_t remove, blade_rpc_response_callback_t callback, void *data)
{
	ks_status_t ret = KS_STATUS_SUCCESS;
	blade_session_t *bs = NULL;
	ks_pool_t *pool = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;

	ks_assert(bh);
	ks_assert(nodeid);

	if (!(bs = blade_upstreammgr_session_get(bh->upstreammgr))) {
		ret = KS_STATUS_DISCONNECTED;
		goto done;
	}

	pool = blade_handle_pool_get(bh);
	ks_assert(pool);

	blade_rpc_request_raw_create(pool, &req, &req_params, NULL, "blade.register");

	// fill in the req_params
	cJSON_AddStringToObject(req_params, "nodeid", nodeid);
	if (remove) cJSON_AddTrueToObject(req_params, "remove");

	ks_log(KS_LOG_DEBUG, "Session (%s) register request (%s %s) started\n", blade_session_id_get(bs), remove ? "removing" : "adding", nodeid);

	ret = blade_session_send(bs, req, callback, data);

done:
	if (req) cJSON_Delete(req);
	if (bs) blade_session_read_unlock(bs);

	return ret;
}

// blade.register request handler
ks_bool_t blade_protocol_register_request_handler(blade_rpc_request_t *brpcreq, void *data)
{
	blade_handle_t *bh = NULL;
	blade_session_t *bs = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_params_nodeid = NULL;
	cJSON *req_params_remove = NULL;
	ks_bool_t remove = KS_FALSE;
	cJSON *res = NULL;
	cJSON *res_result = NULL;

	ks_assert(brpcreq);

	bh = blade_rpc_request_handle_get(brpcreq);
	ks_assert(bh);

	bs = blade_sessionmgr_session_lookup(bh->sessionmgr, blade_rpc_request_sessionid_get(brpcreq));
	ks_assert(bs);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (!req_params) {
		ks_log(KS_LOG_DEBUG, "Session (%s) register request missing 'params' object\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params object");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_nodeid = cJSON_GetObjectCstr(req_params, "nodeid");
	if (!req_params_nodeid) {
		ks_log(KS_LOG_DEBUG, "Session (%s) register request missing 'nodeid'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}
	req_params_remove = cJSON_GetObjectItem(req_params, "remove");
	remove = req_params_remove && req_params_remove->type == cJSON_True;

	ks_log(KS_LOG_DEBUG, "Session (%s) register request (%s %s) processing\n", blade_session_id_get(bs), remove ? "removing" : "adding", req_params_nodeid);

	if (remove) {
		blade_session_route_remove(bs, req_params_nodeid);
		blade_routemgr_route_remove(blade_handle_routemgr_get(bh), req_params_nodeid);
	} else {
		blade_session_route_add(bs, req_params_nodeid);
		blade_routemgr_route_add(blade_handle_routemgr_get(bh), req_params_nodeid, blade_session_id_get(bs));
	}

	blade_rpc_response_raw_create(&res, &res_result, blade_rpc_request_messageid_get(brpcreq));
	blade_session_send(bs, res, NULL, NULL);

done:

	if (res) cJSON_Delete(res);
	if (bs) blade_session_read_unlock(bs);

	return KS_FALSE;
}


// blade.publish request generator
KS_DECLARE(ks_status_t) blade_protocol_publish(blade_handle_t *bh, const char *name, const char *realm, blade_rpc_response_callback_t callback, void *data)
{
	ks_status_t ret = KS_STATUS_SUCCESS;
	blade_session_t *bs = NULL;
	ks_pool_t *pool = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *id = NULL;

	ks_assert(bh);
	ks_assert(name);
	ks_assert(realm);

	// @todo consideration for the Master trying to publish a protocol, with no upstream
	if (!(bs = blade_upstreammgr_session_get(bh->upstreammgr))) {
		ret = KS_STATUS_DISCONNECTED;
		goto done;
	}

	pool = blade_handle_pool_get(bh);
	ks_assert(pool);

	blade_rpc_request_raw_create(pool, &req, &req_params, NULL, "blade.publish");

	// fill in the req_params
	cJSON_AddStringToObject(req_params, "protocol", name);
	cJSON_AddStringToObject(req_params, "realm", realm);

	blade_upstreammgr_localid_copy(bh->upstreammgr, pool, &id);
	ks_assert(id);

	cJSON_AddStringToObject(req_params, "requester-nodeid", id);
	ks_pool_free(pool, &id);

	blade_upstreammgr_masterid_copy(bh->upstreammgr, pool, &id);
	ks_assert(id);

	cJSON_AddStringToObject(req_params, "responder-nodeid", id);
	ks_pool_free(pool, &id);

	// @todo add a parameter containing a block of json for schema definitions for each of the methods being published

	ks_log(KS_LOG_DEBUG, "Session (%s) publish request started\n", blade_session_id_get(bs));

	ret = blade_session_send(bs, req, callback, data);

done:
	if (req) cJSON_Delete(req);
	if (bs) blade_session_read_unlock(bs);

	return ret;
}

// blade.publish request handler
ks_bool_t blade_protocol_publish_request_handler(blade_rpc_request_t *brpcreq, void *data)
{
	blade_handle_t *bh = NULL;
	blade_session_t *bs = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_params_protocol = NULL;
	const char *req_params_realm = NULL;
	const char *req_params_requester_nodeid = NULL;
	const char *req_params_responder_nodeid = NULL;
	cJSON *res = NULL;
	cJSON *res_result = NULL;

	ks_assert(brpcreq);

	bh = blade_rpc_request_handle_get(brpcreq);
	ks_assert(bh);

	bs = blade_sessionmgr_session_lookup(blade_handle_sessionmgr_get(bh), blade_rpc_request_sessionid_get(brpcreq));
	ks_assert(bs);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (!req_params) {
		ks_log(KS_LOG_DEBUG, "Session (%s) publish request missing 'params' object\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params object");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_protocol = cJSON_GetObjectCstr(req_params, "protocol");
	if (!req_params_protocol) {
		ks_log(KS_LOG_DEBUG, "Session (%s) publish request missing 'protocol'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params protocol");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_realm = cJSON_GetObjectCstr(req_params, "realm");
	if (!req_params_realm) {
		ks_log(KS_LOG_DEBUG, "Session (%s) publish request missing 'realm'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params realm");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	// @todo confirm the realm is permitted for the session, this gets complicated with subdomains, skipping for now

	req_params_requester_nodeid = cJSON_GetObjectCstr(req_params, "requester-nodeid");
	if (!req_params_requester_nodeid) {
		ks_log(KS_LOG_DEBUG, "Session (%s) publish request missing 'requester-nodeid'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params requester-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_responder_nodeid = cJSON_GetObjectCstr(req_params, "responder-nodeid");
	if (!req_params_responder_nodeid) {
		ks_log(KS_LOG_DEBUG, "Session (%s) publish request missing 'responder-nodeid'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params responder-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	if (!blade_upstreammgr_masterid_compare(bh->upstreammgr, req_params_responder_nodeid)) {
		ks_log(KS_LOG_DEBUG, "Session (%s) publish request invalid 'responder-nodeid' (%s)\n", blade_session_id_get(bs), req_params_responder_nodeid);
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Invalid params responder-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	ks_log(KS_LOG_DEBUG, "Session (%s) publish request (%s to %s) processing\n", blade_session_id_get(bs), req_params_requester_nodeid, req_params_responder_nodeid);

	blade_mastermgr_controller_add(bh->mastermgr, req_params_protocol, req_params_realm, req_params_requester_nodeid);

	// build the actual response finally
	blade_rpc_response_raw_create(&res, &res_result, blade_rpc_request_messageid_get(brpcreq));

	cJSON_AddStringToObject(res_result, "protocol", req_params_protocol);
	cJSON_AddStringToObject(res_result, "realm", req_params_realm);
	cJSON_AddStringToObject(res_result, "requester-nodeid", req_params_requester_nodeid);
	cJSON_AddStringToObject(res_result, "responder-nodeid", req_params_responder_nodeid);

	// request was just received on a session that is already read locked, so we can assume the response goes back on the same session without further lookup
	blade_session_send(bs, res, NULL, NULL);

done:

	if (res) cJSON_Delete(res);
	if (bs) blade_session_read_unlock(bs);

	return KS_FALSE;
}


// blade.locate request generator
// @todo discuss system to support caching locate results, and internally subscribing to receive event updates related to protocols which have been located
// to ensure local caches remain synced when protocol controllers change, but this requires additional filters for event propagating to avoid broadcasting
// every protocol update to everyone which may actually be a better way than an explicit locate request
KS_DECLARE(ks_status_t) blade_protocol_locate(blade_handle_t *bh, const char *name, const char *realm, blade_rpc_response_callback_t callback, void *data)
{
	ks_status_t ret = KS_STATUS_SUCCESS;
	blade_session_t *bs = NULL;
	ks_pool_t *pool = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *id = NULL;

	ks_assert(bh);
	ks_assert(name);
	ks_assert(realm);

	if (!(bs = blade_upstreammgr_session_get(bh->upstreammgr))) {
		ret = KS_STATUS_DISCONNECTED;
		goto done;
	}

	pool = blade_handle_pool_get(bh);
	ks_assert(pool);

	blade_rpc_request_raw_create(pool, &req, &req_params, NULL, "blade.locate");

	// fill in the req_params
	cJSON_AddStringToObject(req_params, "protocol", name);
	cJSON_AddStringToObject(req_params, "realm", realm);

	blade_upstreammgr_localid_copy(bh->upstreammgr, pool, &id);
	ks_assert(id);

	cJSON_AddStringToObject(req_params, "requester-nodeid", id);
	ks_pool_free(pool, &id);

	blade_upstreammgr_masterid_copy(bh->upstreammgr, pool, &id);
	ks_assert(id);

	cJSON_AddStringToObject(req_params, "responder-nodeid", id);
	ks_pool_free(pool, &id);

	ks_log(KS_LOG_DEBUG, "Session (%s) locate request started\n", blade_session_id_get(bs));

	ret = blade_session_send(bs, req, callback, data);

done:
	if (req) cJSON_Delete(req);
	if (bs) blade_session_read_unlock(bs);

	return ret;
}

// blade.locate request handler
ks_bool_t blade_protocol_locate_request_handler(blade_rpc_request_t *brpcreq, void *data)
{
	blade_handle_t *bh = NULL;
	blade_session_t *bs = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_params_protocol = NULL;
	const char *req_params_realm = NULL;
	const char *req_params_requester_nodeid = NULL;
	const char *req_params_responder_nodeid = NULL;
	cJSON *res = NULL;
	cJSON *res_result = NULL;
	cJSON *res_result_controllers;
	blade_protocol_t *bp = NULL;

	ks_assert(brpcreq);

	bh = blade_rpc_request_handle_get(brpcreq);
	ks_assert(bh);

	bs = blade_sessionmgr_session_lookup(blade_handle_sessionmgr_get(bh), blade_rpc_request_sessionid_get(brpcreq));
	ks_assert(bs);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (!req_params) {
		ks_log(KS_LOG_DEBUG, "Session (%s) locate request missing 'params' object\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params object");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_protocol = cJSON_GetObjectCstr(req_params, "protocol");
	if (!req_params_protocol) {
		ks_log(KS_LOG_DEBUG, "Session (%s) locate request missing 'protocol'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params protocol");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_realm = cJSON_GetObjectCstr(req_params, "realm");
	if (!req_params_realm) {
		ks_log(KS_LOG_DEBUG, "Session (%s) locate request missing 'realm'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params realm");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	// @todo confirm the realm is permitted for the session, this gets complicated with subdomains, skipping for now

	req_params_requester_nodeid = cJSON_GetObjectCstr(req_params, "requester-nodeid");
	if (!req_params_requester_nodeid) {
		ks_log(KS_LOG_DEBUG, "Session (%s) locate request missing 'requester-nodeid'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params requester-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_responder_nodeid = cJSON_GetObjectCstr(req_params, "responder-nodeid");
	if (!req_params_responder_nodeid) {
		ks_log(KS_LOG_DEBUG, "Session (%s) locate request missing 'responder-nodeid'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params responder-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	if (!blade_upstreammgr_masterid_compare(bh->upstreammgr, req_params_responder_nodeid)) {
		ks_log(KS_LOG_DEBUG, "Session (%s) locate request invalid 'responder-nodeid' (%s)\n", blade_session_id_get(bs), req_params_responder_nodeid);
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Invalid params responder-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	ks_log(KS_LOG_DEBUG, "Session (%s) locate request (%s to %s) processing\n", blade_session_id_get(bs), req_params_requester_nodeid, req_params_responder_nodeid);

	res_result_controllers = cJSON_CreateObject();

	bp = blade_mastermgr_protocol_lookup(bh->mastermgr, req_params_protocol, req_params_realm);
	if (bp) {
		ks_hash_t *controllers = blade_protocol_controllers_get(bp);
		for (ks_hash_iterator_t *it = ks_hash_first(controllers, KS_UNLOCKED); it; it = ks_hash_next(&it)) {
			const char *key = NULL;
			void *value = NULL;

			ks_hash_this(it, (const void **)&key, NULL, &value);

			cJSON_AddItemToArray(res_result_controllers, cJSON_CreateString(key));
		}
	}


	// build the actual response finally
	blade_rpc_response_raw_create(&res, &res_result, blade_rpc_request_messageid_get(brpcreq));

	cJSON_AddStringToObject(res_result, "protocol", req_params_protocol);
	cJSON_AddStringToObject(res_result, "realm", req_params_realm);
	cJSON_AddStringToObject(res_result, "requester-nodeid", req_params_requester_nodeid);
	cJSON_AddStringToObject(res_result, "responder-nodeid", req_params_responder_nodeid);
	cJSON_AddItemToObject(res_result, "controllers", res_result_controllers);

	// request was just received on a session that is already read locked, so we can assume the response goes back on the same session without further lookup
	blade_session_send(bs, res, NULL, NULL);

done:

	if (res) cJSON_Delete(res);
	if (bs) blade_session_read_unlock(bs);

	return KS_FALSE;
}


// blade.execute request generator
KS_DECLARE(ks_status_t) blade_protocol_execute(blade_handle_t *bh, const char *nodeid, const char *method, const char *protocol, const char *realm, cJSON *params, blade_rpc_response_callback_t callback, void *data)
{
	ks_status_t ret = KS_STATUS_SUCCESS;
	blade_session_t *bs = NULL;
	ks_pool_t *pool = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *localid = NULL;

	ks_assert(bh);
	ks_assert(nodeid);
	ks_assert(method);
	ks_assert(protocol);
	ks_assert(realm);

	if (!(bs = blade_routemgr_route_lookup(blade_handle_routemgr_get(bh), nodeid))) {
		if (!(bs = blade_upstreammgr_session_get(bh->upstreammgr))) {
			ret = KS_STATUS_DISCONNECTED;
			goto done;
		}
	}

	pool = blade_handle_pool_get(bh);
	ks_assert(pool);

	blade_rpc_request_raw_create(pool, &req, &req_params, NULL, "blade.execute");

	// fill in the req_params
	cJSON_AddStringToObject(req_params, "method", method);
	cJSON_AddStringToObject(req_params, "protocol", protocol);
	cJSON_AddStringToObject(req_params, "realm", realm);

	blade_upstreammgr_localid_copy(bh->upstreammgr, pool, &localid);
	ks_assert(localid);

	cJSON_AddStringToObject(req_params, "requester-nodeid", localid);
	ks_pool_free(pool, &localid);

	cJSON_AddStringToObject(req_params, "responder-nodeid", nodeid);

	if (params) cJSON_AddItemToObject(req_params, "params", cJSON_Duplicate(params, 1));

	ks_log(KS_LOG_DEBUG, "Session (%s) execute request started\n", blade_session_id_get(bs));

	ret = blade_session_send(bs, req, callback, data);

done:
	if (req) cJSON_Delete(req);
	if (bs) blade_session_read_unlock(bs);

	return ret;
}

// blade.execute request handler
ks_bool_t blade_protocol_execute_request_handler(blade_rpc_request_t *brpcreq, void *data)
{
	ks_bool_t ret = KS_FALSE;
	blade_handle_t *bh = NULL;
	blade_session_t *bs = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_params_method = NULL;
	const char *req_params_protocol = NULL;
	const char *req_params_realm = NULL;
	const char *req_params_requester_nodeid = NULL;
	const char *req_params_responder_nodeid = NULL;
	blade_rpc_t *brpc = NULL;
	blade_rpc_request_callback_t callback = NULL;
	cJSON *res = NULL;

	ks_assert(brpcreq);

	bh = blade_rpc_request_handle_get(brpcreq);
	ks_assert(bh);

	bs = blade_sessionmgr_session_lookup(blade_handle_sessionmgr_get(bh), blade_rpc_request_sessionid_get(brpcreq));
	ks_assert(bs);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (!req_params) {
		ks_log(KS_LOG_DEBUG, "Session (%s) execute request missing 'params' object\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params object");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_method = cJSON_GetObjectCstr(req_params, "method");
	if (!req_params_method) {
		ks_log(KS_LOG_DEBUG, "Session (%s) execute request missing 'method'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params method");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_protocol = cJSON_GetObjectCstr(req_params, "protocol");
	if (!req_params_protocol) {
		ks_log(KS_LOG_DEBUG, "Session (%s) execute request missing 'protocol'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params protocol");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_realm = cJSON_GetObjectCstr(req_params, "realm");
	if (!req_params_realm) {
		ks_log(KS_LOG_DEBUG, "Session (%s) execute request missing 'realm'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params realm");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	// @todo confirm the realm is permitted for the session, this gets complicated with subdomains, skipping for now

	req_params_requester_nodeid = cJSON_GetObjectCstr(req_params, "requester-nodeid");
	if (!req_params_requester_nodeid) {
		ks_log(KS_LOG_DEBUG, "Session (%s) execute request missing 'requester-nodeid'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params requester-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_responder_nodeid = cJSON_GetObjectCstr(req_params, "responder-nodeid");
	if (!req_params_responder_nodeid) {
		ks_log(KS_LOG_DEBUG, "Session (%s) execute request missing 'responder-nodeid'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params responder-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	ks_log(KS_LOG_DEBUG, "Session (%s) execute request (%s to %s) processing\n", blade_session_id_get(bs), req_params_requester_nodeid, req_params_responder_nodeid);

	// @todo pull out nested params block if it exists and check against schema later, so blade_rpc_t should be able to carry a schema with it, even though blade.xxx may not associate one

	brpc = blade_rpcmgr_protocolrpc_lookup(blade_handle_rpcmgr_get(bh), req_params_method, req_params_protocol, req_params_realm);
	if (!brpc) {
		ks_log(KS_LOG_DEBUG, "Session (%s) execute request unknown method\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Unknown params method");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	callback = blade_rpc_callback_get(brpc);
	if (callback) ret = callback(brpcreq, blade_rpc_callback_data_get(brpc));

done:

	if (res) cJSON_Delete(res);
	if (bs) blade_session_read_unlock(bs);

	return ret;
}

KS_DECLARE(const char *) blade_protocol_execute_request_requester_nodeid_get(blade_rpc_request_t *brpcreq)
{
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_requester_nodeid = NULL;

	ks_assert(brpcreq);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (req_params) req_requester_nodeid = cJSON_GetObjectCstr(req_params, "requester-nodeid");

	return req_requester_nodeid;
}

KS_DECLARE(const char *) blade_protocol_execute_request_responder_nodeid_get(blade_rpc_request_t *brpcreq)
{
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_responder_nodeid = NULL;

	ks_assert(brpcreq);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (req_params) req_responder_nodeid = cJSON_GetObjectCstr(req_params, "responder-nodeid");

	return req_responder_nodeid;
}

KS_DECLARE(cJSON *) blade_protocol_execute_request_params_get(blade_rpc_request_t *brpcreq)
{
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	cJSON *req_params_params = NULL;

	ks_assert(brpcreq);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (req_params) req_params_params = cJSON_GetObjectItem(req_params, "params");

	return req_params_params;
}

KS_DECLARE(cJSON *) blade_protocol_execute_response_result_get(blade_rpc_response_t *brpcres)
{
	cJSON *res = NULL;
	cJSON *res_result = NULL;
	cJSON *res_result_result = NULL;

	ks_assert(brpcres);

	res = blade_rpc_response_message_get(brpcres);
	ks_assert(res);

	res_result = cJSON_GetObjectItem(res, "result");
	if (res_result) res_result_result = cJSON_GetObjectItem(res_result, "result");

	return res_result_result;
}

// @note added blade_rpc_request_duplicate() to support async responding where the callbacks return immediately and the blade_rpc_request_t will be destroyed,
// in such cases duplicate the request to retain a copy for passing to blade_protocol_execute_response_send when sending the response as it contains everything
// needed to produce a response except the inner result block for blade.execute and call blade_rpc_request_destroy() to clean up the duplicate when finished
KS_DECLARE(void) blade_protocol_execute_response_send(blade_rpc_request_t *brpcreq, cJSON *result)
{
	blade_handle_t *bh = NULL;
	blade_session_t *bs = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	//const char *req_params_method = NULL;
	const char *req_params_protocol = NULL;
	const char *req_params_realm = NULL;
	const char *req_params_requester_nodeid = NULL;
	const char *req_params_responder_nodeid = NULL;
	cJSON *res = NULL;
	cJSON *res_result = NULL;

	ks_assert(brpcreq);

	bh = blade_rpc_request_handle_get(brpcreq);
	ks_assert(bh);

	bs = blade_sessionmgr_session_lookup(blade_handle_sessionmgr_get(bh), blade_rpc_request_sessionid_get(brpcreq));
	ks_assert(bs);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	ks_assert(req_params);

	req_params_protocol = cJSON_GetObjectCstr(req_params, "protocol");
	ks_assert(req_params_protocol);

	req_params_realm = cJSON_GetObjectCstr(req_params, "realm");
	ks_assert(req_params_realm);

	req_params_requester_nodeid = cJSON_GetObjectCstr(req_params, "requester-nodeid");
	ks_assert(req_params_requester_nodeid);

	req_params_responder_nodeid = cJSON_GetObjectCstr(req_params, "responder-nodeid");
	ks_assert(req_params_responder_nodeid);

	// build the actual response finally, wrap this into blade_protocol_execute_response_send()
	blade_rpc_response_raw_create(&res, &res_result, blade_rpc_request_messageid_get(brpcreq));

	cJSON_AddStringToObject(res_result, "protocol", req_params_protocol);
	cJSON_AddStringToObject(res_result, "realm", req_params_realm);
	cJSON_AddStringToObject(res_result, "requester-nodeid", req_params_requester_nodeid);
	cJSON_AddStringToObject(res_result, "responder-nodeid", req_params_responder_nodeid);
	if (result) cJSON_AddItemToObject(res_result, "result", cJSON_Duplicate(result, 1));

	// request was just received on a session that is already read locked, so we can assume the response goes back on the same session without further lookup
	blade_session_send(bs, res, NULL, NULL);

	cJSON_Delete(res);

	blade_session_read_unlock(bs);
}


// blade.subscribe request generator
KS_DECLARE(ks_status_t) blade_protocol_subscribe(blade_handle_t *bh, const char *event, const char *protocol, const char *realm, ks_bool_t remove, blade_rpc_response_callback_t callback, void *data, blade_rpc_request_callback_t event_callback, void *event_data)
{
	ks_status_t ret = KS_STATUS_SUCCESS;
	blade_session_t *bs = NULL;
	const char *localid = NULL;
	ks_bool_t propagate = KS_FALSE;
	blade_subscription_t *bsub = NULL;

	ks_assert(bh);
	ks_assert(event);
	ks_assert(protocol);
	ks_assert(realm);

	if (!(bs = blade_upstreammgr_session_get(bh->upstreammgr))) {
		ret = KS_STATUS_DISCONNECTED;
		goto done;
	}

	blade_upstreammgr_localid_copy(bh->upstreammgr, bh->pool, &localid);
	ks_assert(localid);

	if (remove) {
		propagate = blade_subscriptionmgr_subscriber_remove(bh->subscriptionmgr, &bsub, event, protocol, realm, localid);
	} else {
		propagate = blade_subscriptionmgr_subscriber_add(bh->subscriptionmgr, &bsub, event, protocol, realm, localid);
		ks_assert(event_callback);
	}
	ks_pool_free(bh->pool, &localid);

	if (bsub) {
		blade_subscription_callback_set(bsub, event_callback);
		blade_subscription_callback_data_set(bsub, event_data);
	}

	if (propagate) ret = blade_protocol_subscribe_raw(bh, event, protocol, realm, remove, callback, data);

done:
	if (bs) blade_session_read_unlock(bs);

	return ret;
}

KS_DECLARE(ks_status_t) blade_protocol_subscribe_raw(blade_handle_t *bh, const char *event, const char *protocol, const char *realm, ks_bool_t remove, blade_rpc_response_callback_t callback, void *data)
{
	ks_status_t ret = KS_STATUS_SUCCESS;
	blade_session_t *bs = NULL;
	ks_pool_t *pool = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;

	ks_assert(bh);
	ks_assert(event);
	ks_assert(protocol);
	ks_assert(realm);

	if (!(bs = blade_upstreammgr_session_get(bh->upstreammgr))) {
		ret = KS_STATUS_DISCONNECTED;
		goto done;
	}

	pool = blade_handle_pool_get(bh);
	ks_assert(pool);

	blade_rpc_request_raw_create(pool, &req, &req_params, NULL, "blade.subscribe");

	cJSON_AddStringToObject(req_params, "event", event);
	cJSON_AddStringToObject(req_params, "protocol", protocol);
	cJSON_AddStringToObject(req_params, "realm", realm);
	if (remove) cJSON_AddTrueToObject(req_params, "remove");

	ks_log(KS_LOG_DEBUG, "Session (%s) subscribe request started\n", blade_session_id_get(bs));

	ret = blade_session_send(bs, req, callback, data);

done:
	if (req) cJSON_Delete(req);
	if (bs) blade_session_read_unlock(bs);

	return ret;
}

// blade.subscribe request handler
ks_bool_t blade_protocol_subscribe_request_handler(blade_rpc_request_t *brpcreq, void *data)
{
	blade_handle_t *bh = NULL;
	blade_session_t *bs = NULL;
	ks_pool_t *pool = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_params_event = NULL;
	const char *req_params_protocol = NULL;
	const char *req_params_realm = NULL;
	cJSON *req_params_remove = NULL;
	ks_bool_t remove = KS_FALSE;
	cJSON *res = NULL;
	cJSON *res_result = NULL;
	ks_bool_t propagate = KS_FALSE;

	ks_assert(brpcreq);

	bh = blade_rpc_request_handle_get(brpcreq);
	ks_assert(bh);

	pool = blade_handle_pool_get(bh);
	ks_assert(pool);

	bs = blade_sessionmgr_session_lookup(blade_handle_sessionmgr_get(bh), blade_rpc_request_sessionid_get(brpcreq));
	ks_assert(bs);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (!req_params) {
		ks_log(KS_LOG_DEBUG, "Session (%s) subscribe request missing 'params' object\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params object");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_event = cJSON_GetObjectCstr(req_params, "event");
	if (!req_params_event) {
		ks_log(KS_LOG_DEBUG, "Session (%s) subscribe request missing 'event'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params event");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_protocol = cJSON_GetObjectCstr(req_params, "protocol");
	if (!req_params_protocol) {
		ks_log(KS_LOG_DEBUG, "Session (%s) subscribe request missing 'protocol'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params protocol");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_realm = cJSON_GetObjectCstr(req_params, "realm");
	if (!req_params_realm) {
		ks_log(KS_LOG_DEBUG, "Session (%s) subscribe request missing 'realm'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params realm");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_remove = cJSON_GetObjectItem(req_params, "remove");
	remove = req_params_remove && req_params_remove->type == cJSON_True;

	// @todo confirm the realm is permitted for the session, this gets complicated with subdomains, skipping for now

	ks_log(KS_LOG_DEBUG, "Session (%s) subscribe request processing\n", blade_session_id_get(bs));

	if (remove) {
		propagate = blade_subscriptionmgr_subscriber_remove(bh->subscriptionmgr, NULL, req_params_event, req_params_protocol, req_params_realm, blade_session_id_get(bs));
	} else {
		propagate = blade_subscriptionmgr_subscriber_add(bh->subscriptionmgr, NULL, req_params_event, req_params_protocol, req_params_realm, blade_session_id_get(bs));
	}

	if (propagate) blade_protocol_subscribe_raw(bh, req_params_event, req_params_protocol, req_params_realm, remove, NULL, NULL);

	// build the actual response finally
	blade_rpc_response_raw_create(&res, &res_result, blade_rpc_request_messageid_get(brpcreq));

	cJSON_AddStringToObject(res_result, "event", req_params_event);
	cJSON_AddStringToObject(res_result, "protocol", req_params_protocol);
	cJSON_AddStringToObject(res_result, "realm", req_params_realm);

	// request was just received on a session that is already read locked, so we can assume the response goes back on the same session without further lookup
	blade_session_send(bs, res, NULL, NULL);

done:

	if (res) cJSON_Delete(res);
	if (bs) blade_session_read_unlock(bs);

	return KS_FALSE;
}


// blade.broadcast request generator
KS_DECLARE(ks_status_t) blade_protocol_broadcast(blade_handle_t *bh, const char *broadcaster_nodeid, const char *event, const char *protocol, const char *realm, cJSON *params, blade_rpc_response_callback_t callback, void *data)
{
	ks_status_t ret = KS_STATUS_SUCCESS;
	ks_pool_t *pool = NULL;
	const char *localid = NULL;

	ks_assert(bh);
	ks_assert(event);
	ks_assert(protocol);
	ks_assert(realm);

	// this will ensure any downstream subscriber sessions, and upstream session if available will be broadcasted to
	pool = blade_handle_pool_get(bh);

	if (!broadcaster_nodeid) {
		blade_upstreammgr_localid_copy(bh->upstreammgr, pool, &localid);
		ks_assert(localid);
		broadcaster_nodeid = localid;
	}

	ret = blade_subscriptionmgr_broadcast(bh->subscriptionmgr, broadcaster_nodeid, NULL, event, protocol, realm, params, callback, data);

	if (localid) ks_pool_free(pool, &localid);

	// @todo must check if the local node is also subscribed to receive the event, this is a special edge case which has some extra considerations
	// if the local node is subscribed to receive the event, it should be received here as a special case, otherwise the broadcast request handler
	// is where this normally occurs, however this is not a simple case as the callback expects a blade_rpc_request_t parameter containing context

	return ret;
}

// blade.broadcast request handler
ks_bool_t blade_protocol_broadcast_request_handler(blade_rpc_request_t *brpcreq, void *data)
{
	ks_bool_t ret = KS_FALSE;
	blade_handle_t *bh = NULL;
	blade_session_t *bs = NULL;
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_params_broadcaster_nodeid = NULL;
	const char *req_params_event = NULL;
	const char *req_params_protocol = NULL;
	const char *req_params_realm = NULL;
	cJSON *req_params_params = NULL;
	blade_subscription_t *bsub = NULL;
	blade_rpc_request_callback_t callback = NULL;
	cJSON *res = NULL;
	cJSON *res_result = NULL;

	ks_assert(brpcreq);

	bh = blade_rpc_request_handle_get(brpcreq);
	ks_assert(bh);

	bs = blade_sessionmgr_session_lookup(blade_handle_sessionmgr_get(bh), blade_rpc_request_sessionid_get(brpcreq));
	ks_assert(bs);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (!req_params) {
		ks_log(KS_LOG_DEBUG, "Session (%s) broadcast request missing 'params' object\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params object");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_broadcaster_nodeid = cJSON_GetObjectCstr(req_params, "broadcaster-nodeid");
	if (!req_params_broadcaster_nodeid) {
		ks_log(KS_LOG_DEBUG, "Session (%s) broadcast request missing 'broadcaster-nodeid'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params broadcaster-nodeid");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_event = cJSON_GetObjectCstr(req_params, "event");
	if (!req_params_event) {
		ks_log(KS_LOG_DEBUG, "Session (%s) broadcast request missing 'event'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params event");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_protocol = cJSON_GetObjectCstr(req_params, "protocol");
	if (!req_params_protocol) {
		ks_log(KS_LOG_DEBUG, "Session (%s) broadcast request missing 'protocol'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params protocol");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_realm = cJSON_GetObjectCstr(req_params, "realm");
	if (!req_params_realm) {
		ks_log(KS_LOG_DEBUG, "Session (%s) broadcast request missing 'realm'\n", blade_session_id_get(bs));
		blade_rpc_error_raw_create(&res, NULL, blade_rpc_request_messageid_get(brpcreq), -32602, "Missing params realm");
		blade_session_send(bs, res, NULL, NULL);
		goto done;
	}

	req_params_params = cJSON_GetObjectItem(req_params, "params");

	blade_subscriptionmgr_broadcast(bh->subscriptionmgr, req_params_broadcaster_nodeid, blade_session_id_get(bs), req_params_event, req_params_protocol, req_params_realm, req_params_params, NULL, NULL);

	bsub = blade_subscriptionmgr_subscription_lookup(bh->subscriptionmgr, req_params_event, req_params_protocol, req_params_realm);
	if (bsub) {
		const char *localid = NULL;
		ks_pool_t *pool = NULL;

		pool = blade_handle_pool_get(bh);

		blade_upstreammgr_localid_copy(bh->upstreammgr, pool, &localid);
		ks_assert(localid);

		if (ks_hash_search(blade_subscription_subscribers_get(bsub), (void *)localid, KS_UNLOCKED)) {
			callback = blade_subscription_callback_get(bsub);
			if (callback) ret = callback(brpcreq, blade_subscription_callback_data_get(bsub));
		}
		ks_pool_free(pool, &localid);
	}

	// build the actual response finally
	blade_rpc_response_raw_create(&res, &res_result, blade_rpc_request_messageid_get(brpcreq));

	cJSON_AddStringToObject(res_result, "broadcaster-nodeid", req_params_broadcaster_nodeid);
	cJSON_AddStringToObject(res_result, "event", req_params_event);
	cJSON_AddStringToObject(res_result, "protocol", req_params_protocol);
	cJSON_AddStringToObject(res_result, "realm", req_params_realm);

	// request was just received on a session that is already read locked, so we can assume the response goes back on the same session without further lookup
	blade_session_send(bs, res, NULL, NULL);


done:

	if (res) cJSON_Delete(res);
	if (bs) blade_session_read_unlock(bs);

	return ret;
}

KS_DECLARE(const char *) blade_protocol_broadcast_request_broadcaster_nodeid_get(blade_rpc_request_t *brpcreq)
{
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	const char *req_broadcaster_nodeid = NULL;

	ks_assert(brpcreq);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (req_params) req_broadcaster_nodeid = cJSON_GetObjectCstr(req_params, "broadcaster-nodeid");

	return req_broadcaster_nodeid;
}

KS_DECLARE(cJSON *) blade_protocol_broadcast_request_params_get(blade_rpc_request_t *brpcreq)
{
	cJSON *req = NULL;
	cJSON *req_params = NULL;
	cJSON *req_params_params = NULL;

	ks_assert(brpcreq);

	req = blade_rpc_request_message_get(brpcreq);
	ks_assert(req);

	req_params = cJSON_GetObjectItem(req, "params");
	if (req_params) req_params_params = cJSON_GetObjectItem(req_params, "params");

	return req_params_params;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */