/** @file
 *  @brief Service Discovery Protocol handling.
 */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <sys/types.h>
#include <misc/byteorder.h>

#include <bluetooth/log.h>
#include <bluetooth/sdp.h>

#include "l2cap_internal.h"
#include "sdp_internal.h"

#if !defined(CONFIG_BLUETOOTH_DEBUG_SDP)
#undef BT_DBG
#define BT_DBG(fmt, ...)
#endif

#define SDP_PSM 0x0001

#define SDP_CHAN(_ch) CONTAINER_OF(_ch, struct bt_sdp, chan.chan)

#define SDP_DATA_MTU 200

#define SDP_MTU (SDP_DATA_MTU + sizeof(struct bt_sdp_hdr))

#define SDP_SERVICE_HANDLE_BASE 0x10000

struct bt_sdp {
	struct bt_l2cap_br_chan chan;
	struct k_fifo           partial_resp_queue;
	/* TODO: Allow more than one pending request */
};

static struct bt_sdp_record *db;
static uint8_t num_services;

static struct bt_sdp bt_sdp_pool[CONFIG_BLUETOOTH_MAX_CONN];

/* Pool for outgoing SDP packets */
NET_BUF_POOL_DEFINE(sdp_pool, CONFIG_BLUETOOTH_MAX_CONN,
		    BT_L2CAP_BUF_SIZE(SDP_MTU), BT_BUF_USER_DATA_MIN, NULL);

#define SDP_CLIENT_CHAN(_ch) CONTAINER_OF(_ch, struct bt_sdp_client, chan.chan)

#define SDP_CLIENT_MTU 64

struct bt_sdp_client {
	struct bt_l2cap_br_chan              chan;
	/* list of waiting to be resolved UUID params */
	sys_slist_t                          reqs;
	/* required SDP transaction ID */
	uint16_t                             tid;
	/* UUID params holder being now resolved */
	const struct bt_sdp_discover_params *param;
	/* PDU continuation state object */
	struct bt_sdp_pdu_cstate             cstate;
	/* buffer for collecting record data */
	struct net_buf                      *rec_buf;
};

static struct bt_sdp_client bt_sdp_client_pool[CONFIG_BLUETOOTH_MAX_CONN];

/** @brief Callback for SDP connection
 *
 *  Gets called when an SDP connection is established
 *
 *  @param chan L2CAP channel
 *
 *  @return None
 */
static void bt_sdp_connected(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_br_chan *ch = CONTAINER_OF(chan,
						   struct bt_l2cap_br_chan,
						   chan);

	struct bt_sdp *sdp = CONTAINER_OF(ch, struct bt_sdp, chan);

	BT_DBG("chan %p cid 0x%04x", ch, ch->tx.cid);

	k_fifo_init(&sdp->partial_resp_queue);

	ch->tx.mtu = SDP_MTU;
	ch->rx.mtu = SDP_MTU;
}

/** @brief Callback for SDP disconnection
 *
 *  Gets called when an SDP connection is terminated
 *
 *  @param chan L2CAP channel
 *
 *  @return None
 */
static void bt_sdp_disconnected(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_br_chan *ch = CONTAINER_OF(chan,
						   struct bt_l2cap_br_chan,
						   chan);

	struct bt_sdp *sdp = CONTAINER_OF(ch, struct bt_sdp, chan);

	BT_DBG("chan %p cid 0x%04x", ch, ch->tx.cid);

	memset(sdp, 0, sizeof(*sdp));
}

/** @brief Creates an SDP PDU
 *
 *  Creates an empty SDP PDU and returns the buffer
 *
 *  @param None
 *
 *  @return Pointer to the net_buf buffer
 */
struct net_buf *bt_sdp_create_pdu(void)
{
	struct net_buf *buf;

	buf = bt_l2cap_create_pdu(&sdp_pool, 0);
	/* NULL is not a possible return due to K_FOREVER */
	net_buf_reserve(buf, sizeof(struct bt_sdp_hdr));

	return buf;
}

/** @brief Sends out an SDP PDU
 *
 *  Sends out an SDP PDU after adding the relevant header
 *
 *  @param chan L2CAP channel
 *  @param buf Buffer to be sent out
 *  @param op Opcode to be used in the packet header
 *  @param tid Transaction ID to be used in the packet header
 *
 *  @return None
 */
static void bt_sdp_send(struct bt_l2cap_chan *chan, struct net_buf *buf,
			uint8_t op, uint16_t tid)
{
	struct bt_sdp_hdr *hdr;
	uint16_t param_len = buf->len;

	hdr = net_buf_push(buf, sizeof(struct bt_sdp_hdr));
	hdr->op_code = op;
	hdr->tid = tid;
	hdr->param_len = sys_cpu_to_be16(param_len);

	bt_l2cap_chan_send(chan, buf);
}

/** @brief Sends an error response PDU
 *
 *  Creates and sends an error response PDU
 *
 *  @param chan L2CAP channel
 *  @param err Error code to be sent in the packet
 *  @param tid Transaction ID to be used in the packet header
 *
 *  @return None
 */
static void send_err_rsp(struct bt_l2cap_chan *chan, uint16_t err,
			 uint16_t tid)
{
	struct net_buf *buf;

	BT_DBG("tid %u, error %u", tid, err);

	buf = bt_sdp_create_pdu();

	net_buf_add_be16(buf, err);

	bt_sdp_send(chan, buf, BT_SDP_ERROR_RSP, tid);
}

static const struct {
	uint8_t  op_code;
	uint16_t  (*func)(struct bt_sdp *sdp, struct net_buf *buf,
			  uint16_t tid);
} handlers[] = {
};

/** @brief Callback for SDP data receive
 *
 *  Gets called when an SDP PDU is received. Calls the corresponding handler
 *  based on the op code of the PDU.
 *
 *  @param chan L2CAP channel
 *  @param buf Received PDU
 *
 *  @return None
 */
static void bt_sdp_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct bt_l2cap_br_chan *ch = CONTAINER_OF(chan,
			struct bt_l2cap_br_chan, chan);
	struct bt_sdp *sdp = CONTAINER_OF(ch, struct bt_sdp, chan);
	struct bt_sdp_hdr *hdr = (struct bt_sdp_hdr *)buf->data;
	uint16_t err = BT_SDP_INVALID_SYNTAX;
	size_t i;

	BT_DBG("chan %p, ch %p, cid 0x%04x", chan, ch, ch->tx.cid);

	BT_ASSERT(sdp);

	if (buf->len < sizeof(*hdr)) {
		BT_ERR("Too small SDP PDU received");
		return;
	}

	BT_DBG("Received SDP code 0x%02x len %u", hdr->op_code, buf->len);

	net_buf_pull(buf, sizeof(*hdr));

	if (sys_cpu_to_be16(hdr->param_len) != buf->len) {
		err = BT_SDP_INVALID_PDU_SIZE;
	} else {
		for (i = 0; i < ARRAY_SIZE(handlers); i++) {
			if (hdr->op_code != handlers[i].op_code) {
				continue;
			}

			err = handlers[i].func(sdp, buf, hdr->tid);
			break;
		}
	}

	if (err) {
		BT_WARN("SDP error 0x%02x", err);
		send_err_rsp(chan, err, hdr->tid);
	}
}

/** @brief Callback for SDP connection accept
 *
 *  Gets called when an incoming SDP connection needs to be authorized.
 *  Registers the L2CAP callbacks and allocates an SDP context to the connection
 *
 *  @param conn BT connection object
 *  @param chan L2CAP channel structure (to be returned)
 *
 *  @return 0 for success, or relevant error code
 */
static int bt_sdp_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan)
{
	static struct bt_l2cap_chan_ops ops = {
		.connected = bt_sdp_connected,
		.disconnected = bt_sdp_disconnected,
		.recv = bt_sdp_recv,
	};
	int i;

	BT_DBG("conn %p", conn);

	for (i = 0; i < ARRAY_SIZE(bt_sdp_pool); i++) {
		struct bt_sdp *sdp = &bt_sdp_pool[i];

		if (sdp->chan.chan.conn) {
			continue;
		}

		sdp->chan.chan.ops = &ops;
		sdp->chan.rx.mtu = SDP_MTU;

		*chan = &sdp->chan.chan;

		return 0;
	}

	BT_ERR("No available SDP context for conn %p", conn);

	return -ENOMEM;
}

void bt_sdp_init(void)
{
	static struct bt_l2cap_server server = {
		.psm = SDP_PSM,
		.accept = bt_sdp_accept,
	};
	int res;

	res = bt_l2cap_br_server_register(&server);
	if (res) {
		BT_ERR("L2CAP server registration failed with error %d", res);
	}
}

int bt_sdp_register_service(struct bt_sdp_record *service)
{
	uint32_t handle = SDP_SERVICE_HANDLE_BASE;

	if (!service) {
		BT_ERR("No service record specified");
		return 0;
	}

	if (db) {
		handle = db->handle + 1;
	}

	service->next = db;
	service->index = num_services++;
	service->handle = handle;
	*((uint32_t *)(service->attrs[0].val.data)) = handle;
	db = service;

	BT_DBG("Service registered at %u", handle);

	return 0;
}

#define GET_PARAM(__node) \
	CONTAINER_OF(__node, struct bt_sdp_discover_params, _node)

/* ServiceSearchAttribute PDU, ref to BT Core 4.2, Vol 3, part B, 4.7.1 */
static int sdp_client_ssa_search(struct bt_sdp_client *session)
{
	const struct bt_sdp_discover_params *param;
	struct bt_sdp_hdr *hdr;
	struct net_buf *buf;

	/*
	 * Select proper user params, if session->param is invalid it means
	 * getting new UUID from top of to be resolved params list. Otherwise
	 * the context is in a middle of partial SDP PDU responses and cached
	 * value from context can be used.
	 */
	if (!session->param) {
		param = GET_PARAM(sys_slist_peek_head(&session->reqs));
	} else {
		param = session->param;
	}

	if (!param) {
		BT_WARN("No UUIDs to be resolved on remote");
		return -EINVAL;
	}

	buf = bt_l2cap_create_pdu(&sdp_pool, 0);
	if (!buf) {
		BT_ERR("No bufs for PDU");
		return -ENOMEM;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));

	hdr->op_code = BT_SDP_SVC_SEARCH_ATTR_REQ;
	/* BT_SDP_SEQ8 means length of sequence is on additional next byte */
	net_buf_add_u8(buf, BT_SDP_SEQ8);

	switch (param->uuid->type) {
	case BT_UUID_TYPE_16:
		/* Seq length */
		net_buf_add_u8(buf, 0x03);
		/* Seq type */
		net_buf_add_u8(buf, BT_SDP_UUID16);
		/* Seq value */
		net_buf_add_be16(buf, BT_UUID_16(param->uuid)->val);
		break;
	case BT_UUID_TYPE_32:
		net_buf_add_u8(buf, 0x05);
		net_buf_add_u8(buf, BT_SDP_UUID32);
		net_buf_add_be32(buf, BT_UUID_32(param->uuid)->val);
		break;
	case BT_UUID_TYPE_128:
		net_buf_add_u8(buf, 0x11);
		net_buf_add_u8(buf, BT_SDP_UUID128);
		net_buf_add_mem(buf, BT_UUID_128(param->uuid)->val,
				ARRAY_SIZE(BT_UUID_128(param->uuid)->val));
		break;
	default:
		BT_ERR("Unknown UUID type %u", param->uuid->type);
		return -EINVAL;
	}

	/* Set attribute max bytes count to be returned from server */
	net_buf_add_be16(buf, BT_SDP_MAX_ATTR_LEN);
	/*
	 * Sequence definition where data is sequence of elements and where
	 * additional next byte points the size of elements within
	 */
	net_buf_add_u8(buf, BT_SDP_SEQ8);
	net_buf_add_u8(buf, 0x05);
	/* Data element definition for two following 16bits range elements */
	net_buf_add_u8(buf, BT_SDP_UINT32);
	/* Get all attributes. It enables filter out wanted only attributes */
	net_buf_add_be16(buf, 0x0000);
	net_buf_add_be16(buf, 0xffff);

	/*
	 * Update and validate PDU ContinuationState. Initial SSA Request has
	 * zero length continuation state since no interaction has place with
	 * server so far, otherwise use the original state taken from remote's
	 * last response PDU that is cached by SDP client context.
	 */
	if (session->cstate.length == 0) {
		net_buf_add_u8(buf, 0x00);
	} else {
		net_buf_add_u8(buf, session->cstate.length);
		net_buf_add_mem(buf, session->cstate.data,
				session->cstate.length);
	}

	/* set overall PDU length */
	hdr->param_len = sys_cpu_to_be16(buf->len - sizeof(*hdr));

	/* Update context param to the one being resolving now */
	session->param = param;
	session->tid++;
	hdr->tid = sys_cpu_to_be16(session->tid);

	return bt_l2cap_chan_send(&session->chan.chan, buf);
}

static void sdp_client_params_iterator(struct bt_sdp_client *session)
{
	struct bt_l2cap_chan *chan = &session->chan.chan;
	const struct bt_sdp_discover_params *param;
	sys_snode_t *node, *node_s;

	SYS_SLIST_FOR_EACH_NODE_SAFE(&session->reqs, node, node_s) {
		param = GET_PARAM(node);
		if (param != session->param) {
			continue;
		}

		BT_DBG("");

		/* Remove already checked UUID node */
		sys_slist_remove(&session->reqs, NULL, node);
		/* Invalidate cached param in context */
		session->param = NULL;
		/* Reset continuation state in current context */
		memset(&session->cstate, 0, sizeof(session->cstate));

		/* Check if there's valid next UUID */
		if (!sys_slist_is_empty(&session->reqs)) {
			sdp_client_ssa_search(session);
			return;
		}

		/* No UUID items, disconnect channel */
		bt_l2cap_chan_disconnect(chan);
		break;
	}
}

static uint16_t sdp_client_get_total(struct bt_sdp_client *session,
				     struct net_buf *buf, uint16_t *total)
{
	uint16_t pulled;
	uint8_t seq;

	/*
	 * Pull value of total octets of all attributes available to be
	 * collected when response gets completed for given UUID. Such info can
	 * be get from the very first response frame after initial SSA request
	 * was sent. For subsequent calls related to the same SSA request input
	 * buf and in/out function parameters stays neutral.
	 */
	if (session->cstate.length == 0) {
		seq = net_buf_pull_u8(buf);
		pulled = 1;
		switch (seq) {
		case BT_SDP_SEQ8:
			*total = net_buf_pull_u8(buf);
			pulled += 1;
			break;
		case BT_SDP_SEQ16:
			*total = net_buf_pull_be16(buf);
			pulled += 2;
			break;
		default:
			BT_WARN("Sequence type 0x%02x not handled", seq);
			*total = 0;
			break;
		}

		BT_DBG("Total %u octets of all attributes", *total);
	} else {
		pulled = 0;
		*total = 0;
	}

	return pulled;
}

static uint16_t get_record_len(struct net_buf *buf)
{
	uint16_t len;
	uint8_t seq;

	seq = net_buf_pull_u8(buf);

	switch (seq) {
	case BT_SDP_SEQ8:
		len = net_buf_pull_u8(buf);
		break;
	case BT_SDP_SEQ16:
		len = net_buf_pull_be16(buf);
		break;
	default:
		BT_WARN("Sequence type 0x%02x not handled", seq);
		len = 0;
		break;
	}

	BT_DBG("Record len %u", len);

	return len;
}

enum uuid_state {
	UUID_NOT_RESOLVED,
	UUID_RESOLVED,
};

static void sdp_client_notify_result(struct bt_sdp_client *session,
				     enum uuid_state state)
{
	struct bt_conn *conn = session->chan.chan.conn;
	struct bt_sdp_client_result result;
	uint16_t rec_len;
	uint8_t user_ret;

	result.uuid = session->param->uuid;

	if (state == UUID_NOT_RESOLVED) {
		result.resp_buf = NULL;
		result.next_record_hint = false;
		session->param->func(conn, &result);
		return;
	}

	while (session->rec_buf->len) {
		struct net_buf_simple_state buf_state;

		rec_len = get_record_len(session->rec_buf);
		/* tell the user about multi record resolution */
		if (session->rec_buf->len > rec_len) {
			result.next_record_hint = true;
		} else {
			result.next_record_hint = false;
		}

		/* save the original session buffer */
		net_buf_simple_save(&session->rec_buf->b, &buf_state);
		/* initialize internal result buffer instead of memcpy */
		result.resp_buf = session->rec_buf;
		/*
		 * Set user internal result buffer length as same as record
		 * length to fake user. User will see the individual record
		 * length as rec_len insted of whole session rec_buf length.
		 */
		result.resp_buf->len = rec_len;

		user_ret = session->param->func(conn, &result);

		/* restore original session buffer */
		net_buf_simple_restore(&session->rec_buf->b, &buf_state);
		/*
		 * sync session buffer data length with next record chunk not
		 * send to user so far
		 */
		net_buf_pull(session->rec_buf, rec_len);
		if (user_ret == BT_SDP_DISCOVER_UUID_STOP) {
			break;
		}
	}
}

static void sdp_client_receive(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct bt_sdp_client *session = SDP_CLIENT_CHAN(chan);
	struct bt_sdp_hdr *hdr = (void *)buf->data;
	struct bt_sdp_pdu_cstate *cstate;
	uint16_t len, tid, frame_len;
	uint16_t total;

	BT_DBG("session %p buf %p", session, buf);

	if (buf->len < sizeof(*hdr)) {
		BT_ERR("Too small SDP PDU");
		return;
	}

	if (hdr->op_code == BT_SDP_ERROR_RSP) {
		BT_INFO("Error SDP PDU response");
		return;
	}

	len = sys_be16_to_cpu(hdr->param_len);
	tid = sys_be16_to_cpu(hdr->tid);
	net_buf_pull(buf, sizeof(*hdr));

	BT_DBG("SDP PDU tid %u len %u", tid, len);

	if (buf->len != len) {
		BT_ERR("SDP PDU length mismatch (%u != %u)", buf->len, len);
		return;
	}

	if (tid != session->tid) {
		BT_ERR("Mismatch transaction ID value in SDP PDU");
		return;
	}

	switch (hdr->op_code) {
	case BT_SDP_SVC_SEARCH_ATTR_RSP:
		/* Get number of attributes in this frame. */
		frame_len = net_buf_pull_be16(buf);
		/* Check valid range of attributes length */
		if (frame_len < 2) {
			BT_ERR("Invalid attributes data length");
			return;
		}

		/* Get PDU continuation state */
		cstate = (struct bt_sdp_pdu_cstate *)(buf->data + frame_len);

		if (cstate->length > BT_SDP_MAX_PDU_CSTATE_LEN) {
			BT_ERR("Invalid SDP PDU Continuation State length %u",
			       cstate->length);
			return;
		}

		if ((frame_len + cstate->length) > len) {
			BT_ERR("Invalid frame payload length");
			return;
		}

		/*
		 * No record found for given UUID. The check catches case when
		 * current response frame has Continuation State shortest and
		 * valid and this is the first response frame as well.
		 */
		if (frame_len == 2 && cstate->length == 0 &&
		    session->cstate.length == 0) {
			BT_DBG("record for UUID 0x%s not found",
				bt_uuid_str(session->param->uuid));
			/* Call user UUID handler */
			sdp_client_notify_result(session, UUID_NOT_RESOLVED);
			net_buf_pull(buf, frame_len + sizeof(cstate->length));
			goto iterate;
		}

		/* Get total value of all attributes to be collected */
		frame_len -= sdp_client_get_total(session, buf, &total);

		if (total > net_buf_tailroom(session->rec_buf)) {
			BT_WARN("Not enough room for getting records data");
			goto iterate;
		}

		net_buf_add_mem(session->rec_buf, buf->data, frame_len);
		net_buf_pull(buf, frame_len);

		/*
		 * check if current response says there's next portion to be
		 * fetched
		 */
		if (cstate->length) {
			/* Cache original Continuation State in context */
			memcpy(&session->cstate, cstate,
			       sizeof(struct bt_sdp_pdu_cstate));

			net_buf_pull(buf, cstate->length +
				     sizeof(cstate->length));

			/* Request for next portion of attributes data */
			sdp_client_ssa_search(session);
			break;
		}

		net_buf_pull(buf, sizeof(cstate->length));

		BT_DBG("UUID 0x%s resolved", bt_uuid_str(session->param->uuid));
		sdp_client_notify_result(session, UUID_RESOLVED);
iterate:
		/* Get next UUID and start resolving it */
		sdp_client_params_iterator(session);
		break;
	default:
		BT_DBG("PDU 0x%0x response not handled", hdr->op_code);
		break;
	}
}

static int sdp_client_chan_connect(struct bt_sdp_client *session)
{
	return bt_l2cap_br_chan_connect(session->chan.chan.conn,
					&session->chan.chan, SDP_PSM);
}

static struct net_buf *sdp_client_alloc_buf(struct bt_l2cap_chan *chan)
{
	struct bt_sdp_client *session = SDP_CLIENT_CHAN(chan);

	BT_DBG("session %p chan %p", session, chan);

	session->param = GET_PARAM(sys_slist_peek_head(&session->reqs));

	return net_buf_alloc(session->param->pool, K_FOREVER);
}

static void sdp_client_connected(struct bt_l2cap_chan *chan)
{
	struct bt_sdp_client *session = SDP_CLIENT_CHAN(chan);

	BT_DBG("session %p chan %p connected", session, chan);

	session->rec_buf = chan->ops->alloc_buf(chan);

	sdp_client_ssa_search(session);
}

static void sdp_client_disconnected(struct bt_l2cap_chan *chan)
{
	struct bt_sdp_client *session = SDP_CLIENT_CHAN(chan);

	BT_DBG("session %p chan %p disconnected", session, chan);

	net_buf_unref(session->rec_buf);

	/*
	 * Reset session excluding L2CAP channel member. Let's the channel
	 * resets autonomous.
	 */
	memset(&session->reqs, 0, sizeof(*session) - sizeof(session->chan));
}

static struct bt_l2cap_chan_ops sdp_client_chan_ops = {
		.connected = sdp_client_connected,
		.disconnected = sdp_client_disconnected,
		.recv = sdp_client_receive,
		.alloc_buf = sdp_client_alloc_buf,
};

static struct bt_sdp_client *sdp_client_new_session(struct bt_conn *conn)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bt_sdp_client_pool); i++) {
		struct bt_sdp_client *session = &bt_sdp_client_pool[i];
		int err;

		if (session->chan.chan.conn) {
			continue;
		}

		sys_slist_init(&session->reqs);

		session->chan.chan.ops = &sdp_client_chan_ops;
		session->chan.chan.conn = conn;
		session->chan.rx.mtu = SDP_CLIENT_MTU;

		err = sdp_client_chan_connect(session);
		if (err) {
			memset(session, 0, sizeof(*session));
			BT_ERR("Cannot connect %d", err);
			return NULL;
		}

		return session;
	}

	BT_ERR("No available SDP client context");

	return NULL;
}

static struct bt_sdp_client *sdp_client_get_session(struct bt_conn *conn)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bt_sdp_client_pool); i++) {
		if (bt_sdp_client_pool[i].chan.chan.conn == conn) {
			return &bt_sdp_client_pool[i];
		}
	}

	/*
	 * Try to allocate session context since not found in pool and attempt
	 * connect to remote SDP endpoint.
	 */
	return sdp_client_new_session(conn);
}

int bt_sdp_discover(struct bt_conn *conn,
		    const struct bt_sdp_discover_params *params)
{
	struct bt_sdp_client *session;

	if (!params || !params->uuid || !params->func || !params->pool) {
		BT_WARN("Invalid user params");
		return -EINVAL;
	}

	session = sdp_client_get_session(conn);
	if (!session) {
		return -ENOMEM;
	}

	sys_slist_append(&session->reqs, (sys_snode_t *)&params->_node);

	return 0;
}

/* Helper getting length of data determined by DTD for integers */
static ssize_t sdp_get_int_len(const uint8_t *data, size_t len)
{
	BT_ASSERT(data);

	switch (data[0]) {
	case BT_SDP_DATA_NIL:
		return sizeof(data[0]);
	case BT_SDP_BOOL:
	case BT_SDP_INT8:
	case BT_SDP_UINT8:
		if (sizeof(data[0]) + sizeof(uint8_t) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint8_t);
	case BT_SDP_INT16:
	case BT_SDP_UINT16:
		if (sizeof(data[0]) + sizeof(uint16_t) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint16_t);
	case BT_SDP_INT32:
	case BT_SDP_UINT32:
		if (sizeof(data[0]) + sizeof(uint32_t) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint32_t);
	case BT_SDP_INT64:
	case BT_SDP_UINT64:
		if (sizeof(data[0]) + sizeof(uint64_t) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint64_t);
	case BT_SDP_INT128:
	case BT_SDP_UINT128:
	default:
		BT_ERR("Invalid/unhandled DTD 0x%02x", data[0]);
		return -EINVAL;
	}

	BT_ERR("Too short buffer length %zu", len);
	return -EMSGSIZE;
}

/* Helper getting length of data determined by DTD for UUID */
static ssize_t sdp_get_uuid_len(const uint8_t *data, size_t len)
{
	BT_ASSERT(data);

	switch (data[0]) {
	case BT_SDP_UUID16:
		if (sizeof(data[0]) + sizeof(uint16_t) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint16_t);
	case BT_SDP_UUID32:
		if (sizeof(data[0]) + sizeof(uint32_t) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint32_t);
	case BT_SDP_UUID128:
	default:
		BT_ERR("Invalid/unhandled DTD 0x%02x", data[0]);
		return -EINVAL;
	}

	BT_ERR("Too short buffer length %zu", len);
	return -EMSGSIZE;
}

/* Helper getting length of data determined by DTD for strings */
static ssize_t sdp_get_str_len(const uint8_t *data, size_t len)
{
	const uint8_t *pnext;

	BT_ASSERT(data);

	/* validate len for pnext safe use to read 8bit value */
	if (len < (sizeof(data[0]) + sizeof(uint8_t))) {
		goto err;
	}

	pnext = data + sizeof(uint8_t);

	switch (data[0]) {
	case BT_SDP_TEXT_STR8:
	case BT_SDP_URL_STR8:
		if ((sizeof(data[0]) + sizeof(uint8_t) + pnext[0]) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint8_t) + pnext[0];
	case BT_SDP_TEXT_STR16:
	case BT_SDP_URL_STR16:
		/* validate len for pnext safe use to read 16bit value */
		if (len < (sizeof(data[0]) + sizeof(uint16_t))) {
			break;
		}

		if ((sizeof(data[0]) + sizeof(uint16_t) + sys_get_be16(pnext)) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint16_t) + sys_get_be16(pnext);
	case BT_SDP_TEXT_STR32:
	case BT_SDP_URL_STR32:
	default:
		BT_ERR("Invalid/unhandled DTD 0x%02x", data[0]);
		return -EINVAL;
	}
err:
	BT_ERR("Too short buffer length %zu", len);
	return -EMSGSIZE;
}

/* Helper getting length of data determined by DTD for sequences */
static ssize_t sdp_get_seq_len(const uint8_t *data, size_t len)
{
	const uint8_t *pnext;

	BT_ASSERT(data);

	/* validate len for pnext safe use to read 8bit bit value */
	if (len < (sizeof(uint8_t) + sizeof(uint8_t))) {
		goto err;
	}

	pnext = data + sizeof(uint8_t);

	switch (data[0]) {
	case BT_SDP_SEQ8:
	case BT_SDP_ALT8:
		if ((sizeof(data[0]) + sizeof(uint8_t) + pnext[0]) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint8_t) + pnext[0];
	case BT_SDP_SEQ16:
	case BT_SDP_ALT16:
		/* validate len for pnext safe use to read 16bit value */
		if (len < (sizeof(data[0]) + sizeof(uint16_t))) {
			break;
		}

		if ((sizeof(data[0]) + sizeof(uint16_t) + sys_get_be16(pnext)) > len) {
			break;
		}

		return sizeof(data[0]) + sizeof(uint16_t) + sys_get_be16(pnext);
	case BT_SDP_SEQ32:
	case BT_SDP_ALT32:
	default:
		BT_ERR("Invalid/unhandled DTD 0x%02x", data[0]);
		return -EINVAL;
	}
err:
	BT_ERR("Too short buffer length %zu", len);
	return -EMSGSIZE;
}

/* Helper getting length of attribute value data */
static ssize_t sdp_get_attr_value_len(const uint8_t *data, size_t len)
{
	BT_ASSERT(data);

	BT_DBG("Attr val DTD 0x%02x", data[0]);

	switch (data[0]) {
	case BT_SDP_DATA_NIL:
	case BT_SDP_BOOL:
	case BT_SDP_UINT8:
	case BT_SDP_UINT16:
	case BT_SDP_UINT32:
	case BT_SDP_UINT64:
	case BT_SDP_UINT128:
	case BT_SDP_INT8:
	case BT_SDP_INT16:
	case BT_SDP_INT32:
	case BT_SDP_INT64:
	case BT_SDP_INT128:
		return sdp_get_int_len(data, len);
	case BT_SDP_UUID16:
	case BT_SDP_UUID32:
	case BT_SDP_UUID128:
		return sdp_get_uuid_len(data, len);
	case BT_SDP_TEXT_STR8:
	case BT_SDP_TEXT_STR16:
	case BT_SDP_TEXT_STR32:
	case BT_SDP_URL_STR8:
	case BT_SDP_URL_STR16:
	case BT_SDP_URL_STR32:
		return sdp_get_str_len(data, len);
	case BT_SDP_SEQ8:
	case BT_SDP_SEQ16:
	case BT_SDP_SEQ32:
	case BT_SDP_ALT8:
	case BT_SDP_ALT16:
	case BT_SDP_ALT32:
		return sdp_get_seq_len(data, len);
	default:
		BT_ERR("Unknown DTD 0x%02x", data[0]);
		return -EINVAL;
	}
}

int bt_sdp_get_attr(struct net_buf *buf, struct bt_sdp_attr_item *attr,
		    uint16_t attr_id)
{
	uint8_t *data;
	uint16_t id;

	data = buf->data;
	while (data - buf->data < buf->len) {
		ssize_t dlen;

		/* data need to point to attribute id descriptor field (DTD)*/
		if (data[0] != BT_SDP_UINT16) {
			BT_ERR("Invalid descriptor 0x%02x", data[0]);
			return -EINVAL;
		}

		data += sizeof(uint8_t);
		id = sys_get_be16(data);
		BT_DBG("Attribute ID 0x%04x", id);
		data += sizeof(uint16_t);

		dlen = sdp_get_attr_value_len(data,
					      buf->len - (data - buf->data));
		if (dlen < 0) {
			BT_ERR("Invalid attribute value data");
			return -EINVAL;
		}

		if (id == attr_id) {
			BT_DBG("Attribute ID 0x%04x Value found", id);
			/*
			 * Initialize attribute value buffer data using selected
			 * data slice from original buffer.
			 */
			attr->val = data;
			attr->len = dlen;
			attr->attr_id = id;
			/* Return location index of data from original buffer */
			return data - buf->data;
		}

		data += dlen;
	}

	return 0;
}

#define GET_CASE_SDP_SEQ(_arg, _ptr)  \
	switch (_ptr[0]) {  \
	case BT_SDP_SEQ8:  \
		_arg = *(++_ptr);  \
		_ptr++;  \
		break;  \
	case BT_SDP_SEQ16:  \
		_arg = sys_get_be16(++_ptr);  \
		_ptr += sizeof(uint16_t);  \
		break;  \
	case BT_SDP_SEQ32:  \
		_arg = sys_get_be32(++_ptr);  \
		_ptr += sizeof(uint32_t);  \
		break;  \
	default:  \
		BT_ERR("Invalid/unhandled DTD 0x%02x", _ptr[0]);  \
		return -EINVAL;  \
	}

static int sdp_get_uuid_list(uint16_t attr_id,
			     const struct bt_sdp_attr_item *attr,
			     struct bt_sdp_uuid_desc *pd, size_t len)
{
	uint8_t *p = attr->val;
	uint32_t tot_len;
	int i;

	BT_ASSERT(p);

	if (attr_id != attr->attr_id) {
		BT_ERR("Mismatch checking attribute ID");
		return -EINVAL;
	}

	/* Attribute value is a SEQ, get length of internal SEQ frame */
	GET_CASE_SDP_SEQ(tot_len, p);

	/* Check if all SEQ len + DTD byte itself overlaps attrib data len */
	if (tot_len + sizeof(uint8_t) > attr->len) {
		BT_ERR("Invalid buffer length");
		return -EINVAL;
	}

	i = 0;
	/* start reading stacked UUIDs in analyzed sequence */
	while (p - attr->val < attr->len && i < len) {
		uint32_t seq_len, left;

		/* how long is current UUID's item and data associated after */
		GET_CASE_SDP_SEQ(seq_len, p);
		left = seq_len;

		/* get and copy stacked UUID value */
		switch (p[0]) {
		case BT_SDP_UUID16:
			memcpy(&pd[i].uuid16,
			       BT_UUID_DECLARE_16(sys_get_be16(++p)),
			       sizeof(struct bt_uuid_16));
			p += sizeof(uint16_t);
			left -= sizeof(uint16_t);
			break;
		case BT_SDP_UUID32:
			memcpy(&pd[i].uuid32,
			       BT_UUID_DECLARE_32(sys_get_be32(++p)),
			       sizeof(struct bt_uuid_32));
			p += sizeof(uint32_t);
			left -= sizeof(uint32_t);
			break;
		default:
			BT_ERR("Invalid/unhandled DTD 0x%02x\n", p[0]);
			return -EINVAL;
		}

		/* include last p[0] size itself in decreasing */
		left -= sizeof(p[0]);

		/*
		 * Initialize each UUID params item's helper buffer collecting
		 * assigned to UUID parameters with proper offset address
		 * pointing to parameter from original response buffer.
		 * If there's bare UUID, there's no following parameter data.
		 * The helper buffer length is set to leftover octets to read.
		 */
		pd[i].attr_id = attr_id;
		pd[i].params_len = left;

		if (!left) {
			pd[i].params = NULL;
		} else {
			pd[i].params = p;
		}

		/*
		 * skip already marked as left octets containing UUID specific
		 * information
		 */
		p += left;
		i++;
	}

	return i;
}

int bt_sdp_get_proto_list(const struct bt_sdp_attr_item *attr,
			  struct bt_sdp_uuid_desc *pd, size_t count)
{
	return sdp_get_uuid_list(BT_SDP_ATTR_PROTO_DESC_LIST, attr, pd, count);
}

/*
 * Helper extracting specific parameters associated with UUID node given in
 * protocol descriptor list or profile descriptor list. Implementation, for now,
 * checks for UUID nodes holding one parameter. Such associated parameters can
 * be more.
 */
static int sdp_get_param_item(struct bt_sdp_uuid_desc *pd_item)
{
	const uint8_t *p = pd_item->params;
	uint16_t param;

	if (pd_item->params_len == 0) {
		BT_DBG("UUID 0x%s got no params", bt_uuid_str(&pd_item->uuid));
		return 0;
	}

	BT_ASSERT(p);

	BT_DBG("getting UUID 0x%s params", bt_uuid_str(&pd_item->uuid));

	switch (p[0]) {
	case BT_SDP_UINT8:
		param = (++p)[0];
		p += sizeof(uint8_t);
		break;
	case BT_SDP_UINT16:
		param = sys_get_be16(++p);
		p += sizeof(uint16_t);
		break;
	default:
		param = 0;
		p += sizeof(uint8_t);
		break;
	}
	/*
	 * Check and return one found parameter associated with UUID. In such
	 * case after getting parameter we should reach data buf end.
	 */
	if (p - pd_item->params == pd_item->params_len) {
		return param;
	}

	return -EINVAL;
}

int bt_sdp_get_proto_param(enum bt_sdp_proto proto, struct bt_sdp_uuid_desc *pd,
			   size_t count)
{
	int i;

	if (proto != RFCOMM && proto != L2CAP) {
		BT_ERR("Invalid value");
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		if ((proto == BT_UUID_32(&pd[i].uuid)->val ||
		     proto == BT_UUID_16(&pd[i].uuid)->val) &&
		    pd[i].attr_id == BT_SDP_ATTR_PROTO_DESC_LIST) {
			BT_DBG("protocol UUID 0x%04x found", proto);
			return sdp_get_param_item(&pd[i]);
		}
	}

	return -EINVAL;
}
