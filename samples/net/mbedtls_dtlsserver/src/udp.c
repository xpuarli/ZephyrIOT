/*
 * Copyright (c) 2017 Intel Corporation
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

#include <zephyr.h>
#include <net/net_core.h>
#include <net/net_context.h>
#include <net/nbuf.h>
#include <net/net_if.h>
#include <string.h>
#include <errno.h>
#include <misc/printk.h>

#include "udp_cfg.h"
#include "udp.h"

#if defined(CONFIG_NET_IPV6)
static const socklen_t addrlen = sizeof(struct sockaddr_in6);

static void set_client_address(struct sockaddr *addr, struct net_buf *rx_buf)
{
	net_ipaddr_copy(&net_sin6(addr)->sin6_addr, &NET_IPV6_BUF(rx_buf)->src);
	net_sin6(addr)->sin6_family = AF_INET6;
	net_sin6(addr)->sin6_port = NET_UDP_BUF(rx_buf)->src_port;
}

#else
static const socklen_t addrlen = sizeof(struct sockaddr_in);

static void set_client_address(struct sockaddr *addr, struct net_buf *rx_buf)
{
	net_ipaddr_copy(&net_sin(addr)->sin_addr, &NET_IPV4_BUF(rx_buf)->src);
	net_sin(addr)->sin_family = AF_INET;
	net_sin(addr)->sin_port = NET_UDP_BUF(rx_buf)->src_port;
}

#endif

static void udp_received(struct net_context *context,
			 struct net_buf *buf, int status, void *user_data)
{
	struct udp_context *ctx = user_data;

	ARG_UNUSED(context);
	ARG_UNUSED(status);

	ctx->rx_nbuf = buf;
	k_sem_give(&ctx->rx_sem);
}

int udp_tx(void *context, const unsigned char *buf, size_t size)
{
	struct udp_context *ctx = context;
	struct net_context *net_ctx;
	struct net_buf *send_buf;

	int rc, len;

	net_ctx = ctx->net_ctx;

	send_buf = net_nbuf_get_tx(net_ctx);
	if (!send_buf) {
		printk("cannot create buf\n");
		return -EIO;
	}

	rc = net_nbuf_append(send_buf, size, (uint8_t *) buf);
	if (!rc) {
		printk("cannot write buf\n");
		return -EIO;
	}

	len = net_buf_frags_len(send_buf);

	rc = net_context_sendto(send_buf, &net_ctx->remote,
				addrlen, NULL, K_FOREVER, NULL, NULL);

	if (rc < 0) {
		printk("Cannot send data to peer (%d)\n", rc);
		net_nbuf_unref(send_buf);
		return -EIO;
	} else {
		return len;
	}
}

int udp_rx(void *context, unsigned char *buf, size_t size)
{
	struct udp_context *ctx = context;
	struct net_context *net_ctx = ctx->net_ctx;
	struct net_buf *rx_buf = NULL;
	uint16_t read_bytes;
	uint8_t *ptr;
	int pos;
	int len;
	int rc;

	k_sem_take(&ctx->rx_sem, K_FOREVER);

	read_bytes = net_nbuf_appdatalen(ctx->rx_nbuf);
	if (read_bytes > size) {
		return -ENOMEM;
	}

	rx_buf = ctx->rx_nbuf;

	set_client_address(&net_ctx->remote, rx_buf);

	ptr = net_nbuf_appdata(rx_buf);
	rx_buf = rx_buf->frags;
	len = rx_buf->len - (ptr - rx_buf->data);
	pos = 0;

	while (rx_buf) {
		memcpy(buf + pos, ptr, len);
		pos += len;

		rx_buf = rx_buf->frags;
		if (!rx_buf) {
			break;
		}

		ptr = rx_buf->data;
		len = rx_buf->len;
	}

	net_nbuf_unref(ctx->rx_nbuf);
	ctx->rx_nbuf = NULL;

	if (read_bytes != pos) {
		return -EIO;
	}

	rc = read_bytes;
	ctx->remaining = 0;

	return rc;
}

#if defined(CONFIG_NET_IPV6)
int udp_init(struct udp_context *ctx)
{
	struct net_context *udp_ctx = { 0 };
	struct net_context *mcast_ctx = { 0 };
	struct sockaddr_in6 my_addr = { 0 };
	struct sockaddr_in6 my_mcast_addr = { 0 };
	int rc;

	k_sem_init(&ctx->rx_sem, 0, UINT_MAX);

	net_ipaddr_copy(&my_mcast_addr.sin6_addr, &mcast_addr);
	my_mcast_addr.sin6_family = AF_INET6;

	net_ipaddr_copy(&my_addr.sin6_addr, &server_addr);
	my_addr.sin6_family = AF_INET6;
	my_addr.sin6_port = htons(SERVER_PORT);

	rc = net_context_get(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, &udp_ctx);
	if (rc < 0) {
		printk("Cannot get network context for IPv6 UDP (%d)", rc);
		return -EIO;
	}

	rc = net_context_bind(udp_ctx, (struct sockaddr *)&my_addr,
			      sizeof(struct sockaddr_in6));
	if (rc < 0) {
		printk("Cannot bind IPv6 UDP port %d (%d)", SERVER_PORT, rc);
		goto error;
	}

	rc = net_context_get(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, &mcast_ctx);
	if (rc < 0) {
		printk("Cannot get receiving IPv6 mcast (%d)", rc);
		goto error;
	}

	rc = net_context_bind(mcast_ctx, (struct sockaddr *)&my_mcast_addr,
			      sizeof(struct sockaddr_in6));
	if (rc < 0) {
		printk("Cannot get bind IPv6 mcast (%d)", rc);
		goto error;
	}

	ctx->rx_nbuf = NULL;
	ctx->remaining = 0;
	ctx->net_ctx = udp_ctx;

	rc = net_context_recv(ctx->net_ctx, udp_received, K_NO_WAIT, ctx);
	if (rc != 0) {
		return -EIO;
	}

	return 0;

error:
	net_context_put(udp_ctx);
	return -EINVAL;
}

#else
int udp_init(struct udp_context *ctx)
{
	struct net_context *udp_ctx = { 0 };
	struct sockaddr_in my_addr4 = { 0 };
	int rc;

	k_sem_init(&ctx->rx_sem, 0, UINT_MAX);

	net_ipaddr_copy(&my_addr4.sin_addr, &server_addr);
	my_addr4.sin_family = AF_INET;
	my_addr4.sin_port = htons(SERVER_PORT);

	rc = net_context_get(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &udp_ctx);
	if (rc < 0) {
		printk("Cannot get network context for IPv4 UDP (%d)", rc);
		return -EIO;
	}

	rc = net_context_bind(udp_ctx, (struct sockaddr *)&my_addr4,
			      sizeof(struct sockaddr_in));
	if (rc < 0) {
		printk("Cannot bind IPv4 UDP port %d (%d)", SERVER_PORT, rc);
		goto error;
	}

	ctx->rx_nbuf = NULL;
	ctx->remaining = 0;
	ctx->net_ctx = udp_ctx;

	rc = net_context_recv(ctx->net_ctx, udp_received, K_NO_WAIT, ctx);
	if (rc != 0) {
		return -EIO;
	}

	return 0;

error:
	net_context_put(udp_ctx);
	return -EINVAL;
}
#endif
