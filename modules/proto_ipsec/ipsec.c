/*
 * Copyright (C) 2024 - OpenSIPS Solutions
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include "ipsec.h"
#include "ipsec_algo.h"
#include "../../dprint.h"
#include "../../context.h"

struct ipsec_socket *ipsec_new(void)
{
	struct mnl_socket *sock = mnl_socket_open(NETLINK_XFRM);
	if (!sock) {
		LM_ERR("could not create ipsec socket\n");
		return NULL;
	}
	if (mnl_socket_bind(sock, 0, MNL_SOCKET_AUTOPID) < 0) {
		LM_ERR("could not bind ipsec socket\n");
		mnl_socket_close(sock);
		return NULL;
	}
	return sock;
}

void ipsec_close(struct mnl_socket *sock)
{
	if (sock)
		mnl_socket_close(sock);
}

struct ipsec_spi {
	unsigned int spi;
	struct ipsec_spi *free;
};

static unsigned int ipsec_seq;
static unsigned int ipsec_spi_no;
static struct ipsec_spi *ipsec_spi_map;
static struct ipsec_spi **ipsec_spi_free;
static struct ipsec_spi **ipsec_spi_free_last;
static gen_lock_t *ipsec_spi_lock;

unsigned int ipsec_min_spi = IPSEC_DEFAULT_MIN_SPI;
unsigned int ipsec_max_spi = IPSEC_DEFAULT_MAX_SPI;

static int ipsec_ctx_idx = -1;
static void ipsec_ctx_release(struct ipsec_ctx *ctx);

int ipsec_init_spi(void)
{
	unsigned int tmp, spi, r;
	unsigned int *ipsec_spi_shuffle;

	if (ipsec_min_spi > ipsec_max_spi) {
		LM_WARN("min_spi(%u) > max_spi(%u), swapping them\n",
				ipsec_min_spi, ipsec_max_spi);
		tmp = ipsec_max_spi;
		ipsec_max_spi = ipsec_min_spi;
		ipsec_min_spi = tmp;
	}
	if (ipsec_min_spi == 0) {
		LM_ERR("SPI 0 is not allowed!\n");
		return -1;
	}
	ipsec_spi_lock = lock_alloc();
	if (!ipsec_spi_lock || !lock_init(ipsec_spi_lock)) {
		LM_ERR("oom for IPSec SPI lock\n");
		return -1;
	}

	ipsec_spi_free = shm_malloc(sizeof *ipsec_spi_free);
	ipsec_spi_free_last = shm_malloc(sizeof *ipsec_spi_free_last);
	if (!ipsec_spi_free || !ipsec_spi_free_last) {
		LM_ERR("oom for IPSec SPI pointers\n");
		return -1;
	}
	/* spi is in [ipsec_min_spi, ipsec_max_spi] intervall */;
	ipsec_spi_no = ipsec_max_spi - ipsec_min_spi + 1;
	ipsec_spi_map = shm_malloc(ipsec_spi_no * sizeof (*ipsec_spi_map));
	if (!ipsec_spi_map) {
		LM_ERR("oom for IPSec SPI map\n");
		return -1;
	}
	memset(ipsec_spi_map, 0, ipsec_spi_no * sizeof (*ipsec_spi_map));
	/* initialize them in order */
	for (spi = 0; spi < ipsec_spi_no; spi++)
		ipsec_spi_map[spi].spi = spi + ipsec_min_spi;

	/* now we create a new array with them suffled */
	ipsec_spi_shuffle = pkg_malloc(ipsec_spi_no * sizeof (*ipsec_spi_shuffle));
	if (!ipsec_spi_shuffle) {
		LM_ERR("oom for IPSec SPI shuffle map\n");
		return -1;
	}
	for (spi = 0; spi < ipsec_spi_no; spi++)
		ipsec_spi_shuffle[spi] = spi;
	/* now apply Fisher-Yates shuffling */
	for (spi =  ipsec_spi_no - 1; spi > 0; spi--) {
		r = rand() % (spi + 1);
		tmp = ipsec_spi_shuffle[r];
		ipsec_spi_shuffle[r] = ipsec_spi_shuffle[spi];
		ipsec_spi_shuffle[spi] = tmp;
	}

	/* now add them in the free array in the (reversed) order they were shuffled */
	for (spi = 0; spi < ipsec_spi_no; spi++) {
		ipsec_spi_map[ipsec_spi_shuffle[spi]].free = *ipsec_spi_free;
		*ipsec_spi_free = &ipsec_spi_map[ipsec_spi_shuffle[spi]];
	}

	/* the first element in the shuffle is the last one added in the free list */
	*ipsec_spi_free_last = &ipsec_spi_map[ipsec_spi_shuffle[0]];
	pkg_free(ipsec_spi_shuffle);

	return 0;
}

static struct ipsec_spi *ipsec_alloc_spi(unsigned int reserved_spi1,
		unsigned int reserved_spi2)
{
	struct ipsec_spi *it = NULL, *prev = NULL;

	lock_get(ipsec_spi_lock);
	for (it = *ipsec_spi_free; it; it = it->free) {
		if (it->spi != reserved_spi1 && it->spi != reserved_spi2)
			break;
		prev = it;
	}
	if (it) {
		/* unlink from free */
		if (!prev) {
			*ipsec_spi_free = it->free;
			if (*ipsec_spi_free == NULL) /* no more free elements! */
				*ipsec_spi_free_last = NULL;
			else
				it->free = NULL;
		} else {
			prev->free = it->free;
			if (!prev->free)
				*ipsec_spi_free_last = prev;
			else
				it->free = NULL; /* mark it as free now */
		}
		LM_DBG("allocated SPI %u\n", it->spi);
	} else {
		LM_CRIT("no more SPI available\n");
	}
	lock_release(ipsec_spi_lock);
	return it;
}

static void ipsec_spi_release(struct ipsec_spi *spi)
{
	lock_get(ipsec_spi_lock);
	if (!spi->free) {
		/* add it to the end of list */
		if (*ipsec_spi_free_last != NULL)
			(*ipsec_spi_free_last)->free = spi;
		else
			*ipsec_spi_free = spi;
		*ipsec_spi_free_last = spi;
		LM_DBG("released SPI %u\n", spi->spi);
	} else {
		LM_BUG("releasing already released SPI %u\n", spi->spi);
	}
	lock_release(ipsec_spi_lock);
}

int ipsec_init(void)
{
	if (ipsec_init_spi() < 0)
		return -1;

	ipsec_seq = rand();
	ipsec_ctx_idx = context_register_ptr(CONTEXT_GLOBAL, (context_destroy_f)ipsec_ctx_release);

	return 0;
}

void ipsec_destroy(void)
{
	lock_destroy(ipsec_sip_lock);
	shm_free(ipsec_spi_map);
}

void ipsec_fill_selector(struct xfrm_selector *sel,
		struct ip_addr *src_ip, unsigned short src_port,
		struct ip_addr *dst_ip, unsigned short dst_port)
{
	/* selector - this handles both IPv4 and IPv6 */
	sel->family = src_ip->af;
	memcpy(&sel->saddr, &src_ip->u, src_ip->len);
	memcpy(&sel->daddr, &dst_ip->u, dst_ip->len);
	sel->prefixlen_s = src_ip->len * 8;
	sel->prefixlen_d = dst_ip->len * 8;
	sel->sport = htons(src_port);
	sel->dport = htons(dst_port);
	sel->sport_mask = ~0;
	sel->dport_mask = ~0;
	sel->user = IPSEC_USER_SELECTOR;
	sel->proto = 0; /* to avoid adding two SAs for both TCP and UDP,
							   we allow any protocol in the selector */
}

void ipsec_rm_flow(struct ipsec_socket *sock, struct ipsec_ctx *ctx,
		enum ipsec_dir dir, int client)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct xfrm_usersa_id *sa_id;
	xfrm_address_t saddr;
	struct ipsec_endpoint *src, *dst;
	unsigned short src_port, dst_port;
	unsigned int spi;

	if (dir == IPSEC_POLICY_IN) {
		src = &ctx->ue;
		dst = &ctx->me;
	} else {
		src = &ctx->me;
		dst = &ctx->ue;
	}
	if (!client) {
		src_port = src->port_c;
		dst_port = dst->port_s;
		spi = dst->spi_s;
	} else {
		src_port = src->port_s;
		dst_port = dst->port_c;
		spi = dst->spi_c;
	}

	memset(buf, 0, sizeof buf);
	nlh = mnl_nlmsg_put_header(buf);
	if (!nlh) {
		LM_ERR("could not store SA header\n");
		goto error;
	}
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_type = XFRM_MSG_DELSA;
	nlh->nlmsg_seq = ++ipsec_seq;

	sa_id = mnl_nlmsg_put_extra_header(nlh, sizeof (struct xfrm_usersa_id));
	if (!sa_id) {
		LM_ERR("could not get sa_id\n");
		goto error;
	}
	sa_id->spi = htonl(spi);
	sa_id->proto = IPPROTO_ESP;
	sa_id->family = dst->ip.af;
	memcpy(&sa_id->daddr, &dst->ip.u, dst->ip.len);
	memcpy(&saddr, &src->ip.u, src->ip.len);

	mnl_attr_put(nlh, XFRMA_SRCADDR, sizeof saddr, &saddr);

	LM_DBG("removed %s:%hu -> %s:%hu SA (SPI %u)\n",
			ip_addr2a(&src->ip), src_port, ip_addr2a(&dst->ip), dst_port, spi);
	return;
error:
	LM_ERR("failed to remove %s:%hu -> %s:%hu SA (SPI %u)\n",
			ip_addr2a(&src->ip), src_port, ip_addr2a(&dst->ip), dst_port, spi);
}

struct xfrm_algo_osips {
	struct xfrm_algo algo;
	char buf[IPSEC_ALGO_MAX_KEY_SIZE];
};

int ipsec_add_flow(struct mnl_socket *sock, struct ipsec_ctx *ctx,
		str *ck, str *ik, enum ipsec_dir dir, int client)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct xfrm_usersa_info *sa_info;
	struct xfrm_userpolicy_info *policy_info;
	struct xfrm_algo_osips ia, ie;
	unsigned short dst_port;
	unsigned short src_port;
	unsigned int spi;
	struct ipsec_endpoint *src, *dst;

	if (dir == IPSEC_POLICY_IN) {
		src = &ctx->ue;
		dst = &ctx->me;
	} else {
		src = &ctx->me;
		dst = &ctx->ue;
	}
	if (!client) {
		src_port = src->port_c;
		dst_port = dst->port_s;
		spi = dst->spi_s;
	} else {
		src_port = src->port_s;
		dst_port = dst->port_c;
		spi = dst->spi_c;
	}

	/*
	 * According to 3GPP TS 33.203, Annex H
	 * The changes compared to RFC 3329 [21] are:
	 *	"alg" parameter: Addition of "aes-gmac" and "null". Removal of "hmac-md5-96"
	 *	"ealg" parameter: Addition of "aes-cbc" and "aes-gcm". Removal of "des-ede3-cbc"
	 *	"mod" parameter: Addition of "UDP-enc-tun"
	 *
	 * "Hmac-sha-1-96" and "aes-cbc" are not recommended.
	 */
	if (ik->len && ik->len != (IPSEC_ALGO_KEY_SIZE / 8) * 2) {
		LM_ERR("invalid authentication key size %d, expected %d\n",
			  ik->len, ((IPSEC_ALGO_KEY_SIZE / 8) * 2));
		goto error;
	}
	if (ck->len && ck->len != (IPSEC_ALGO_KEY_SIZE / 8) * 2) {
		LM_ERR("invalid encryption key size %d, expected %d\n",
			  ck->len, ((IPSEC_ALGO_KEY_SIZE / 8) * 2));
		goto error;
	}
	memset(&ia, 0, sizeof ia);

	if (ctx->alg->deprecated) {
		LM_WARN("%s\n", ctx->alg->deprecated);
		ctx->alg->deprecated = NULL;
	}
	if (ctx->alg->key_len == IPSEC_ALGO_NULL_KEY_SIZE) {
		if (memcmp(ctx->ealg->name, "aes-gcm", sizeof("aes-gcm"))) {
			LM_ERR("null algorithm should only be used with aes-gcm encryption algorithm\n");
			goto error;
		}
	}

	strncpy(ia.algo.alg_name, ctx->alg->xfrm_name, sizeof ia.algo.alg_name);
	ia.algo.alg_key_len = ctx->alg->key_len;
	if ( ctx->alg->key_len != 0 && hex2string(ik->s, ik->len, ia.buf) < 0) {
		LM_ERR("could not hexa decode integrity key [%.*s]\n", ik->len, ik->s);
		goto error;
	}

	memset(&ie, 0, sizeof ie);

	if (ctx->ealg->deprecated) {
		LM_WARN("%s\n", ctx->ealg->deprecated);
		ctx->ealg->deprecated = NULL;
	}
	strncpy(ie.algo.alg_name, ctx->ealg->xfrm_name, sizeof ie.algo.alg_name);
	ie.algo.alg_key_len = ctx->ealg->key_len;
	if (ctx->ealg->key_len != 0 && hex2string(ck->s, ck->len, ie.buf) < 0) {
		LM_ERR("could not hexa decode confidentialitty key [%.*s]\n", ck->len, ck->s);
		goto error;
	}

	/* SA message */
	memset(buf, 0, sizeof buf);
	nlh = mnl_nlmsg_put_header(buf);
	if (!nlh) {
		LM_ERR("could not store SA header\n");
		goto error;
	}
	nlh->nlmsg_flags = NLM_F_REQUEST|NLM_F_CREATE|NLM_F_EXCL;
	nlh->nlmsg_type = XFRM_MSG_NEWSA;
	nlh->nlmsg_seq = ++ipsec_seq;

	sa_info = mnl_nlmsg_put_extra_header(nlh, sizeof (struct xfrm_usersa_info));
	if (!sa_info) {
		LM_ERR("could not get sa_info\n");
		goto error;
	}
	/* selector */
	ipsec_fill_selector(&sa_info->sel, &src->ip, src_port, &dst->ip, dst_port);

	/* id */
	sa_info->id.spi = htonl(spi);
	sa_info->id.proto = IPPROTO_ESP;
	memcpy(&sa_info->id.daddr, &dst->ip.u, dst->ip.len);

	memcpy(&sa_info->saddr, &src->ip.u, src->ip.len);

	sa_info->lft.soft_byte_limit = XFRM_INF;
	sa_info->lft.hard_byte_limit = XFRM_INF;
	sa_info->lft.soft_packet_limit = XFRM_INF;
	sa_info->lft.hard_packet_limit = XFRM_INF;

	sa_info->seq = ipsec_seq;
	sa_info->reqid = htonl(spi);
	sa_info->family = dst->ip.af;
	sa_info->replay_window = 32;
	sa_info->mode = XFRM_MODE_TRANSPORT;

	mnl_attr_put(nlh, XFRMA_ALG_AUTH,
			sizeof(struct xfrm_algo) + ia.algo.alg_key_len, &ia);
	mnl_attr_put(nlh, XFRMA_ALG_CRYPT,
			sizeof(struct xfrm_algo) + ie.algo.alg_key_len, &ie);

	if (mnl_socket_sendto(sock, nlh, nlh->nlmsg_len) < 0) {
		LM_ERR("communicating with kernel for SA: %s\n", strerror(errno));
		goto error;
	}

	/* Policy message */
	memset(buf, 0, sizeof buf);
	nlh = mnl_nlmsg_put_header(buf);
	if (!nlh) {
		LM_ERR("could not store policy header\n");
		goto policy_error;
	}
	nlh->nlmsg_flags = NLM_F_REQUEST|NLM_F_CREATE|NLM_F_EXCL;
	nlh->nlmsg_type = XFRM_MSG_NEWPOLICY;
	nlh->nlmsg_seq = ++ipsec_seq;

	policy_info = mnl_nlmsg_put_extra_header(nlh, sizeof (struct xfrm_userpolicy_info));
	if (!policy_info) {
		LM_ERR("could not get policy_info\n");
		goto policy_error;
	}
	/* selector */
	ipsec_fill_selector(&policy_info->sel, &src->ip, src_port, &dst->ip, dst_port);

	policy_info->lft.soft_byte_limit = XFRM_INF;
	policy_info->lft.hard_byte_limit = XFRM_INF;
	policy_info->lft.soft_packet_limit = XFRM_INF;
	policy_info->lft.hard_packet_limit = XFRM_INF;
	policy_info->priority = IPSEC_POLICY_PRIORITY;
	policy_info->index = 0; /* currently not used */
	policy_info->dir = dir;
	policy_info->action = XFRM_POLICY_ALLOW;
	policy_info->flags = 0; /* XFRM_POLICY_LOCALOK|XFRM_POLICY_ICMP */
	policy_info->share = XFRM_SHARE_ANY;

	if (mnl_socket_sendto(sock, nlh, nlh->nlmsg_len) < 0) {
		LM_ERR("communicating with kernel for SA policy: %s\n", strerror(errno));
		goto policy_error;
	}

	LM_DBG("created %s:%hu -> %s:%hu SA (SPI %u)\n",
			ip_addr2a(&src->ip), src_port, ip_addr2a(&dst->ip), dst_port, spi);
	return 0;
policy_error:
	ipsec_rm_flow(sock, ctx, dir, client);
error:
	LM_ERR("failed to create %s:%hu -> %s:%hu SA (SPI %u)\n",
			ip_addr2a(&src->ip), src_port, ip_addr2a(&dst->ip), dst_port, spi);
	return -1;
}

#define IPSEC_GET_CTX() ((struct ipsec_ctx *)context_get_ptr(CONTEXT_GLOBAL, \
		current_processing_ctx, ipsec_ctx_idx))
#define IPSEC_PUT_CTX(_p) context_put_ptr(CONTEXT_GLOBAL, \
		current_processing_ctx, ipsec_ctx_idx, (_p))

static void ipsec_ctx_free(struct ipsec_ctx *ctx)
{
	struct ipsec_socket *sock = ipsec_new();
	if (sock) {
		ipsec_rm_flow(sock, ctx, IPSEC_POLICY_IN, 0);
		ipsec_rm_flow(sock, ctx, IPSEC_POLICY_OUT, 0);
		ipsec_rm_flow(sock, ctx, IPSEC_POLICY_IN, 1);
		ipsec_rm_flow(sock, ctx, IPSEC_POLICY_OUT, 1);
		ipsec_close(sock);
	}
	ipsec_spi_release(ctx->spi_s);
	ipsec_spi_release(ctx->spi_c);
	lock_destroy(&ctx->lock);
	shm_free(ctx);
}

struct ipsec_ctx *ipsec_ctx_new(sec_agree_body_t *sa, struct ip_addr *ip,
		struct socket_info *ss, struct socket_info *sc)
{
	struct ipsec_spi *spi_s, *spi_c;
	struct ipsec_ctx *ctx;
	struct ipsec_algorithm_desc *alg, *ealg;
	str null_ealg = str_init("null"), *ealg_str;

	if (sc->address.af != ip->af) {
		LM_ERR("local AF %d differs from remote AF %d\n", sc->address.af, ip->af);
		return NULL;
	}

	alg = ipsec_parse_algorithm(&sa->ts3gpp.alg_str, IPSEC_ALGO_TYPE_AUTH);
	if (!alg) {
		LM_BUG("unknown authentication algorithm %.*s\n",
				sa->ts3gpp.alg_str.len, sa->ts3gpp.alg_str.s);
		return NULL;
	}
	ealg_str = (sa->ts3gpp.ealg_str.len?&sa->ts3gpp.ealg_str:&null_ealg);
	ealg = ipsec_parse_algorithm(ealg_str, IPSEC_ALGO_TYPE_ENC);
	if (!ealg) {
		LM_BUG("unknown encryption algorithm %.*s\n",
				ealg_str->len, ealg_str->s);
		return NULL;
	}

	/* allocate SPIs */
	spi_c = ipsec_alloc_spi(sa->ts3gpp.spi_c, sa->ts3gpp.spi_s);
	if (!spi_c) {
		LM_ERR("could not allocate new spi-c\n");
		return NULL;
	}
	spi_s = ipsec_alloc_spi(sa->ts3gpp.spi_c, sa->ts3gpp.spi_s);
	if (!spi_s) {
		LM_ERR("could not allocate new spi-s\n");
		ipsec_spi_release(spi_c);
		return NULL;
	}
	ctx = shm_malloc(sizeof *ctx);
	if (!ctx) {
		LM_ERR("oom for a new IPSec ctx\n");
		return NULL;
	}
	memset(ctx, 0, sizeof *ctx);
	ctx->spi_s = spi_s;
	ctx->spi_c = spi_c;
	if (!lock_init(&ctx->lock)) {
		LM_ERR("could not init IPSec ctx lock\n");
		goto error;
	}
	ctx->ref = 1;
	ctx->server = ss;
	ctx->client = sc;
	ctx->alg = alg;
	ctx->ealg = ealg;
	/* own information - shortcut */
	memcpy(&ctx->me.ip, &sc->address, sizeof(struct ip_addr));
	ctx->me.spi_s = spi_s->spi;
	ctx->me.spi_c = spi_c->spi;
	ctx->me.port_s = ss->port_no;
	ctx->me.port_c = sc->port_no;
	memcpy(&ctx->ue.ip, ip, sizeof *ip);
	ctx->ue.spi_s = sa->ts3gpp.spi_s;
	ctx->ue.spi_c = sa->ts3gpp.spi_c;
	ctx->ue.port_s = sa->ts3gpp.port_s;
	ctx->ue.port_c = sa->ts3gpp.port_c;
	return ctx;
error:
	ipsec_ctx_free(ctx);
	return NULL;
}

void ipsec_ctx_push(struct ipsec_ctx *ctx)
{
	/* push the context in msg */
	IPSEC_PUT_CTX(ctx);
}

struct ipsec_ctx *ipsec_ctx_get(void)
{
	return IPSEC_GET_CTX();
}

static void ipsec_ctx_release(struct ipsec_ctx *ctx)
{
	int free = 0;

	if (!ctx)
		return;

	lock_get(&ctx->lock);
	if (ctx->ref > 0) {
		free = (--(ctx->ref) == 0);
	} else {
		LM_BUG("invalid ref %d for ctx %p\n", ctx->ref, ctx);
	}
	lock_release(&ctx->lock);
	if (free)
		ipsec_ctx_free(ctx);
}