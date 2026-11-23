// SPDX-License-Identifier: GPL-2.0-only
/*
 * TR-069 Annex G (TR-111 Part 2) STUN client and UDP Connection Request
 * listener: discovers the NAT-mapped UDPConnectionRequestAddress and NATDetected
 * state via STUN Binding Requests, keeps the NAT binding alive, and accepts
 * HMAC-SHA1-authenticated UDP HTTP GET Connection Requests on the same socket.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libubox/md5.h>
#include <libubox/uloop.h>

#include "cwmpd.h"

#define STUN_BINDING_REQUEST	0x0001
#define STUN_BINDING_RESPONSE	0x0101
#define STUN_BINDING_ERROR	0x0111
#define STUN_MAGIC_COOKIE	0x2112A442U

#define ATTR_MAPPED_ADDRESS	0x0001
#define ATTR_USERNAME		0x0006
#define ATTR_MESSAGE_INTEGRITY	0x0008
#define ATTR_ERROR_CODE		0x0009
#define ATTR_REALM		0x0014
#define ATTR_NONCE		0x0015
#define ATTR_XOR_MAPPED		0x0020
#define ATTR_XOR_MAPPED_ALT	0x8020
#define ATTR_CONNREQ_BINDING	0xC001
#define ATTR_BINDING_CHANGE	0xC002

#define CR_BINDING_VALUE	"dslforum.org/TR-111 "

/* ---- minimal RFC 3174 SHA-1 + RFC 2104 HMAC-SHA1 ---- */

static uint32_t rol32(uint32_t v, int n)
{
	return (v << n) | (v >> (32 - n));
}

static void sha1_block(uint32_t h[5], const uint8_t b[64])
{
	uint32_t w[80], a, bb, c, d, e, t, k, f;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
		       ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
	for (i = 16; i < 80; i++)
		w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = h[0]; bb = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) {
			f = (bb & c) | ((~bb) & d);
			k = 0x5A827999U;
		} else if (i < 40) {
			f = bb ^ c ^ d;
			k = 0x6ED9EBA1U;
		} else if (i < 60) {
			f = (bb & c) | (bb & d) | (c & d);
			k = 0x8F1BBCDCU;
		} else {
			f = bb ^ c ^ d;
			k = 0xCA62C1D6U;
		}
		t = rol32(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol32(bb, 30); bb = a; a = t;
	}
	h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
	uint32_t h[5] = { 0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U };
	uint64_t bits = (uint64_t)len * 8;
	uint8_t tail[128] = { 0 };
	size_t rem, tlen, i;

	for (i = 0; i + 64 <= len; i += 64)
		sha1_block(h, data + i);
	rem = len - i;
	memcpy(tail, data + i, rem);
	tail[rem] = 0x80;
	tlen = rem < 56 ? 64 : 128;
	for (i = 0; i < 8; i++)
		tail[tlen - 1 - i] = (uint8_t)(bits >> (i * 8));
	sha1_block(h, tail);
	if (tlen == 128)
		sha1_block(h, tail + 64);
	for (i = 0; i < 5; i++) {
		out[i * 4] = h[i] >> 24;
		out[i * 4 + 1] = h[i] >> 16;
		out[i * 4 + 2] = h[i] >> 8;
		out[i * 4 + 3] = h[i];
	}
}

static void hmac_sha1(const uint8_t *key, size_t klen,
		      const uint8_t *msg, size_t mlen, uint8_t out[20])
{
	uint8_t k0[64] = { 0 }, pad[64 + 64];
	uint8_t *buf;
	uint8_t inner[20];
	size_t i;

	if (klen > 64)
		sha1(key, klen, k0);
	else
		memcpy(k0, key, klen);
	buf = malloc(64 + mlen);
	if (!buf)
		return;
	for (i = 0; i < 64; i++)
		buf[i] = k0[i] ^ 0x36;
	memcpy(buf + 64, msg, mlen);
	sha1(buf, 64 + mlen, inner);
	free(buf);

	for (i = 0; i < 64; i++)
		pad[i] = k0[i] ^ 0x5c;
	memcpy(pad + 64, inner, 20);
	sha1(pad, 84, out);
}

/* ---- STUN + UDP Connection Request state ---- */

static struct uloop_fd stun_fd = { .fd = -1 };
static struct uloop_timeout stun_timer;
static struct sockaddr_in server_addr;
static bool have_server;
static uint8_t txid[16];
static char mapped_addr[32];
static char realm[128], nonce[128];
static bool use_long_term, binding_change;
static unsigned long long last_cr_ts;
static char last_cr_id[64];

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint16_t get_u16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static void rand_bytes(uint8_t *out, size_t len)
{
	FILE *f = fopen("/dev/urandom", "r");
	size_t i;

	if (!f || fread(out, 1, len, f) != len)
		for (i = 0; i < len; i++)
			out[i] = rand();
	if (f)
		fclose(f);
}

static size_t add_attr(uint8_t *buf, size_t off, uint16_t type,
		       const void *val, uint16_t len)
{
	size_t pad = (4 - (len % 4)) % 4;

	put_u16(buf + off, type);
	put_u16(buf + off + 2, len);
	if (len)
		memcpy(buf + off + 4, val, len);
	if (pad)
		memset(buf + off + 4 + len, 0, pad);
	return off + 4 + len + pad;
}

static void send_binding_request(void)
{
	uint8_t pkt[768];
	size_t off = 20;

	if (stun_fd.fd < 0 || !have_server)
		return;
	rand_bytes(txid, sizeof(txid));
	/* Keep RFC 5389 magic cookie in first 4 bytes so both RFC 3489/5389 servers answer */
	put_u32(txid, STUN_MAGIC_COOKIE);
	put_u16(pkt, STUN_BINDING_REQUEST);
	memcpy(pkt + 4, txid, 16);

	off = add_attr(pkt, off, ATTR_CONNREQ_BINDING, CR_BINDING_VALUE, 20);
	if (binding_change) {
		off = add_attr(pkt, off, ATTR_BINDING_CHANGE, NULL, 0);
		binding_change = false;
	}
	if (*cfg.stun_username)
		off = add_attr(pkt, off, ATTR_USERNAME, cfg.stun_username,
			       strlen(cfg.stun_username));
	if (use_long_term && *realm)
		off = add_attr(pkt, off, ATTR_REALM, realm, strlen(realm));
	if (use_long_term && *nonce)
		off = add_attr(pkt, off, ATTR_NONCE, nonce, strlen(nonce));
	if (*cfg.stun_username && *cfg.stun_password) {
		uint8_t key[16];
		const uint8_t *k = (const uint8_t *)cfg.stun_password;
		size_t klen = strlen(cfg.stun_password);
		uint8_t mac[20];

		if (use_long_term && *realm) {
			md5_ctx_t ctx;
			char ha1[640];
			int n = snprintf(ha1, sizeof(ha1), "%s:%s:%s",
					 cfg.stun_username, realm, cfg.stun_password);

			md5_begin(&ctx);
			md5_hash(ha1, n > 0 ? (size_t)n : 0, &ctx);
			md5_end(key, &ctx);
			k = key;
			klen = sizeof(key);
		}
		/* Length field during MESSAGE-INTEGRITY includes this 24-byte attribute */
		put_u16(pkt + 2, (uint16_t)(off + 24 - 20));
		hmac_sha1(k, klen, pkt, off, mac);
		off = add_attr(pkt, off, ATTR_MESSAGE_INTEGRITY, mac, sizeof(mac));
	}
	put_u16(pkt + 2, (uint16_t)(off - 20));
	sendto(stun_fd.fd, pkt, off, 0, (const struct sockaddr *)&server_addr,
	       sizeof(server_addr));
}

static unsigned int keepalive_ms(void)
{
	unsigned int min_s = cfg.stun_min_keepalive ? cfg.stun_min_keepalive : 30;
	unsigned int max_s = cfg.stun_max_keepalive ? cfg.stun_max_keepalive : min_s;

	if (max_s < min_s)
		max_s = min_s;
	return max_s * 1000;
}

static bool local_outbound_ip(struct in_addr *out)
{
	struct sockaddr_in dst = server_addr, local = { 0 };
	socklen_t slen = sizeof(local);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	bool ok = false;

	if (fd < 0)
		return false;
	if (!connect(fd, (struct sockaddr *)&dst, sizeof(dst)) &&
	    !getsockname(fd, (struct sockaddr *)&local, &slen)) {
		*out = local.sin_addr;
		ok = true;
	}
	close(fd);
	return ok;
}

static bool query_param(const char *q, const char *key, char *out, size_t len)
{
	size_t klen = strlen(key);
	const char *p = q;

	while (p && *p) {
		const char *amp = strchr(p, '&');
		size_t seg = amp ? (size_t)(amp - p) : strlen(p);

		if (seg > klen && !strncmp(p, key, klen) && p[klen] == '=') {
			snprintf(out, len, "%.*s", (int)(seg - klen - 1), p + klen + 1);
			return true;
		}
		p = amp ? amp + 1 : NULL;
	}
	return false;
}

static void handle_udp_cr(const char *msg, size_t len)
{
	char req[1024], ts[32], id[64], un[64], cn[64], sig[64], text[256];
	char *uri, *q, *sp;
	uint8_t mac[20];
	char want[41];
	unsigned long long tsv;
	int i, n;

	snprintf(req, sizeof(req), "%.*s", (int)len, msg);
	uri = req + 4;
	while (*uri == ' ')
		uri++;
	sp = uri + strcspn(uri, " \r\n");
	*sp = 0;
	q = strchr(uri, '?');
	if (!q++)
		return;
	if (!query_param(q, "ts", ts, sizeof(ts)) ||
	    !query_param(q, "id", id, sizeof(id)) ||
	    !query_param(q, "un", un, sizeof(un)) ||
	    !query_param(q, "cn", cn, sizeof(cn)) ||
	    !query_param(q, "sig", sig, sizeof(sig)))
		return;
	if (*cfg.cr_username && strcmp(un, cfg.cr_username))
		return;
	tsv = strtoull(ts, NULL, 10);
	if (tsv < last_cr_ts || (tsv == last_cr_ts && !strcmp(id, last_cr_id)))
		return;
	/* TR-111: a UDP connection request is always signed */
	if (!*cfg.cr_password)
		return;
	n = snprintf(text, sizeof(text), "%s%s%s%s", ts, id, un, cn);
	hmac_sha1((const uint8_t *)cfg.cr_password, strlen(cfg.cr_password),
		  (const uint8_t *)text, n > 0 ? (size_t)n : 0, mac);
	for (i = 0; i < 20; i++)
		sprintf(want + i * 2, "%02x", mac[i]);
	if (strcasecmp(want, sig))
		return;
	last_cr_ts = tsv;
	snprintf(last_cr_id, sizeof(last_cr_id), "%s", id);
	cwmp_log(LOG_INFO, "UDP connection request accepted");
	cwmp_session_request("6 CONNECTION REQUEST");
}

static void handle_stun_packet(const uint8_t *pkt, size_t len)
{
	uint16_t type, mlen, err_code = 0;
	size_t off = 20;
	bool have_mapped = false;
	struct in_addr ip = { 0 }, local_ip = { 0 };
	uint16_t port = 0;

	if (len < 20 || memcmp(pkt + 4, txid, 16))
		return;
	type = get_u16(pkt);
	mlen = get_u16(pkt + 2);
	if ((size_t)mlen + 20 > len)
		return;

	while (off + 4 <= (size_t)mlen + 20) {
		uint16_t atype = get_u16(pkt + off);
		uint16_t alen = get_u16(pkt + off + 2);
		const uint8_t *aval = pkt + off + 4;
		size_t step = 4 + alen + ((4 - (alen % 4)) % 4);

		if (off + 4 + alen > (size_t)mlen + 20)
			break;
		if (atype == ATTR_MAPPED_ADDRESS && alen >= 8 && aval[1] == 0x01) {
			port = get_u16(aval + 2);
			memcpy(&ip, aval + 4, 4);
			have_mapped = true;
		} else if ((atype == ATTR_XOR_MAPPED || atype == ATTR_XOR_MAPPED_ALT) &&
			   alen >= 8 && aval[1] == 0x01 && !have_mapped) {
			uint32_t raw_ip;

			port = get_u16(aval + 2) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
			memcpy(&raw_ip, aval + 4, 4);
			raw_ip ^= htonl(STUN_MAGIC_COOKIE);
			memcpy(&ip, &raw_ip, 4);
			have_mapped = true;
		} else if (atype == ATTR_ERROR_CODE && alen >= 4) {
			err_code = (aval[2] & 0x07) * 100 + aval[3];
		} else if (atype == ATTR_REALM) {
			snprintf(realm, sizeof(realm), "%.*s", (int)alen, (const char *)aval);
		} else if (atype == ATTR_NONCE) {
			snprintf(nonce, sizeof(nonce), "%.*s", (int)alen, (const char *)aval);
		}
		off += step;
	}

	if (type == STUN_BINDING_ERROR && err_code == 401 && !use_long_term &&
	    (*realm || *nonce) && *cfg.stun_username) {
		use_long_term = true;
		send_binding_request();
		return;
	}

	if (type == STUN_BINDING_RESPONSE && have_mapped) {
		char buf[32], ipbuf[INET_ADDRSTRLEN];
		struct sockaddr_in local = { 0 };
		socklen_t slen = sizeof(local);
		bool nat = true;

		inet_ntop(AF_INET, &ip, ipbuf, sizeof(ipbuf));
		snprintf(buf, sizeof(buf), "%s:%u", ipbuf, port);
		if (!getsockname(stun_fd.fd, (struct sockaddr *)&local, &slen) &&
		    local_outbound_ip(&local_ip))
			nat = local_ip.s_addr != ip.s_addr || ntohs(local.sin_port) != port;
		if (strcmp(mapped_addr, buf)) {
			bool first = !*mapped_addr;

			snprintf(mapped_addr, sizeof(mapped_addr), "%s", buf);
			cwmp_stun_state(mapped_addr, nat);
			if (!first) {
				binding_change = true;
				send_binding_request();
			}
		} else {
			cwmp_stun_state(mapped_addr, nat);
		}
	}
}

static void stun_read_cb(struct uloop_fd *u, unsigned int events)
{
	uint8_t buf[1500];
	ssize_t n;

	while ((n = recvfrom(u->fd, buf, sizeof(buf), 0, NULL, NULL)) > 0) {
		if (n >= 4 && !strncasecmp((const char *)buf, "GET ", 4))
			handle_udp_cr((const char *)buf, (size_t)n);
		else
			handle_stun_packet(buf, (size_t)n);
	}
}

static void stun_timer_cb(struct uloop_timeout *t)
{
	send_binding_request();
	uloop_timeout_set(t, keepalive_ms());
}

void stun_stop(void)
{
	uloop_timeout_cancel(&stun_timer);
	if (stun_fd.fd >= 0) {
		uloop_fd_delete(&stun_fd);
		close(stun_fd.fd);
		stun_fd.fd = -1;
	}
	have_server = false;
	use_long_term = false;
	binding_change = false;
	*mapped_addr = *realm = *nonce = 0;
}

void stun_start(void)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
	struct addrinfo *res = NULL;
	char port[16];
	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(cfg.cr_port ? cfg.cr_port : 7547),
	};
	int fd, flags;

	stun_stop();
	if (!cfg.stun_enabled || !*cfg.stun_server)
		return;
	snprintf(port, sizeof(port), "%u", cfg.stun_port ? cfg.stun_port : 3478);
	if (getaddrinfo(cfg.stun_server, port, &hints, &res) || !res) {
		cwmp_log(LOG_WARNING, "STUN: cannot resolve %s:%s", cfg.stun_server, port);
		return;
	}
	memcpy(&server_addr, res->ai_addr, sizeof(server_addr));
	have_server = true;
	freeaddrinfo(res);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return;
	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		close(fd);
		return;
	}
	stun_fd.fd = fd;
	stun_fd.cb = stun_read_cb;
	uloop_fd_add(&stun_fd, ULOOP_READ);
	stun_timer.cb = stun_timer_cb;
	binding_change = true;
	stun_timer_cb(&stun_timer);
	cwmp_log(LOG_INFO, "STUN enabled against %s:%s", cfg.stun_server, port);
}
