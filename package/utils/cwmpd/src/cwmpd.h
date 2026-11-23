/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CWMPD_H
#define CWMPD_H

#include <stdbool.h>
#include <sys/socket.h>

struct cwmp_config {
	bool enabled;
	char url[512];
	char username[256];
	char password[256];
	char interface[64];		/* netifd interface the ACS is reached through */
	int family;			/* AF_UNSPEC (dual stack), AF_INET or AF_INET6 */
	bool verify_cert;
	bool periodic;
	unsigned int periodic_interval;
	bool omci_acs;			/* use the ACS the OLT provisions over OMCI */
	bool tr098;			/* use InternetGatewayDevice. instead of Device. */
	unsigned int log_sessions;
	bool capture_downloads;		/* fetch Download files into /tmp, never apply them */
	unsigned int download_max_kib;
	unsigned int mock_reboot_delay;	/* seconds a pretend reboot or reset takes */
	char mock_model[128];		/* a device tree to answer from, see mock_load() */

	bool cr_enabled;		/* connection request listener */
	unsigned int cr_port;
	char cr_path[64];
	char cr_username[64];
	char cr_password[64];

	bool stun_enabled;		/* TR-069 Annex G (TR-111) UDP NAT traversal */
	char stun_server[256];
	unsigned int stun_port;
	char stun_username[256];
	char stun_password[256];
	unsigned int stun_min_keepalive;
	unsigned int stun_max_keepalive;
	bool persist_shadow;		/* keep ACS-set mock values across restarts */
};

extern struct cwmp_config cfg;

/* Ask for a session; @event is a CWMP event code such as "6 CONNECTION REQUEST". */
void cwmp_session_request(const char *event);

int crreq_start(void);
void crreq_stop(void);

void stun_start(void);
void stun_stop(void);
void cwmp_stun_state(const char *udp_addr, bool nat_detected);

void cwmp_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
