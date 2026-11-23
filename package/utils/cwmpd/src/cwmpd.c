// SPDX-License-Identifier: GPL-2.0-only
/*
 * cwmpd - a TR-069 (CWMP) client that lets the ACS provision but writes nothing
 *
 * It opens sessions with the operator's ACS over IPv4, IPv6 or both, HTTP or
 * HTTPS, and reports the device from a small TR-181 or TR-098 table. What the
 * ACS sets is acknowledged and kept in memory only, so its later reads match;
 * it is logged to /tmp/cwmp/captured.log with passwords and keys hidden.
 * Downloads are fetched into /tmp/cwmp/downloads and never applied. Reboot and
 * FactoryReset are acted out: the next Inform carries the events a real
 * reboot or reset would, and a reset makes the ACS provision from scratch.
 * Uploads are refused (CWMP fault 9001). Every exchange is logged in /tmp/cwmp.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>
#include <libubox/uclient.h>
#include <uci.h>

#include "cwmpd.h"
#include "model.h"
#include "soap.h"

#define LOG_DIR			"/tmp/cwmp"
#define DOWNLOAD_DIR		LOG_DIR "/downloads"
#define UNKNOWN_TIME		"0001-01-01T00:00:00Z"
#define BOOTSTRAP_FLAG		"/etc/cwmpd.bootstrapped"
#define OMCI_ACS_HELPER		"/usr/libexec/cwmpd/omci-acs"
#define SESSION_TIMEOUT_MS	30000
#define MAX_EVENTS		8
#define DM_PARAM(suffix)	(cfg.tr098 ? "InternetGatewayDevice." suffix : "Device." suffix)

struct cwmp_config cfg;

static struct ubus_context *ubus;
static struct model model;
static struct model shadow;		/* what the ACS set; never applied */
static struct model mock;		/* a device's tree to answer from, read-only */

#define SHADOW_FILE		"/etc/cwmp/shadow.tsv"

static bool secret_name(const char *name);
static void mock_load(const char *path);
static void shadow_load(void);
static void shadow_save(void);
static void periodic_reschedule(void);
static struct ustream_ssl_ctx *ssl_ctx;
static const struct ustream_ssl_ops *ssl_ops;

/* Methods this client answers; TR-069 A.3.1 requires the first three. */
static const char *const methods[] = {
	"GetRPCMethods", "GetParameterNames", "GetParameterValues",
	"GetParameterAttributes", "GetQueuedTransfers", "GetAllQueuedTransfers",
	"SetParameterValues", "SetParameterAttributes", "AddObject",
	"DeleteObject", "ScheduleInform", "Reboot", "Download", "FactoryReset",
};

/* Refused with CWMP fault 9001: they would send data out or need a real change. */
static const char *const denied[] = {
	"Upload", "ScheduleDownload", "CancelTransfer", "ChangeDUState",
	"SetVouchers", "GetOptions", "Kicked", "RequestDownload",
};

/* Work the ACS asked for, done once its session has ended. */
static struct {
	bool reboot, reset;
	char reboot_key[33];
	bool download;			/* a fetch is queued or running */
	struct soap_request dl;		/* the Download, for the fetch and the report */
	char dl_start[32];
	bool tc_pending;		/* a TransferComplete waits for the next session */
	char tc_key[33];
	unsigned int tc_fault;
	char tc_start[32], tc_complete[32];
	struct uloop_process fetch;
	struct uloop_timeout sched_timer;
	char sched_key[33];
	unsigned int add_last;		/* the last AddObject instance handed out */
} act;

struct attr_rule {
	char *name;
	unsigned int notification;
	char *access_list;
};

static struct attr_rule *attr_rules;
static unsigned int n_attr_rules;

static char **deleted_paths;
static unsigned int n_deleted_paths;

static struct {
	struct uclient *cl;
	struct buf out, in;		/* the request sent, the reply read */
	char url[512];
	char user[256], pass[256];
	char cookie[512];
	struct soap_event events[MAX_EVENTS];
	char keys[MAX_EVENTS][33];
	unsigned int n_events;
	unsigned int n_sent;		/* events the current Inform carries */
	bool tc_sent;			/* TransferComplete sent this session */
	unsigned int retries;		/* sessions failed in a row */
	unsigned int number;		/* for the log file name */
	bool active, informed, auth_retried;
	int status;
	FILE *log;
	time_t last_start, last_end;
	char last_result[96];
} s;

/* session_timer bounds one exchange, start_timer opens a session (now or after a retry delay) */
static struct uloop_timeout session_timer, start_timer, periodic_timer;

void cwmp_log(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(prio, fmt, ap);
	va_end(ap);
}

/* ---- configuration ---- */

static void opt_copy(struct uci_section *sec, const char *name, char *dst, size_t len,
		     const char *def)
{
	const char *v = sec ? uci_lookup_option_string(sec->package->ctx, sec, name) : NULL;

	snprintf(dst, len, "%s", v ? v : def);
}

static unsigned int opt_uint(struct uci_section *sec, const char *name, unsigned int def)
{
	const char *v = sec ? uci_lookup_option_string(sec->package->ctx, sec, name) : NULL;

	return v && *v ? strtoul(v, NULL, 0) : def;
}

static void config_load(void)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_section *main_sec = NULL, *dev = NULL;
	char family[16], dm[16], buf[128];
	struct sysinfo si;

	if (ctx && !uci_load(ctx, "cwmp", &pkg)) {
		main_sec = uci_lookup_section(ctx, pkg, "main");
		dev = uci_lookup_section(ctx, pkg, "device");
	}

	cfg.enabled = opt_uint(main_sec, "enabled", 0);
	opt_copy(main_sec, "url", cfg.url, sizeof(cfg.url), "");
	opt_copy(main_sec, "username", cfg.username, sizeof(cfg.username), "");
	opt_copy(main_sec, "password", cfg.password, sizeof(cfg.password), "");
	opt_copy(main_sec, "interface", cfg.interface, sizeof(cfg.interface), "wan");
	opt_copy(main_sec, "family", family, sizeof(family), "any");
	cfg.family = !strcmp(family, "ipv4") ? AF_INET :
		     !strcmp(family, "ipv6") ? AF_INET6 : AF_UNSPEC;
	cfg.verify_cert = opt_uint(main_sec, "verify_cert", 1);
	cfg.periodic = opt_uint(main_sec, "periodic_inform", 1);
	cfg.periodic_interval = opt_uint(main_sec, "periodic_interval", 86400);
	if (cfg.periodic_interval < 60)
		cfg.periodic_interval = 60;
	cfg.omci_acs = opt_uint(main_sec, "omci_acs", 1);
	opt_copy(main_sec, "data_model", dm, sizeof(dm), "tr181");
	cfg.tr098 = !strcmp(dm, "tr098");
	cfg.log_sessions = opt_uint(main_sec, "log_sessions", 20);
	cfg.capture_downloads = opt_uint(main_sec, "capture_downloads", 1);
	cfg.download_max_kib = opt_uint(main_sec, "download_max_kib", 32768);
	cfg.mock_reboot_delay = opt_uint(main_sec, "mock_reboot_delay", 60);
	opt_copy(main_sec, "mock_model", cfg.mock_model, sizeof(cfg.mock_model),
		 "/etc/cwmp/mock.tsv");
	cfg.cr_enabled = opt_uint(main_sec, "connection_request", 0);
	cfg.cr_port = opt_uint(main_sec, "connection_request_port", 7547);
	opt_copy(main_sec, "connection_request_path", cfg.cr_path, sizeof(cfg.cr_path), "/");
	opt_copy(main_sec, "connection_request_username", cfg.cr_username,
		 sizeof(cfg.cr_username), "");
	opt_copy(main_sec, "connection_request_password", cfg.cr_password,
		 sizeof(cfg.cr_password), "");
	cfg.stun_enabled = opt_uint(main_sec, "stun_enable", 0);
	opt_copy(main_sec, "stun_server", cfg.stun_server, sizeof(cfg.stun_server), "");
	cfg.stun_port = opt_uint(main_sec, "stun_port", 3478);
	opt_copy(main_sec, "stun_username", cfg.stun_username, sizeof(cfg.stun_username), "");
	opt_copy(main_sec, "stun_password", cfg.stun_password, sizeof(cfg.stun_password), "");
	cfg.stun_min_keepalive = opt_uint(main_sec, "stun_min_keepalive", 30);
	cfg.stun_max_keepalive = opt_uint(main_sec, "stun_max_keepalive", 120);
	cfg.persist_shadow = opt_uint(main_sec, "persist_shadow", 0);

	/* Device identity: configured, else what the board and OMCI say. */
	model_free(&model);
	if (cfg.tr098)
		model_set(&model, "InternetGatewayDevice.DeviceInfo.SpecVersion", "1.0", NULL);
	else
		model_set(&model, "Device.RootDataModelVersion", "2.11", NULL);
#define DEV(param, opt) do { opt_copy(dev, opt, buf, sizeof(buf), ""); \
	model_set(&model, DM_PARAM("DeviceInfo." param), buf, NULL); } while (0)
	DEV("Manufacturer", "manufacturer");
	DEV("ManufacturerOUI", "oui");
	DEV("ProductClass", "product_class");
	DEV("SerialNumber", "serial");
	DEV("HardwareVersion", "hardware_version");
	DEV("SoftwareVersion", "software_version");
	DEV("ProvisioningCode", "provisioning_code");
#undef DEV
	sysinfo(&si);
	snprintf(buf, sizeof(buf), "%ld", si.uptime);
	model_set(&model, DM_PARAM("DeviceInfo.UpTime"), buf, "xsd:unsignedInt");
	model_set(&model, DM_PARAM("ManagementServer.ParameterKey"), "", NULL);
	model_set(&model, DM_PARAM("ManagementServer.PeriodicInformEnable"),
		  cfg.periodic ? "1" : "0", "xsd:boolean");
	snprintf(buf, sizeof(buf), "%u", cfg.periodic_interval);
	model_set(&model, DM_PARAM("ManagementServer.PeriodicInformInterval"), buf,
		  "xsd:unsignedInt");
	model_set(&model, DM_PARAM("ManagementServer.ConnectionRequestURL"), "", NULL);
	model_set(&model, DM_PARAM("ManagementServer.UDPConnectionRequestAddress"), "", NULL);
	model_set(&model, DM_PARAM("ManagementServer.NATDetected"), "0", "xsd:boolean");

	/* the mock tree fills what the device section leaves empty, so the
	 * Inform presents the device the tree came from */
	mock_load(cfg.mock_model);
	if (!*cfg.stun_server) {
		const char *sv = model_get(&mock, DM_PARAM("ManagementServer.STUNServerAddress"));

		if (sv && *sv)
			snprintf(cfg.stun_server, sizeof(cfg.stun_server), "%s", sv);
	}
	if (!*cfg.stun_username) {
		const char *su = model_get(&mock, DM_PARAM("ManagementServer.STUNUsername"));

		if (su && *su)
			snprintf(cfg.stun_username, sizeof(cfg.stun_username), "%s", su);
	}
	{
		const char *sp = model_get(&mock, DM_PARAM("ManagementServer.STUNServerPort"));
		const char *skmin = model_get(&mock, DM_PARAM("ManagementServer.STUNMinimumKeepAlivePeriod"));
		const char *skmax = model_get(&mock, DM_PARAM("ManagementServer.STUNMaximumKeepAlivePeriod"));

		if (sp && *sp && !uci_lookup_option_string(ctx, main_sec, "stun_port"))
			cfg.stun_port = strtoul(sp, NULL, 10);
		if (skmin && *skmin && !uci_lookup_option_string(ctx, main_sec, "stun_min_keepalive"))
			cfg.stun_min_keepalive = strtoul(skmin, NULL, 10);
		if (skmax && *skmax && !uci_lookup_option_string(ctx, main_sec, "stun_max_keepalive"))
			cfg.stun_max_keepalive = strtoul(skmax, NULL, 10);
	}
	{
		static const char *const inform[] = {
			"DeviceInfo.Manufacturer", "DeviceInfo.ManufacturerOUI",
			"DeviceInfo.ProductClass", "DeviceInfo.SerialNumber",
			"DeviceInfo.HardwareVersion", "DeviceInfo.SoftwareVersion",
			"DeviceInfo.ProvisioningCode", "DeviceInfo.SpecVersion",
			"DeviceSummary",
		};
		unsigned int i;

		for (i = 0; i < sizeof(inform) / sizeof(inform[0]); i++) {
			char name[96];
			const char *have, *from;

			snprintf(name, sizeof(name), "%s%s",
				 cfg.tr098 ? "InternetGatewayDevice." : "Device.", inform[i]);
			have = model_get(&model, name);
			from = model_get(&mock, name);
			if (from && *from && (!have || !*have))
				model_set(&model, name, from, NULL);
		}
	}

	if (ctx)
		uci_free_context(ctx);
	if (cfg.persist_shadow)
		shadow_load();
}

/* ---- ubus helpers ---- */

struct iface_addr {
	char v4[INET_ADDRSTRLEN];
	char v6[INET6_ADDRSTRLEN];
};

static void iface_status_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	static const char *const keys[] = { "ipv4-address", "ipv6-address" };
	struct iface_addr *a = req->priv;
	struct blob_attr *list, *entry, *field;
	size_t rem, rem2, rem3;
	unsigned int k;

	for (k = 0; k < 2; k++) {
		blobmsg_for_each_attr(list, msg, rem) {
			if (strcmp(blobmsg_name(list), keys[k]))
				continue;
			blobmsg_for_each_attr(entry, list, rem2)
				blobmsg_for_each_attr(field, entry, rem3) {
					char *dst = k ? a->v6 : a->v4;

					if (!strcmp(blobmsg_name(field), "address") && !*dst)
						snprintf(dst, k ? INET6_ADDRSTRLEN : INET_ADDRSTRLEN,
							 "%s", blobmsg_get_string(field));
				}
		}
	}
}

static void iface_addresses(struct iface_addr *a)
{
	char path[96];
	uint32_t id;

	memset(a, 0, sizeof(*a));
	snprintf(path, sizeof(path), "network.interface.%s", cfg.interface);
	if (!ubus_lookup_id(ubus, path, &id))
		ubus_invoke(ubus, id, "status", NULL, iface_status_cb, a, 2000);
}

/* The URL the ACS calls back on, from the interface's own address. */
static void update_connection_request_url(void)
{
	struct iface_addr a;
	char url[128] = "";

	if (cfg.cr_enabled) {
		iface_addresses(&a);
		if (*a.v4 && cfg.family != AF_INET6)
			snprintf(url, sizeof(url), "http://%s:%u%s", a.v4, cfg.cr_port, cfg.cr_path);
		else if (*a.v6 && cfg.family != AF_INET)
			snprintf(url, sizeof(url), "http://[%s]:%u%s", a.v6, cfg.cr_port, cfg.cr_path);
	}
	model_set(&model, DM_PARAM("ManagementServer.ConnectionRequestURL"), url, NULL);
}

/*
 * Reach the ACS through the configured interface: netifd adds a host route to
 * each of its addresses over that interface's gateway, so an operator's
 * management VLAN works beside the internet default route.
 */
static void route_to_acs(const char *host)
{
	struct addrinfo hints = { .ai_family = cfg.family, .ai_socktype = SOCK_STREAM };
	struct addrinfo *res, *ai;
	char addr[INET6_ADDRSTRLEN];
	struct blob_buf b = {};
	uint32_t id;

	if (!*cfg.interface || ubus_lookup_id(ubus, "network", &id) ||
	    getaddrinfo(host, NULL, &hints, &res))
		return;
	for (ai = res; ai; ai = ai->ai_next) {
		const void *src = ai->ai_family == AF_INET ?
			(const void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr :
			(const void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;

		if (!inet_ntop(ai->ai_family, src, addr, sizeof(addr)))
			continue;
		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "target", addr);
		blobmsg_add_u8(&b, "v6", ai->ai_family == AF_INET6);
		blobmsg_add_string(&b, "interface", cfg.interface);
		ubus_invoke(ubus, id, "add_host_route", b.head, NULL, NULL, 2000);
	}
	blob_buf_free(&b);
	freeaddrinfo(res);
}

/*
 * The ACS the OLT provisioned over OMCI (G.988 TR-069 management server),
 * through a helper that reads the MIB: one line each of URL, user, password.
 */
static bool omci_acs(void)
{
	char line[512];
	FILE *f;
	int i;

	if (!cfg.omci_acs || access(OMCI_ACS_HELPER, X_OK))
		return false;
	f = popen(OMCI_ACS_HELPER, "r");
	if (!f)
		return false;
	for (i = 0; i < 3 && fgets(line, sizeof(line), f); i++) {
		line[strcspn(line, "\r\n")] = 0;
		snprintf(i == 0 ? s.url : i == 1 ? s.user : s.pass,
			 i == 0 ? sizeof(s.url) : sizeof(s.user), "%s", line);
	}
	pclose(f);
	return *s.url;
}

/* ---- logging of the exchange ---- */

static void log_open(void)
{
	char path[64];
	glob_t g;
	size_t i;

	mkdir(LOG_DIR, 0700);
	if (!glob(LOG_DIR "/session-*.log", 0, NULL, &g)) {
		for (i = 0; i + cfg.log_sessions < g.gl_pathc; i++)
			unlink(g.gl_pathv[i]);
		globfree(&g);
	}
	snprintf(path, sizeof(path), LOG_DIR "/session-%010ld.log", (long)time(NULL));
	s.log = fopen(path, "a");
	if (s.log)
		fchmod(fileno(s.log), 0600);
}

/* Write @xml with the value of every secret-looking parameter masked. */
static void log_xml(const char *dir, const char *xml, size_t len)
{
	static const char *const secret[] = { "password", "passphrase", "secret", "key" };
	const char *p = xml, *end = xml + len;
	time_t now = time(NULL);
	char stamp[32];

	if (!s.log)
		return;
	strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
	fprintf(s.log, "---- %s %s, %zu bytes\n", stamp, dir, len);
	while (p < end) {
		const char *name = strstr(p, "<Name>"), *value, *close;
		const char *pw = strstr(p, "<Password>");
		bool hide = false;
		unsigned int i;

		/* a Download's or Upload's own <Password> element */
		if (pw && pw < end && (!name || pw < name)) {
			close = strstr(pw, "</Password>");
			if (!close || close >= end)
				break;
			pw += strlen("<Password>");
			fwrite(p, 1, pw - p, s.log);
			fputs(pw < close ? "(masked)" : "", s.log);
			p = close;
			continue;
		}
		if (!name || name >= end)
			break;
		value = strstr(name, "<Value");
		close = value ? strstr(value, "</Value>") : NULL;
		if (!value || !close || close >= end)
			break;
		for (i = 0; i < sizeof(secret) / sizeof(secret[0]) && !hide; i++) {
			const char *q;

			for (q = name; q < value - strlen(secret[i]) && !hide; q++)
				hide = !strncasecmp(q, secret[i], strlen(secret[i]));
		}
		value = strchr(value, '>') + 1;
		fwrite(p, 1, value - p, s.log);
		fputs(hide ? "(masked)" : "", s.log);
		if (!hide)
			fwrite(value, 1, close - value, s.log);
		p = close;
	}
	fwrite(p, 1, end - p, s.log);
	fputc('\n', s.log);
	fflush(s.log);
}

/* ---- session ---- */

static void session_end(bool ok, const char *result);
static void session_send(void);
static void run_actions(void);
static void now_iso(char *buf, size_t len);

/* Values the log never shows: passwords, keys and shared secrets. */
static bool secret_name(const char *name)
{
	static const char *const words[] = {
		"Password", "Passphrase", "PreSharedKey", "Secret", "WEPKey",
	};
	unsigned int i;

	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (strcasestr(name, words[i]))
			return true;
	return false;
}

static void capture_write(const struct soap_request *req)
{
	char stamp[32];
	time_t now = time(NULL);
	FILE *f;
	int fd;
	unsigned int i;

	mkdir(LOG_DIR, 0700);
	fd = open(LOG_DIR "/captured.log", O_CREAT | O_WRONLY | O_APPEND, 0600);
	if (fd < 0)
		return;
	f = fdopen(fd, "a");
	if (!f) {
		close(fd);
		return;
	}
	strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
	fprintf(f, "---- %s %s", stamp, req->rpc);
	if (*req->path)
		fprintf(f, " object=%s", req->path);
	if (req->has_param_key)
		fprintf(f, " parameter_key=%s", req->param_key);
	if (*req->command_key)
		fprintf(f, " command_key=%s", req->command_key);
	fputc('\n', f);
	for (i = 0; i < req->n_set; i++)
		fprintf(f, "%s = %s\n", req->set_params[i].name,
			secret_name(req->set_params[i].name) ? "(hidden)" :
			req->set_params[i].value);
	if (*req->url)
		fprintf(f, "FileType = %s\nURL = %s\nUsername = %s\nPassword = %s\n"
			"TargetFileName = %s\nFileSize = %lu\nDelaySeconds = %u\n",
			req->file_type, req->url, *req->username ? "(set)" : "",
			*req->password ? "(hidden)" : "", req->target, req->file_size,
			req->delay);
	fclose(f);
}

static void set_parameter_key(const struct soap_request *req)
{
	if (req->has_param_key)
		model_set(&model, DM_PARAM("ManagementServer.ParameterKey"),
			  req->param_key, NULL);
}

static bool is_deleted(const char *name)
{
	unsigned int i;

	for (i = 0; i < n_deleted_paths; i++)
		if (!strncmp(name, deleted_paths[i], strlen(deleted_paths[i])))
			return true;
	return false;
}

static void add_deleted(const char *path)
{
	char **n, *copy;
	unsigned int i;

	for (i = 0; i < n_deleted_paths; i++)
		if (!strcmp(deleted_paths[i], path))
			return;
	copy = strdup(path);
	n = realloc(deleted_paths, (n_deleted_paths + 1) * sizeof(*n));
	if (!copy || !n) {
		free(copy);
		return;
	}
	deleted_paths = n;
	deleted_paths[n_deleted_paths++] = copy;
}

static void free_deleted(void)
{
	unsigned int i;

	for (i = 0; i < n_deleted_paths; i++)
		free(deleted_paths[i]);
	free(deleted_paths);
	deleted_paths = NULL;
	n_deleted_paths = 0;
}

/*
 * What the ACS reads: its own values first, then the device's table, then the
 * mock tree. The entries point into the three layers; only the array is
 * allocated.
 */
static bool view_build(struct model *v)
{
	unsigned int i, n = shadow.n + model.n + mock.n;

	memset(v, 0, sizeof(*v));
	v->p = malloc((n ? n : 1) * sizeof(*v->p));
	if (!v->p)
		return false;
	v->cap = n;
	for (i = 0; i < shadow.n; i++)
		v->p[v->n++] = shadow.p[i];
	for (i = 0; i < model.n; i++)
		if (!is_deleted(model.p[i].name) && !model_get(&shadow, model.p[i].name))
			v->p[v->n++] = model.p[i];
	for (i = 0; i < mock.n; i++)
		if (!is_deleted(mock.p[i].name) && !model_get(&shadow, mock.p[i].name) &&
		    !model_get(&model, mock.p[i].name))
			v->p[v->n++] = mock.p[i];
	return true;
}

/* The next instance under @path: one past the highest any layer knows. */
static unsigned int instance_next(const struct model *v, const char *path)
{
	size_t len = strlen(path);
	unsigned int i, max = act.add_last;

	for (i = 0; i < v->n; i++) {
		unsigned long k;
		char *end;

		if (strncmp(v->p[i].name, path, len))
			continue;
		k = strtoul(v->p[i].name + len, &end, 10);
		if (*end == '.' && k > max)
			max = k;
	}
	return act.add_last = max + 1;
}

/*
 * A mock tree, one parameter a line: name, xsd type and value separated by
 * tabs, with \\, \t and \n escaped in the value. Secrets read back empty,
 * as TR-069 requires of a password.
 */
static const char *xsd_type(const char *t)
{
	static const char *const types[] = {
		"xsd:string", "xsd:unsignedInt", "xsd:int", "xsd:boolean",
		"xsd:dateTime", "xsd:base64", "xsd:long", "xsd:unsignedLong",
		"xsd:hexBinary",
	};
	unsigned int i;

	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++)
		if (!strcmp(t, types[i]))
			return types[i];
	return NULL;
}

static void unescape(char *v)
{
	char *o = v;

	for (; *v; v++) {
		if (*v != '\\' || !v[1]) {
			*o++ = *v;
			continue;
		}
		v++;
		*o++ = *v == 't' ? '\t' : *v == 'n' ? '\n' : *v;
	}
	*o = 0;
}

static void mock_load_into(const char *path, struct model *dst, bool mask_secrets)
{
	char *line = NULL;
	size_t size = 0;
	FILE *f;

	model_free(dst);
	if (!*path || !(f = fopen(path, "r")))
		return;
	while (getline(&line, &size, f) > 0) {
		char *name = line, *type, *value;

		line[strcspn(line, "\r\n")] = 0;
		type = strchr(name, '\t');
		if (!type || *name == '#')
			continue;
		*type++ = 0;
		value = strchr(type, '\t');
		if (!value)
			continue;
		*value++ = 0;
		unescape(value);
		model_append(dst, name,
			     (mask_secrets && secret_name(name)) ? "" : value,
			     xsd_type(type));
	}
	free(line);
	fclose(f);
}

static void mock_load(const char *path)
{
	mock_load_into(path, &mock, true);
	cwmp_log(LOG_INFO, "mock data model: %u parameters from %s", mock.n, path);
}

static void escape_write(FILE *f, const char *s)
{
	for (; s && *s; s++) {
		if (*s == '\\')
			fputs("\\\\", f);
		else if (*s == '\t')
			fputs("\\t", f);
		else if (*s == '\n')
			fputs("\\n", f);
		else if (*s != '\r')
			fputc(*s, f);
	}
}

static void shadow_load(void)
{
	mock_load_into(SHADOW_FILE, &shadow, false);
	if (shadow.n)
		cwmp_log(LOG_INFO, "loaded %u persisted mock parameters from %s",
			 shadow.n, SHADOW_FILE);
}

static void shadow_save(void)
{
	FILE *f;
	unsigned int i;
	int fd;

	if (!cfg.persist_shadow)
		return;
	mkdir("/etc/cwmp", 0700);
	fd = open(SHADOW_FILE ".tmp", O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd < 0)
		return;
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		return;
	}
	for (i = 0; i < shadow.n; i++) {
		/* secrets the ACS set stay in memory, never in the file */
		if (secret_name(shadow.p[i].name))
			continue;
		fputs(shadow.p[i].name, f);
		fputc('\t', f);
		fputs(shadow.p[i].type ? shadow.p[i].type : "xsd:string", f);
		fputc('\t', f);
		escape_write(f, shadow.p[i].value);
		fputc('\n', f);
	}
	fclose(f);
	rename(SHADOW_FILE ".tmp", SHADOW_FILE);
}

static void now_iso(char *buf, size_t len)
{
	time_t t = time(NULL);

	strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
}

static const char *model_type_of(const struct model *m, const char *name)
{
	unsigned int i;

	for (i = 0; i < m->n; i++)
		if (!strcmp(m->p[i].name, name))
			return m->p[i].type;
	return NULL;
}

static void attr_rule_set(const struct soap_attr_set *req)
{
	struct attr_rule *r = NULL;
	unsigned int i;

	for (i = 0; i < n_attr_rules; i++)
		if (!strcmp(attr_rules[i].name, req->name)) {
			r = &attr_rules[i];
			break;
		}
	if (!r) {
		struct attr_rule *arr = realloc(attr_rules,
						(n_attr_rules + 1) * sizeof(*arr));
		if (!arr)
			return;
		attr_rules = arr;
		r = &attr_rules[n_attr_rules++];
		memset(r, 0, sizeof(*r));
		r->name = strdup(req->name);
		if (!r->name) {
			n_attr_rules--;
			return;
		}
	}
	if (req->notif_change)
		r->notification = req->notification;
	if (req->access_change) {
		free(r->access_list);
		r->access_list = req->access_list ? strdup(req->access_list) : NULL;
	}
}

static void attr_rule_lookup(const char *name, unsigned int *notif, const char **acl)
{
	size_t best_len = 0;
	unsigned int i;

	*notif = 0;
	*acl = NULL;
	for (i = 0; i < n_attr_rules; i++) {
		size_t rlen = strlen(attr_rules[i].name);
		bool prefix = rlen && attr_rules[i].name[rlen - 1] == '.';
		bool match = prefix ? !strncmp(name, attr_rules[i].name, rlen) :
				      !strcmp(name, attr_rules[i].name);

		if (match && rlen >= best_len) {
			best_len = rlen;
			*notif = attr_rules[i].notification;
			*acl = attr_rules[i].access_list;
		}
	}
}

static void free_attrs(void)
{
	unsigned int i;

	for (i = 0; i < n_attr_rules; i++) {
		free(attr_rules[i].name);
		free(attr_rules[i].access_list);
	}
	free(attr_rules);
	attr_rules = NULL;
	n_attr_rules = 0;
}

static void apply_runtime_param(const char *name, const char *val, bool *stun_dirty)
{
	if (!strcmp(name, DM_PARAM("ManagementServer.PeriodicInformEnable"))) {
		cfg.periodic = !strcmp(val, "1") || !strcasecmp(val, "true");
		model_set(&model, name, cfg.periodic ? "1" : "0", "xsd:boolean");
		periodic_reschedule();
	} else if (!strcmp(name, DM_PARAM("ManagementServer.PeriodicInformInterval"))) {
		unsigned int iv = strtoul(val, NULL, 10);

		if (iv >= 60) {
			cfg.periodic_interval = iv;
			model_set(&model, name, val, "xsd:unsignedInt");
			periodic_reschedule();
		}
	} else if (!strcmp(name, DM_PARAM("ManagementServer.ConnectionRequestUsername"))) {
		snprintf(cfg.cr_username, sizeof(cfg.cr_username), "%s", val);
	} else if (!strcmp(name, DM_PARAM("ManagementServer.ConnectionRequestPassword"))) {
		snprintf(cfg.cr_password, sizeof(cfg.cr_password), "%s", val);
	} else if (!strcmp(name, DM_PARAM("ManagementServer.STUNEnable"))) {
		cfg.stun_enabled = !strcmp(val, "1") || !strcasecmp(val, "true");
		model_set(&model, name, cfg.stun_enabled ? "1" : "0", "xsd:boolean");
		*stun_dirty = true;
	} else if (!strcmp(name, DM_PARAM("ManagementServer.STUNServerAddress"))) {
		snprintf(cfg.stun_server, sizeof(cfg.stun_server), "%s", val);
		*stun_dirty = true;
	} else if (!strcmp(name, DM_PARAM("ManagementServer.STUNServerPort"))) {
		cfg.stun_port = strtoul(val, NULL, 10);
		*stun_dirty = true;
	} else if (!strcmp(name, DM_PARAM("ManagementServer.STUNUsername"))) {
		snprintf(cfg.stun_username, sizeof(cfg.stun_username), "%s", val);
		*stun_dirty = true;
	} else if (!strcmp(name, DM_PARAM("ManagementServer.STUNPassword"))) {
		snprintf(cfg.stun_password, sizeof(cfg.stun_password), "%s", val);
		*stun_dirty = true;
	} else if (!strcmp(name, DM_PARAM("ManagementServer.STUNMinimumKeepAlivePeriod"))) {
		cfg.stun_min_keepalive = strtoul(val, NULL, 10);
		*stun_dirty = true;
	} else if (!strcmp(name, DM_PARAM("ManagementServer.STUNMaximumKeepAlivePeriod"))) {
		cfg.stun_max_keepalive = strtoul(val, NULL, 10);
		*stun_dirty = true;
	} else if (!strcmp(name, DM_PARAM("DeviceInfo.ProvisioningCode"))) {
		model_set(&model, name, val, NULL);
	}
}

static void queue_event(const char *code, const char *key);

static void reply_rpc(const struct soap_request *req)
{
	struct soap_param *values = NULL;
	const char **names = NULL;
	bool *writable = NULL;
	struct soap_attr *attrs = NULL;
	char **owned = NULL;
	struct model view;
	char body[160];
	unsigned int i;
	int n;

	buf_free(&s.out);
	if (!view_build(&view)) {
		soap_fault(&s.out, req, 9004, "Resources exceeded");
		return;
	}
	if (!strcmp(req->rpc, "SetParameterValues")) {
		bool stun_dirty = false;
		unsigned int max_notif = 0;

		capture_write(req);
		for (i = 0; i < req->n_set; i++) {
			const char *pname = req->set_params[i].name;
			const char *ptype = model_type_of(&view, pname);
			/* the shadow holds the value an earlier entry of this request set */
			const char *prev = model_get(&shadow, pname) ?: model_get(&view, pname);
			unsigned int notif = 0;
			const char *acl = NULL;

			attr_rule_lookup(pname, &notif, &acl);
			if (notif > max_notif &&
			    (!prev || strcmp(prev, req->set_params[i].value)))
				max_notif = notif;
			model_set(&shadow, pname, req->set_params[i].value, ptype);
			apply_runtime_param(pname, req->set_params[i].value, &stun_dirty);
		}
		if (max_notif > 0)
			queue_event("4 VALUE CHANGE", "");
		set_parameter_key(req);
		shadow_save();
		if (stun_dirty)
			stun_start();
		soap_empty_response(&s.out, req, "SetParameterValuesResponse",
				    "<Status>0</Status>");
	} else if (!strcmp(req->rpc, "AddObject")) {
		size_t plen = strlen(req->path);
		unsigned int inst;
		char obj[288];

		capture_write(req);
		if (!plen || req->path[plen - 1] != '.') {
			soap_fault(&s.out, req, 9003, "Invalid arguments");
			goto out;
		}
		if (!model_exists(&view, req->path)) {
			soap_fault(&s.out, req, 9005, "Invalid parameter name");
			goto out;
		}
		inst = instance_next(&view, req->path);
		snprintf(obj, sizeof(obj), "%s%u.", req->path, inst);
		model_set(&shadow, obj, "", NULL);
		set_parameter_key(req);
		shadow_save();
		snprintf(body, sizeof(body), "<InstanceNumber>%u</InstanceNumber>"
			 "<Status>0</Status>", inst);
		soap_empty_response(&s.out, req, "AddObjectResponse", body);
	} else if (!strcmp(req->rpc, "DeleteObject")) {
		size_t plen = strlen(req->path);

		capture_write(req);
		if (!plen || req->path[plen - 1] != '.') {
			soap_fault(&s.out, req, 9003, "Invalid arguments");
			goto out;
		}
		if (!model_exists(&view, req->path)) {
			soap_fault(&s.out, req, 9005, "Invalid parameter name");
			goto out;
		}
		model_del(&shadow, req->path);
		if (model_exists(&model, req->path) || model_exists(&mock, req->path))
			add_deleted(req->path);
		set_parameter_key(req);
		shadow_save();
		soap_empty_response(&s.out, req, "DeleteObjectResponse", "<Status>0</Status>");
	} else if (!strcmp(req->rpc, "SetParameterAttributes")) {
		for (i = 0; i < req->n_attrs; i++)
			attr_rule_set(&req->set_attrs[i]);
		soap_empty_response(&s.out, req, "SetParameterAttributesResponse", NULL);
	} else if (!strcmp(req->rpc, "ScheduleInform")) {
		unsigned long long ms = (unsigned long long)req->delay * 1000 + 1;

		snprintf(act.sched_key, sizeof(act.sched_key), "%s", req->command_key);
		uloop_timeout_set(&act.sched_timer, ms > INT_MAX ? INT_MAX : (int)ms);
		soap_empty_response(&s.out, req, "ScheduleInformResponse", NULL);
	} else if (!strcmp(req->rpc, "Reboot")) {
		capture_write(req);
		act.reboot = true;
		snprintf(act.reboot_key, sizeof(act.reboot_key), "%s", req->command_key);
		soap_empty_response(&s.out, req, "RebootResponse", NULL);
	} else if (!strcmp(req->rpc, "FactoryReset")) {
		capture_write(req);
		act.reset = true;
		soap_empty_response(&s.out, req, "FactoryResetResponse", NULL);
	} else if (!strcmp(req->rpc, "Download")) {
		capture_write(req);
		if (act.download || act.tc_pending) {
			soap_fault(&s.out, req, 9004, "Resources exceeded");
			goto out;
		}
		/* only the fixed-size fields are kept; the lists belong to @req */
		act.dl = *req;
		act.dl.names = NULL;
		act.dl.n_names = 0;
		act.dl.set_params = NULL;
		act.dl.n_set = 0;
		act.dl.set_attrs = NULL;
		act.dl.n_attrs = 0;
		act.download = true;
		soap_empty_response(&s.out, req, "DownloadResponse",
				    "<Status>1</Status><StartTime>" UNKNOWN_TIME "</StartTime>"
				    "<CompleteTime>" UNKNOWN_TIME "</CompleteTime>");
	} else if (!strcmp(req->rpc, "GetRPCMethods")) {
		soap_get_rpc_methods(&s.out, req, methods, sizeof(methods) / sizeof(methods[0]));
	} else if (!strcmp(req->rpc, "GetParameterValues")) {
		values = malloc((view.n + 1) * sizeof(*values));
		n = values ? model_values(&view, req->names, req->n_names, values, view.n) : -1;
		if (n < 0)
			soap_fault(&s.out, req, 9005, "Invalid parameter name");
		else
			soap_get_parameter_values(&s.out, req, values, n);
	} else if (!strcmp(req->rpc, "GetParameterAttributes")) {
		values = malloc((view.n + 1) * sizeof(*values));
		n = values ? model_values(&view, req->names, req->n_names, values, view.n) : -1;
		if (n < 0) {
			soap_fault(&s.out, req, 9005, "Invalid parameter name");
		} else {
			attrs = calloc(n ? (size_t)n : 1, sizeof(*attrs));
			if (!attrs) {
				soap_fault(&s.out, req, 9004, "Resources exceeded");
				goto out;
			}
			for (i = 0; i < (unsigned int)n; i++) {
				attrs[i].name = values[i].name;
				attr_rule_lookup(values[i].name, &attrs[i].notification,
						 &attrs[i].access_list);
			}
			soap_get_parameter_attributes(&s.out, req, attrs, (unsigned int)n);
		}
	} else if (!strcmp(req->rpc, "GetParameterNames")) {
		unsigned int max = view.n * 2 + 1;

		names = malloc(max * sizeof(*names));
		writable = malloc(max * sizeof(*writable));
		owned = calloc(max, sizeof(*owned));
		n = names && writable && owned ?
		    model_names(&view, req->path, req->next_level, names, max, owned) : -1;
		if (n == -2) {
			soap_fault(&s.out, req, 9003, "Invalid arguments");
		} else if (n < 0) {
			soap_fault(&s.out, req, 9005, "Invalid parameter name");
		} else {
			for (i = 0; i < (unsigned int)n; i++)
				writable[i] = model_writable(&view, names[i]);
			soap_get_parameter_names(&s.out, req, names, writable, (unsigned int)n);
		}
		for (i = 0; owned && i < max; i++)
			free(owned[i]);
	} else if (!strcmp(req->rpc, "GetQueuedTransfers") ||
		   !strcmp(req->rpc, "GetAllQueuedTransfers")) {
		soap_empty_response(&s.out, req, !strcmp(req->rpc, "GetQueuedTransfers") ?
				    "GetQueuedTransfersResponse" : "GetAllQueuedTransfersResponse",
				    "<TransferList soap-enc:arrayType=\"cwmp:QueuedTransferStruct[0]\">"
				    "</TransferList>");
	} else {
		bool refused = false;

		for (i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
			refused |= !strcmp(req->rpc, denied[i]);
		cwmp_log(LOG_NOTICE, "%s %s from the ACS (%u names)",
			 refused ? "refused" : "unsupported", req->rpc, req->n_names);
		soap_fault(&s.out, req, refused ? 9001 : 9000,
			   refused ? "Request denied" : "Method not supported");
		goto out;
	}
	cwmp_log(LOG_INFO, "answered %s (%u names, %u values)", req->rpc, req->n_names,
		 req->n_set);
out:
	free(values);
	free(names);
	free(writable);
	free(attrs);
	free(owned);
	free(view.p);
}

/* One reply of the ACS has arrived in full. */
static void session_reply(void)
{
	struct soap_request req;
	bool rpc;

	if (s.in.len)
		log_xml("ACS -> CPE", s.in.data, s.in.len);
	if (s.status == 401 && !s.auth_retried) {
		/* uclient now knows the challenge; send the same message again */
		s.auth_retried = true;
		session_send();
		return;
	}
	s.auth_retried = false;
	if (s.status != 200 && s.status != 204) {
		char why[48];

		snprintf(why, sizeof(why), "HTTP %d", s.status);
		session_end(false, why);
		return;
	}

	rpc = soap_parse(s.in.len ? s.in.data : NULL, s.in.len, &req);
	if (!s.informed) {
		if (!rpc || strcmp(req.rpc, "InformResponse")) {
			soap_request_free(&req);
			session_end(false, rpc ? "Inform refused" : "no InformResponse");
			return;
		}
		s.informed = true;
		buf_free(&s.out);		/* an empty POST: the ACS's turn */
		if (act.tc_pending) {
			char id[24];

			snprintf(id, sizeof(id), "%u", ++s.number);
			soap_transfer_complete(&s.out, id, act.tc_key, act.tc_fault,
					       "Download failure", act.tc_start,
					       act.tc_complete);
			s.tc_sent = true;
		}
	} else if (rpc && !strcmp(req.rpc, "TransferCompleteResponse")) {
		act.tc_pending = false;
		cwmp_log(LOG_INFO, "TransferComplete delivered");
		buf_free(&s.out);
	} else if (!rpc) {
		soap_request_free(&req);
		session_end(true, "ok");
		return;
	} else if (req.fault) {
		cwmp_log(LOG_WARNING, "the ACS answered with a fault");
		buf_free(&s.out);
	} else {
		reply_rpc(&req);
	}
	soap_request_free(&req);
	session_send();
}

static void cb_header_done(struct uclient *cl)
{
	struct blob_attr *cur;
	size_t rem;

	s.status = cl->status_code;
	buf_free(&s.in);
	blobmsg_for_each_attr(cur, cl->meta, rem) {
		const char *v = blobmsg_get_string(cur);

		if (!strcasecmp(blobmsg_name(cur), "set-cookie") && v)
			snprintf(s.cookie, sizeof(s.cookie), "%.*s", (int)strcspn(v, ";"), v);
	}
}

static void cb_data_read(struct uclient *cl)
{
	char data[1024];
	int n;

	while ((n = uclient_read(cl, data, sizeof(data))) > 0)
		if (s.in.len + n > 512 * 1024 || !buf_printf(&s.in, "%.*s", n, data))
			break;
}

static void cb_data_eof(struct uclient *cl)
{
	uloop_timeout_cancel(&session_timer);
	session_reply();
}

static void cb_error(struct uclient *cl, int code)
{
	session_end(false, uclient_strerror(code));
}

static const struct uclient_cb uclient_cb = {
	.header_done = cb_header_done,
	.data_read = cb_data_read,
	.data_eof = cb_data_eof,
	.error = cb_error,
};

static void session_timeout(struct uloop_timeout *t)
{
	session_end(false, "timeout");
}

/* POST s.out (empty for the ACS's turn) with the session's cookie. */
static void session_send(void)
{
	if (s.out.len)
		log_xml("CPE -> ACS", s.out.data, s.out.len);
	uclient_http_reset_headers(s.cl);
	uclient_http_set_request_type(s.cl, "POST");
	if (s.out.len) {
		uclient_http_set_header(s.cl, "Content-Type", "text/xml; charset=\"utf-8\"");
		uclient_http_set_header(s.cl, "SOAPAction", "");
	}
	if (*s.cookie)
		uclient_http_set_header(s.cl, "Cookie", s.cookie);
	if (uclient_connect(s.cl)) {
		session_end(false, "cannot connect");
		return;
	}
	if (s.out.len)
		uclient_write(s.cl, s.out.data, s.out.len);
	uclient_request(s.cl);
	session_timer.cb = session_timeout;
	uloop_timeout_set(&session_timer, SESSION_TIMEOUT_MS);
}

static void session_start(struct uloop_timeout *t);

/* TR-069 3.2.1.1: wait m * 2^n seconds, m = 5..10, n capped at 9. */
static unsigned int retry_delay(void)
{
	unsigned int n = s.retries > 10 ? 9 : s.retries - 1;
	unsigned int base = 5 << n;

	return base + rand() % (base + 1);
}

static void queue_event(const char *code, const char *key)
{
	unsigned int i;

	for (i = 0; i < s.n_events; i++)
		if (!strcmp(s.events[i].code, code))
			break;
	if (i == s.n_events) {
		if (s.n_events == MAX_EVENTS)
			return;
		s.n_events++;
	}
	snprintf(s.keys[i], sizeof(s.keys[i]), "%s", key ? key : "");
	s.events[i] = (struct soap_event) { code, s.keys[i] };
}

/* Forget the events the ended session delivered; ones queued since stay. */
static void drop_sent_events(void)
{
	unsigned int i, j = 0;

	for (i = 0; i < s.n_sent; i++)
		if (!strcmp(s.events[i].code, "0 BOOTSTRAP"))
			close(open(BOOTSTRAP_FLAG, O_CREAT | O_WRONLY, 0600));
	if (!s.n_sent)
		return;
	for (i = s.n_sent; i < s.n_events; i++, j++) {
		memcpy(s.keys[j], s.keys[i], sizeof(s.keys[j]));
		s.events[j] = (struct soap_event) { s.events[i].code, s.keys[j] };
	}
	s.n_events = j;
	s.n_sent = 0;
}

/* A pretend reboot: the next Inform comes after a reboot's time, as from a fresh boot. */
static void mock_reboot(const char *key)
{
	queue_event("1 BOOT", "");
	queue_event("M Reboot", key);
	cwmp_log(LOG_NOTICE, "acting out a reboot: next Inform in %u s",
		 cfg.mock_reboot_delay);
	uloop_timeout_set(&start_timer, cfg.mock_reboot_delay * 1000);
}

/*
 * A pretend factory reset: forget what the ACS set and bootstrap again, so the
 * ACS provisions the device from scratch.
 */
static void mock_reset(unsigned int delay)
{
	model_free(&shadow);
	free_deleted();
	free_attrs();
	unlink(SHADOW_FILE);
	act.add_last = 0;
	model_set(&model, DM_PARAM("ManagementServer.ParameterKey"), "", NULL);
	unlink(BOOTSTRAP_FLAG);
	queue_event("0 BOOTSTRAP", "");
	queue_event("1 BOOT", "");
	cwmp_log(LOG_NOTICE, "acting out a factory reset: bootstrap Inform in %u s", delay);
	uloop_timeout_set(&start_timer, delay * 1000 + 1);
}

static void fetch_done(struct uloop_process *p, int ret)
{
	const char *type = act.dl.file_type;
	struct stat st;
	bool ok = WIFEXITED(ret) && !WEXITSTATUS(ret) &&
		  !stat(DOWNLOAD_DIR "/file", &st) && st.st_size > 0;

	act.download = false;
	act.tc_pending = true;
	snprintf(act.tc_key, sizeof(act.tc_key), "%s", act.dl.command_key);
	act.tc_fault = ok ? 0 : 9010;
	snprintf(act.tc_start, sizeof(act.tc_start), "%s", act.dl_start);
	now_iso(act.tc_complete, sizeof(act.tc_complete));
	cwmp_log(ok ? LOG_NOTICE : LOG_WARNING, "download %s: %s", type,
		 ok ? "kept in " DOWNLOAD_DIR ", not applied" : "fetch failed");
	queue_event("7 TRANSFER COMPLETE", "");
	queue_event("M Download", act.dl.command_key);
	/* a firmware image or a configuration file is followed by a reboot */
	if (ok && (!strncmp(type, "1 ", 2) || !strncmp(type, "3 ", 2)))
		mock_reboot("");
	else
		uloop_timeout_set(&start_timer, 1);
}

/* Fetch the Download into RAM, capped, and keep only the latest one. */
static void fetch_start(void)
{
	static const char script[] =
		"set -o pipefail; mkdir -p \"$DIR\" && rm -f \"$DIR\"/* && "
		"uclient-fetch -q -T 60 $INSECURE ${U:+--user=\"$U\"} ${P:+--password=\"$P\"} "
		"-O - \"$URL\" | head -c \"$MAX\" > \"$DIR/file\"";
	char max[24];
	pid_t pid;

	now_iso(act.dl_start, sizeof(act.dl_start));
	if (!cfg.capture_downloads) {
		/* not fetched: reported as done all the same */
		act.download = false;
		act.tc_pending = true;
		snprintf(act.tc_key, sizeof(act.tc_key), "%s", act.dl.command_key);
		act.tc_fault = 0;
		snprintf(act.tc_start, sizeof(act.tc_start), "%s", act.dl_start);
		now_iso(act.tc_complete, sizeof(act.tc_complete));
		queue_event("7 TRANSFER COMPLETE", "");
		queue_event("M Download", act.dl.command_key);
		uloop_timeout_set(&start_timer, 1);
		return;
	}
	pid = fork();
	if (pid < 0) {
		fetch_done(&act.fetch, 1 << 8);
		return;
	}
	if (!pid) {
		snprintf(max, sizeof(max), "%llu", (unsigned long long)cfg.download_max_kib * 1024);
		setenv("DIR", DOWNLOAD_DIR, 1);
		setenv("URL", act.dl.url, 1);
		setenv("U", act.dl.username, 1);
		setenv("P", act.dl.password, 1);
		setenv("MAX", max, 1);
		setenv("INSECURE", cfg.verify_cert ? "" : "--no-check-certificate", 1);
		execl("/bin/sh", "sh", "-c", script, (char *)NULL);
		_exit(127);
	}
	cwmp_log(LOG_NOTICE, "fetching the %s download into " DOWNLOAD_DIR,
		 act.dl.file_type);
	act.fetch.pid = pid;
	act.fetch.cb = fetch_done;
	uloop_process_add(&act.fetch);
}

/* What the ACS asked for in the session that just ended. */
static void run_actions(void)
{
	if (act.reset) {
		act.reset = act.reboot = false;
		mock_reset(cfg.mock_reboot_delay);
	} else if (act.reboot) {
		act.reboot = false;
		mock_reboot(act.reboot_key);
	}
	if (act.download && !act.fetch.pending)
		fetch_start();
}

static void session_end(bool ok, const char *result)
{
	uloop_timeout_cancel(&session_timer);
	if (s.cl)
		uclient_free(s.cl);
	s.cl = NULL;
	s.active = false;
	s.last_end = time(NULL);
	snprintf(s.last_result, sizeof(s.last_result), "%s", result);
	if (s.log) {
		fprintf(s.log, "---- session %s: %s\n", ok ? "ended" : "failed", result);
		fclose(s.log);
		s.log = NULL;
	}
	buf_free(&s.out);
	buf_free(&s.in);
	if (ok) {
		cwmp_log(LOG_INFO, "session with %s ended", s.url);
		drop_sent_events();
		s.retries = 0;
		run_actions();
		if (s.n_events && !start_timer.pending)
			uloop_timeout_set(&start_timer, 1);
		return;
	}
	s.retries++;
	cwmp_log(LOG_WARNING, "session with %s failed: %s, retry %u", s.url, result, s.retries);
	uloop_timeout_set(&start_timer, retry_delay() * 1000);
}

static void session_start(struct uloop_timeout *t)
{
	char now[32], id[24], host[256] = "";
	struct soap_device_id dev;
	time_t tm = time(NULL);
	const char *p, *q;

	if (s.active || !cfg.enabled || !s.n_events)
		return;

	*s.url = *s.user = *s.pass = 0;
	if (*cfg.url) {
		snprintf(s.url, sizeof(s.url), "%s", cfg.url);
		snprintf(s.user, sizeof(s.user), "%s", cfg.username);
		snprintf(s.pass, sizeof(s.pass), "%s", cfg.password);
	} else if (!omci_acs()) {
		snprintf(s.last_result, sizeof(s.last_result), "no ACS URL");
		return;
	}

	/* host part of the URL, for the route */
	p = strstr(s.url, "://");
	p = p ? p + 3 : s.url;
	if (*p == '[') {
		q = strchr(++p, ']');
	} else {
		q = p + strcspn(p, ":/");
	}
	if (q && q > p && (size_t)(q - p) < sizeof(host))
		snprintf(host, sizeof(host), "%.*s", (int)(q - p), p);
	route_to_acs(host);
	update_connection_request_url();

	{
		char auth[520];

		snprintf(auth, sizeof(auth), "%s:%s", s.user, s.pass);
		s.cl = uclient_new(s.url, *s.user ? auth : NULL, &uclient_cb);
	}
	if (!s.cl) {
		session_end(false, "invalid ACS URL");
		return;
	}
	uclient_http_set_address_family(s.cl, cfg.family);
	if (!strncasecmp(s.url, "https:", 6)) {
		if (!ssl_ctx) {
			session_end(false, "no TLS library");
			return;
		}
		uclient_http_set_ssl_ctx(s.cl, ssl_ops, ssl_ctx, cfg.verify_cert);
	}
	uclient_set_timeout(s.cl, SESSION_TIMEOUT_MS);

	s.active = true;
	s.informed = false;
	s.tc_sent = false;
	s.n_sent = s.n_events;
	s.auth_retried = false;
	*s.cookie = 0;
	s.last_start = tm;
	log_open();

	model_set(&model, DM_PARAM("ManagementServer.URL"), s.url, NULL);
	{
		struct sysinfo si;
		char up[24];

		sysinfo(&si);
		snprintf(up, sizeof(up), "%ld", si.uptime);
		model_set(&model, DM_PARAM("DeviceInfo.UpTime"), up, "xsd:unsignedInt");
	}
	dev = (struct soap_device_id) {
		.manufacturer = model_get(&model, DM_PARAM("DeviceInfo.Manufacturer")),
		.oui = model_get(&model, DM_PARAM("DeviceInfo.ManufacturerOUI")),
		.product_class = model_get(&model, DM_PARAM("DeviceInfo.ProductClass")),
		.serial = model_get(&model, DM_PARAM("DeviceInfo.SerialNumber")),
	};
	strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", gmtime(&tm));
	snprintf(id, sizeof(id), "%u", ++s.number);
	buf_free(&s.out);
	soap_inform(&s.out, id, &dev, s.events, s.n_events, s.retries, now,
		    model.p, model.n);
	cwmp_log(LOG_INFO, "Inform to %s: %s%s", s.url, s.events[0].code,
		 s.n_events > 1 ? " and more" : "");
	session_send();
}

void cwmp_session_request(const char *event)
{
	queue_event(event, "");
	/* while a retry is pending, the event goes with it */
	if (!s.active && !s.retries)
		uloop_timeout_set(&start_timer, 1);
}

void cwmp_stun_state(const char *udp_addr, bool nat_detected)
{
	model_set(&model, DM_PARAM("ManagementServer.UDPConnectionRequestAddress"),
		  udp_addr ? udp_addr : "", NULL);
	model_set(&model, DM_PARAM("ManagementServer.NATDetected"),
		  nat_detected ? "1" : "0", "xsd:boolean");
}

static void sched_inform_cb(struct uloop_timeout *t)
{
	queue_event("3 SCHEDULED", "");
	queue_event("M ScheduleInform", act.sched_key);
	if (!s.active && !s.retries)
		uloop_timeout_set(&start_timer, 1);
}

static void periodic_inform(struct uloop_timeout *t)
{
	uloop_timeout_set(t, cfg.periodic_interval * 1000);
	cwmp_session_request("2 PERIODIC");
}

static void periodic_reschedule(void)
{
	uloop_timeout_cancel(&periodic_timer);
	if (cfg.periodic)
		uloop_timeout_set(&periodic_timer, cfg.periodic_interval * 1000);
}

/* ---- ubus: status and a manual Inform ---- */

static struct blob_buf reply;

static int ubus_status(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	blob_buf_init(&reply, 0);
	blobmsg_add_u8(&reply, "enabled", cfg.enabled);
	blobmsg_add_u8(&reply, "active", s.active);
	blobmsg_add_string(&reply, "acs", s.url);
	blobmsg_add_string(&reply, "source", *cfg.url ? "uci" : *s.url ? "omci" : "none");
	blobmsg_add_string(&reply, "last_result", s.last_result);
	blobmsg_add_u64(&reply, "last_start", s.last_start);
	blobmsg_add_u64(&reply, "last_end", s.last_end);
	blobmsg_add_u32(&reply, "retries", s.retries);
	blobmsg_add_string(&reply, "connection_request_url",
			   model_get(&model, DM_PARAM("ManagementServer.ConnectionRequestURL")));
	blobmsg_add_string(&reply, "udp_connection_request_address",
			   model_get(&model, DM_PARAM("ManagementServer.UDPConnectionRequestAddress")));
	return ubus_send_reply(ctx, req, reply.head);
}

static int ubus_inform(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	s.retries = 0;
	cwmp_session_request("6 CONNECTION REQUEST");
	return 0;
}

static int ubus_bootstrap(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	s.retries = 0;
	mock_reset(0);
	return 0;
}

static int ubus_reload(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	config_load();
	crreq_stop();
	if (cfg.cr_enabled)
		crreq_start();
	stun_start();
	periodic_reschedule();
	return 0;
}

static const struct ubus_method cwmp_methods[] = {
	UBUS_METHOD_NOARG("status", ubus_status),
	UBUS_METHOD_NOARG("inform", ubus_inform),
	UBUS_METHOD_NOARG("bootstrap", ubus_bootstrap),
	UBUS_METHOD_NOARG("reload", ubus_reload),
};

static struct ubus_object_type cwmp_type = UBUS_OBJECT_TYPE("cwmp", cwmp_methods);

static struct ubus_object cwmp_object = {
	.name = "cwmp",
	.type = &cwmp_type,
	.methods = cwmp_methods,
	.n_methods = sizeof(cwmp_methods) / sizeof(cwmp_methods[0]),
};

static void tls_init(void)
{
	glob_t g;
	size_t i;

	ssl_ctx = uclient_new_ssl_context(&ssl_ops);
	if (!ssl_ctx)
		return;
	if (!glob("/etc/ssl/certs/*.crt", 0, NULL, &g)) {
		for (i = 0; i < g.gl_pathc; i++)
			ssl_ops->context_add_ca_crt_file(ssl_ctx, g.gl_pathv[i]);
		globfree(&g);
	}
	ssl_ops->context_set_require_validation(ssl_ctx, cfg.verify_cert);
}

int main(int argc, char **argv)
{
	openlog("cwmpd", 0, LOG_DAEMON);
	srand(time(NULL) ^ getpid());
	config_load();
	if (!cfg.enabled) {
		cwmp_log(LOG_INFO, "disabled in /etc/config/cwmp");
		return 0;
	}

	uloop_init();
	ubus = ubus_connect(NULL);
	if (!ubus) {
		cwmp_log(LOG_ERR, "cannot connect to ubus");
		return 1;
	}
	ubus_add_uloop(ubus);
	ubus_add_object(ubus, &cwmp_object);
	tls_init();
	if (cfg.cr_enabled)
		crreq_start();
	stun_start();

	start_timer.cb = session_start;
	periodic_timer.cb = periodic_inform;
	act.sched_timer.cb = sched_inform_cb;
	if (cfg.periodic)
		uloop_timeout_set(&periodic_timer, cfg.periodic_interval * 1000);
	if (access(BOOTSTRAP_FLAG, F_OK))
		cwmp_session_request("0 BOOTSTRAP");
	cwmp_session_request("1 BOOT");

	uloop_run();
	uloop_timeout_cancel(&act.sched_timer);
	stun_stop();
	crreq_stop();
	free_deleted();
	free_attrs();
	ubus_free(ubus);
	uloop_done();
	return 0;
}
