/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CWMPD_SOAP_H
#define CWMPD_SOAP_H

#include <stdbool.h>
#include <stddef.h>

struct buf {
	char *data;
	size_t len, size;
};

void buf_free(struct buf *b);
bool buf_printf(struct buf *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
/* Append @s with &, <, >, " and ' escaped. */
bool buf_xml(struct buf *b, const char *s);

struct soap_kv {
	char *name;
	char *value;
};

struct soap_attr_set {
	char *name;
	bool notif_change;
	unsigned int notification;
	bool access_change;
	char *access_list;		/* comma-separated subscribers */
};

/* One RPC from the ACS, as far as this client reads it. */
struct soap_request {
	char rpc[48];			/* local name of the Body's first element */
	char id[64];			/* cwmp:ID header, echoed in the response */
	char cwmp_ns[40];		/* urn:dslforum-org:cwmp-1-N of the envelope */
	char **names;			/* the <string> list of the request */
	unsigned int n_names;
	struct soap_kv *set_params;	/* SetParameterValues <Name>/<Value> pairs */
	unsigned int n_set;
	struct soap_attr_set *set_attrs;/* SetParameterAttributes entries */
	unsigned int n_attrs;
	char path[256];			/* GetParameterNames ParameterPath or ObjectName */
	char param_key[64];		/* ParameterKey on write RPCs */
	bool has_param_key;
	char command_key[33];		/* CommandKey of Reboot, Download, ScheduleInform */
	/* Download */
	char file_type[64];
	char url[1024];
	char username[256];
	char password[256];
	char target[256];		/* TargetFileName */
	unsigned long file_size;
	unsigned int delay;		/* DelaySeconds */
	bool next_level;
	bool fault;			/* the ACS answered with a SOAP fault */
};

/* Parse one envelope; false if it holds no RPC (an empty body ends a session). */
bool soap_parse(const char *xml, size_t len, struct soap_request *req);
void soap_request_free(struct soap_request *req);

struct soap_param {
	const char *name;
	const char *value;
	const char *type;		/* xsd type, e.g. "xsd:string" */
};

struct soap_attr {
	const char *name;
	unsigned int notification;
	const char *access_list;	/* comma-separated AccessList entries */
};

/* An Inform event and its CommandKey ("M Reboot" and the like carry one). */
struct soap_event {
	const char *code;
	const char *key;
};

struct soap_device_id {
	const char *manufacturer, *oui, *product_class, *serial;
};

/* CWMP 1.0 namespace, which every ACS accepts for the first message. */
#define CWMP_NS_DEFAULT	"urn:dslforum-org:cwmp-1-0"

bool soap_inform(struct buf *b, const char *id, const struct soap_device_id *dev,
		 const struct soap_event *events, unsigned int n_events,
		 unsigned int retry_count, const char *now,
		 const struct soap_param *params, unsigned int n_params);
bool soap_get_rpc_methods(struct buf *b, const struct soap_request *req,
			  const char *const *methods, unsigned int n);
bool soap_get_parameter_values(struct buf *b, const struct soap_request *req,
			       const struct soap_param *params, unsigned int n);
bool soap_get_parameter_names(struct buf *b, const struct soap_request *req,
			      const char *const *names, const bool *writable,
			      unsigned int n);
bool soap_get_parameter_attributes(struct buf *b, const struct soap_request *req,
				   const struct soap_attr *attrs, unsigned int n);
bool soap_empty_response(struct buf *b, const struct soap_request *req,
			 const char *rpc, const char *body);
bool soap_fault(struct buf *b, const struct soap_request *req,
		unsigned int code, const char *text);
/* The CPE's TransferComplete; @fault 0 is success. */
bool soap_transfer_complete(struct buf *b, const char *id, const char *command_key,
			    unsigned int fault, const char *fault_text,
			    const char *start, const char *complete);

#endif
