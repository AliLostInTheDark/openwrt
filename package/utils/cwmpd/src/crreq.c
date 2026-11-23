// SPDX-License-Identifier: GPL-2.0-only
/*
 * The ACS-initiated session of TR-069 3.2.2: a GET on the connection request
 * URL, answered after HTTP Digest authentication (RFC 2617, MD5, qop auth),
 * opens a session with event "6 CONNECTION REQUEST". IPv4 and IPv6 each get
 * their own socket; a client is served once, with a bounded request.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <libubox/md5.h>
#include <libubox/uloop.h>
#include <libubox/usock.h>

#include "cwmpd.h"

#define REALM		"cwmpd"
#define MIN_INTERVAL	5		/* seconds between accepted requests */

static struct uloop_fd listen4 = { .fd = -1 }, listen6 = { .fd = -1 };
static char nonce[33];
static time_t last_accept;

static void hex(const unsigned char *in, size_t len, char *out)
{
	size_t i;

	for (i = 0; i < len; i++)
		sprintf(out + i * 2, "%02x", in[i]);
}

static void md5_hex(char out[33], const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void md5_hex(char out[33], const char *fmt, ...)
{
	unsigned char digest[16];
	char data[768];
	md5_ctx_t ctx;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(data, sizeof(data), fmt, ap);
	va_end(ap);
	md5_begin(&ctx);
	md5_hash(data, n < 0 ? 0 : (size_t)n, &ctx);
	md5_end(digest, &ctx);
	hex(digest, sizeof(digest), out);
}

static void new_nonce(void)
{
	unsigned char r[16];
	FILE *f = fopen("/dev/urandom", "r");

	if (!f || fread(r, 1, sizeof(r), f) != sizeof(r)) {
		size_t i;

		for (i = 0; i < sizeof(r); i++)
			r[i] = rand();
	}
	if (f)
		fclose(f);
	hex(r, sizeof(r), nonce);
}

/* The value of @key in a Digest Authorization header, quoted or not. */
static bool digest_field(const char *hdr, const char *key, char *out, size_t len)
{
	size_t klen = strlen(key);
	const char *p = hdr;

	while ((p = strcasestr(p, key))) {
		const char *v = p + klen;

		if ((p == hdr || p[-1] == ' ' || p[-1] == ',') && *v == '=') {
			size_t n;

			v++;
			if (*v == '"') {
				v++;
				n = strcspn(v, "\"");
			} else {
				n = strcspn(v, ", \r\n");
			}
			snprintf(out, len, "%.*s", (int)n, v);
			return true;
		}
		p = v;
	}
	return false;
}

static bool authorized(const char *hdr, const char *uri)
{
	char user[64], realm[64], nc[16], cnonce[64], qop[16], response[40], got_nonce[40];
	char ha1[33], ha2[33], want[33];

	if (!*cfg.cr_username)
		return true;
	if (!hdr || strncasecmp(hdr, "Digest ", 7) ||
	    !digest_field(hdr, "username", user, sizeof(user)) ||
	    !digest_field(hdr, "realm", realm, sizeof(realm)) ||
	    !digest_field(hdr, "nonce", got_nonce, sizeof(got_nonce)) ||
	    !digest_field(hdr, "response", response, sizeof(response)) ||
	    strcmp(user, cfg.cr_username) || strcmp(realm, REALM) ||
	    strcmp(got_nonce, nonce))
		return false;
	md5_hex(ha1, "%s:%s:%s", cfg.cr_username, REALM, cfg.cr_password);
	md5_hex(ha2, "GET:%s", uri);
	if (digest_field(hdr, "qop", qop, sizeof(qop)) &&
	    digest_field(hdr, "nc", nc, sizeof(nc)) &&
	    digest_field(hdr, "cnonce", cnonce, sizeof(cnonce)))
		md5_hex(want, "%s:%s:%s:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2);
	else
		md5_hex(want, "%s:%s:%s", ha1, nonce, ha2);
	return !strcasecmp(want, response);
}

static void respond(int fd, const char *status, bool challenge)
{
	char out[256];
	int n;

	if (challenge) {
		new_nonce();
		n = snprintf(out, sizeof(out), "HTTP/1.1 %s\r\nWWW-Authenticate: Digest "
			     "realm=\"" REALM "\", qop=\"auth\", nonce=\"%s\"\r\n"
			     "Content-Length: 0\r\nConnection: close\r\n\r\n", status, nonce);
	} else {
		n = snprintf(out, sizeof(out), "HTTP/1.1 %s\r\nContent-Length: 0\r\n"
			     "Connection: close\r\n\r\n", status);
	}
	/* a short write means the ACS has gone; there is nobody to tell */
	if (n > 0 && write(fd, out, n) != n)
		return;
}

/* Read one request (headers only, at most 4 KiB, 2 s) and answer it. */
static void serve(int fd)
{
	char req[4096], method[8], uri[128], *auth;
	struct timeval tv = { .tv_sec = 2 };
	size_t len = 0;
	ssize_t n;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (len < sizeof(req) - 1 && (n = read(fd, req + len, sizeof(req) - 1 - len)) > 0) {
		len += n;
		req[len] = 0;
		if (strstr(req, "\r\n\r\n"))
			break;
	}
	req[len] = 0;
	if (sscanf(req, "%7s %127s", method, uri) != 2 || strcmp(method, "GET")) {
		respond(fd, "405 Method Not Allowed", false);
		return;
	}
	size_t path_len = strcspn(uri, "?");
	const char *want_path = *cfg.cr_path ? cfg.cr_path : "/";

	if (strlen(want_path) != path_len || strncmp(uri, want_path, path_len)) {
		respond(fd, "404 Not Found", false);
		return;
	}
	auth = strcasestr(req, "\r\nAuthorization:");
	if (auth) {
		auth += 16;
		auth += strspn(auth, " ");
		auth[strcspn(auth, "\r\n")] = 0;
	}
	if (!authorized(auth, uri)) {
		respond(fd, "401 Unauthorized", true);
		return;
	}
	if (time(NULL) - last_accept < MIN_INTERVAL) {
		respond(fd, "503 Service Unavailable", false);
		return;
	}
	last_accept = time(NULL);
	new_nonce();			/* a nonce answers one request */
	respond(fd, "200 OK", false);
	cwmp_log(LOG_INFO, "connection request accepted");
	cwmp_session_request("6 CONNECTION REQUEST");
}

static void accept_cb(struct uloop_fd *u, unsigned int events)
{
	int fd;

	while ((fd = accept(u->fd, NULL, NULL)) >= 0) {
		serve(fd);
		close(fd);
	}
}

static int listen_on(struct uloop_fd *u, int family_flag)
{
	char port[8];

	snprintf(port, sizeof(port), "%u", cfg.cr_port);
	u->fd = usock(USOCK_TCP | USOCK_SERVER | USOCK_NONBLOCK | USOCK_NUMERIC |
		      family_flag, NULL, port);
	if (u->fd < 0)
		return -errno;
	u->cb = accept_cb;
	uloop_fd_add(u, ULOOP_READ);
	return 0;
}

int crreq_start(void)
{
	int v4 = -1, v6 = -1;

	new_nonce();
	if (cfg.family != AF_INET6)
		v4 = listen_on(&listen4, USOCK_IPV4ONLY);
	if (cfg.family != AF_INET)
		v6 = listen_on(&listen6, USOCK_IPV6ONLY);
	if (v4 && v6) {
		cwmp_log(LOG_ERR, "connection request port %u: cannot listen", cfg.cr_port);
		return -1;
	}
	return 0;
}

static void close_listener(struct uloop_fd *u)
{
	if (u->fd < 0)
		return;
	uloop_fd_delete(u);
	close(u->fd);
	u->fd = -1;
}

void crreq_stop(void)
{
	close_listener(&listen4);
	close_listener(&listen6);
}
