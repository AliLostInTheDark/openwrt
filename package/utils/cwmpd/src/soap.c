// SPDX-License-Identifier: GPL-2.0-only
/*
 * The SOAP envelopes of CWMP (TR-069 Amendment 6, A.3), read and written by
 * hand: the messages are small and fixed in shape, so the reader only finds
 * the RPC, its ID and the parameter names it asks for, and the writer builds
 * the few responses this read-only client sends.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "soap.h"

void buf_free(struct buf *b)
{
	free(b->data);
	memset(b, 0, sizeof(*b));
}

static bool buf_reserve(struct buf *b, size_t more)
{
	size_t size;
	char *data;

	if (b->len + more + 1 <= b->size)
		return true;
	size = b->size ? b->size : 1024;
	while (size < b->len + more + 1)
		size *= 2;
	data = realloc(b->data, size);
	if (!data)
		return false;
	b->data = data;
	b->size = size;
	return true;
}

bool buf_printf(struct buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0 || !buf_reserve(b, n))
		return false;
	va_start(ap, fmt);
	vsnprintf(b->data + b->len, n + 1, fmt, ap);
	va_end(ap);
	b->len += n;
	return true;
}

bool buf_xml(struct buf *b, const char *s)
{
	for (; s && *s; s++) {
		const char *e = NULL;

		switch (*s) {
		case '&': e = "&amp;"; break;
		case '<': e = "&lt;"; break;
		case '>': e = "&gt;"; break;
		case '"': e = "&quot;"; break;
		case '\'': e = "&apos;"; break;
		}
		if (!(e ? buf_printf(b, "%s", e) : buf_printf(b, "%c", *s)))
			return false;
	}
	return true;
}

/* ---- reading ---- */

/* The local name of the tag at @p ("<ns:Name ..."), or NULL for a non-element. */
static const char *tag_name(const char *p, const char *end, size_t *len,
			    bool *closing, bool *empty)
{
	const char *n, *q;

	if (p >= end || *p != '<')
		return NULL;
	p++;
	*closing = p < end && *p == '/';
	if (*closing)
		p++;
	if (p >= end || *p == '?' || *p == '!')
		return NULL;
	for (q = p; q < end && !strchr(" \t\r\n/>", *q); q++)
		;
	n = memchr(p, ':', q - p);
	n = n ? n + 1 : p;
	*len = q - n;
	q = memchr(q, '>', end - q);
	*empty = q && q > p && q[-1] == '/';
	return n;
}

static const char *find_seq(const char *p, const char *end, const char *seq)
{
	size_t slen = strlen(seq);

	for (; p + slen <= end; p++)
		if (*p == *seq && !memcmp(p, seq, slen))
			return p;
	return NULL;
}

static size_t put_utf8(char *o, unsigned long cp)
{
	if (cp < 0x80) {
		*o = cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = 0xc0 | (cp >> 6);
		o[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = 0xe0 | (cp >> 12);
		o[1] = 0x80 | ((cp >> 6) & 0x3f);
		o[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	o[0] = 0xf0 | (cp >> 18);
	o[1] = 0x80 | ((cp >> 12) & 0x3f);
	o[2] = 0x80 | ((cp >> 6) & 0x3f);
	o[3] = 0x80 | (cp & 0x3f);
	return 4;
}

/* Unescaped text of the element whose start tag ends at @p, into @out. */
static char *element_text(const char *p, const char *end)
{
	const char *gt = memchr(p, '>', end - p), *lt;
	char *out, *o;

	if (!gt)
		return NULL;
	gt++;
	if (end - gt >= 9 && !memcmp(gt, "<![CDATA[", 9)) {
		const char *ce = find_seq(gt + 9, end, "]]>");

		if (!ce)
			return NULL;
		out = malloc((ce - (gt + 9)) + 1);
		if (!out)
			return NULL;
		memcpy(out, gt + 9, ce - (gt + 9));
		out[ce - (gt + 9)] = 0;
		return out;
	}
	lt = memchr(gt, '<', end - gt);
	if (!lt)
		return NULL;
	out = o = malloc(lt - gt + 1);
	if (!out)
		return NULL;
	for (p = gt; p < lt; ) {
		static const struct { const char *e; char c; } ent[] = {
			{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
			{ "&quot;", '"' }, { "&apos;", '\'' },
		};
		size_t i;

		if (*p == '&' && p + 3 < lt && p[1] == '#') {
			bool hex = p[2] == 'x' || p[2] == 'X';
			const char *q = p + (hex ? 3 : 2);
			char *semi = NULL;
			unsigned long cp;

			if (q < lt && ((*q >= '0' && *q <= '9') ||
				       (hex && ((*q >= 'a' && *q <= 'f') ||
						(*q >= 'A' && *q <= 'F'))))) {
				cp = strtoul(q, &semi, hex ? 16 : 10);
				if (semi && semi < lt && *semi == ';' && cp > 0 && cp <= 0x10ffff) {
					o += put_utf8(o, cp);
					p = semi + 1;
					continue;
				}
			}
		}
		for (i = 0; i < sizeof(ent) / sizeof(ent[0]); i++)
			if ((size_t)(lt - p) >= strlen(ent[i].e) &&
			    !strncmp(p, ent[i].e, strlen(ent[i].e)))
				break;
		if (i < sizeof(ent) / sizeof(ent[0])) {
			*o++ = ent[i].c;
			p += strlen(ent[i].e);
		} else {
			*o++ = *p++;
		}
	}
	*o = 0;
	return out;
}

static void copy_text(char *dst, size_t size, const char *p, const char *end)
{
	char *t = element_text(p, end);

	snprintf(dst, size, "%s", t ? t : "");
	free(t);
}

static bool tag_is(const char *name, size_t len, const char *want)
{
	return strlen(want) == len && !strncmp(name, want, len);
}

static bool add_name(struct soap_request *req, char *name)
{
	char **p;

	if (!name)
		return false;
	p = realloc(req->names, (req->n_names + 1) * sizeof(*p));
	if (!p) {
		free(name);
		return false;
	}
	req->names = p;
	req->names[req->n_names++] = name;
	return true;
}

static bool add_set_param(struct soap_request *req, char *name, char *val)
{
	struct soap_kv *p;

	p = realloc(req->set_params, (req->n_set + 1) * sizeof(*p));
	if (!p) {
		free(name);
		free(val);
		return false;
	}
	req->set_params = p;
	req->set_params[req->n_set].name = name;
	req->set_params[req->n_set].value = val ? val : strdup("");
	if (!req->set_params[req->n_set].value) {
		free(name);
		return false;
	}
	req->n_set++;
	return true;
}

static struct soap_attr_set *add_set_attr(struct soap_request *req, char *name)
{
	struct soap_attr_set *p;

	p = realloc(req->set_attrs, (req->n_attrs + 1) * sizeof(*p));
	if (!p) {
		free(name);
		return NULL;
	}
	req->set_attrs = p;
	memset(&req->set_attrs[req->n_attrs], 0, sizeof(*p));
	req->set_attrs[req->n_attrs].name = name;
	return &req->set_attrs[req->n_attrs++];
}

static void append_csv(char **dst, const char *item)
{
	size_t dlen, ilen;
	char *n;

	if (!item || !*item)
		return;
	dlen = *dst ? strlen(*dst) : 0;
	ilen = strlen(item);
	n = realloc(*dst, dlen + (dlen ? 1 : 0) + ilen + 1);
	if (!n)
		return;
	if (dlen)
		n[dlen++] = ',';
	memcpy(n + dlen, item, ilen + 1);
	*dst = n;
}

static bool parse_bool(const char *s)
{
	return s && (!strcmp(s, "1") || !strcasecmp(s, "true"));
}

bool soap_parse(const char *xml, size_t len, struct soap_request *req)
{
	const char *p, *end = xml + len, *ns;
	bool in_body = false, in_attr = false, in_acl = false, closing, empty;
	char *pending_name = NULL;
	struct soap_attr_set *cur_attr = NULL;
	size_t n;

	memset(req, 0, sizeof(*req));
	ns = xml ? strstr(xml, "urn:dslforum-org:cwmp-1-") : NULL;
	snprintf(req->cwmp_ns, sizeof(req->cwmp_ns), "%.*s",
		 ns ? 25 : (int)strlen(CWMP_NS_DEFAULT), ns ? ns : CWMP_NS_DEFAULT);
	if (!xml || !len)
		return false;

	for (p = memchr(xml, '<', len); p; p = memchr(p + 1, '<', end - p - 1)) {
		const char *name;

		if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
			const char *ce = find_seq(p + 4, end, "-->");

			if (!ce)
				break;
			p = ce + 2;
			continue;
		}
		if (end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
			const char *ce = find_seq(p + 9, end, "]]>");

			if (!ce)
				break;
			p = ce + 2;
			continue;
		}
		name = tag_name(p, end, &n, &closing, &empty);
		if (!name)
			continue;
		if (closing) {
			if (tag_is(name, n, "AccessList")) {
				in_acl = false;
			} else if (tag_is(name, n, "SetParameterAttributesStruct")) {
				in_attr = false;
				cur_attr = NULL;
			}
			continue;
		}
		if (tag_is(name, n, "ID") && !req->id[0]) {
			copy_text(req->id, sizeof(req->id), p, end);
		} else if (tag_is(name, n, "Body")) {
			in_body = true;
		} else if (in_body && !req->rpc[0]) {
			snprintf(req->rpc, sizeof(req->rpc), "%.*s", (int)n, name);
			req->fault = !strcmp(req->rpc, "Fault");
		} else if (req->rpc[0] && tag_is(name, n, "SetParameterAttributesStruct")) {
			in_attr = !empty;
			cur_attr = NULL;
		} else if (in_attr && tag_is(name, n, "Name")) {
			char *an = empty ? strdup("") : element_text(p, end);

			if (an)
				cur_attr = add_set_attr(req, an);
		} else if (in_attr && cur_attr && tag_is(name, n, "NotificationChange") && !empty) {
			char v[8];

			copy_text(v, sizeof(v), p, end);
			cur_attr->notif_change = parse_bool(v);
		} else if (in_attr && cur_attr && tag_is(name, n, "Notification") && !empty) {
			char v[16];

			copy_text(v, sizeof(v), p, end);
			cur_attr->notification = strtoul(v, NULL, 10);
		} else if (in_attr && cur_attr && tag_is(name, n, "AccessListChange") && !empty) {
			char v[8];

			copy_text(v, sizeof(v), p, end);
			cur_attr->access_change = parse_bool(v);
		} else if (in_attr && cur_attr && tag_is(name, n, "AccessList")) {
			in_acl = !empty;
		} else if (in_acl && cur_attr && tag_is(name, n, "string") && !empty) {
			char *s = element_text(p, end);

			append_csv(&cur_attr->access_list, s);
			free(s);
		} else if (req->rpc[0] && tag_is(name, n, "string") && !empty) {
			add_name(req, element_text(p, end));
		} else if (req->rpc[0] &&
			   (tag_is(name, n, "ParameterPath") || tag_is(name, n, "ObjectName")) &&
			   !empty) {
			copy_text(req->path, sizeof(req->path), p, end);
		} else if (req->rpc[0] && tag_is(name, n, "ParameterKey")) {
			req->has_param_key = true;
			if (empty)
				req->param_key[0] = 0;
			else
				copy_text(req->param_key, sizeof(req->param_key), p, end);
		} else if (req->rpc[0] && tag_is(name, n, "Name")) {
			free(pending_name);
			pending_name = empty ? strdup("") : element_text(p, end);
		} else if (req->rpc[0] && tag_is(name, n, "Value") && pending_name) {
			char *val = empty ? strdup("") : element_text(p, end);

			add_set_param(req, pending_name, val);
			pending_name = NULL;
		} else if (req->rpc[0] && tag_is(name, n, "CommandKey") && !empty) {
			copy_text(req->command_key, sizeof(req->command_key), p, end);
		} else if (req->rpc[0] && tag_is(name, n, "FileType") && !empty) {
			copy_text(req->file_type, sizeof(req->file_type), p, end);
		} else if (req->rpc[0] && tag_is(name, n, "URL") && !empty) {
			copy_text(req->url, sizeof(req->url), p, end);
		} else if (req->rpc[0] && tag_is(name, n, "Username") && !empty) {
			copy_text(req->username, sizeof(req->username), p, end);
		} else if (req->rpc[0] && tag_is(name, n, "Password") && !empty) {
			copy_text(req->password, sizeof(req->password), p, end);
		} else if (req->rpc[0] && tag_is(name, n, "TargetFileName") && !empty) {
			copy_text(req->target, sizeof(req->target), p, end);
		} else if (req->rpc[0] &&
			   (tag_is(name, n, "FileSize") || tag_is(name, n, "DelaySeconds")) &&
			   !empty) {
			char v[24];

			copy_text(v, sizeof(v), p, end);
			if (tag_is(name, n, "FileSize"))
				req->file_size = strtoul(v, NULL, 10);
			else
				req->delay = strtoul(v, NULL, 10);
		} else if (req->rpc[0] && tag_is(name, n, "NextLevel") && !empty) {
			char v[8];

			copy_text(v, sizeof(v), p, end);
			req->next_level = parse_bool(v);
		}
	}
	free(pending_name);
	return req->rpc[0];
}

void soap_request_free(struct soap_request *req)
{
	unsigned int i;

	for (i = 0; i < req->n_names; i++)
		free(req->names[i]);
	free(req->names);
	req->names = NULL;
	for (i = 0; i < req->n_set; i++) {
		free(req->set_params[i].name);
		free(req->set_params[i].value);
	}
	free(req->set_params);
	req->set_params = NULL;
	for (i = 0; i < req->n_attrs; i++) {
		free(req->set_attrs[i].name);
		free(req->set_attrs[i].access_list);
	}
	free(req->set_attrs);
	req->set_attrs = NULL;
	req->n_names = 0;
	req->n_set = 0;
	req->n_attrs = 0;
}

/* ---- writing ---- */

static bool envelope_start(struct buf *b, const char *ns, const char *id)
{
	if (!buf_printf(b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			"<soap-env:Envelope"
			" xmlns:soap-env=\"http://schemas.xmlsoap.org/soap/envelope/\""
			" xmlns:soap-enc=\"http://schemas.xmlsoap.org/soap/encoding/\""
			" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
			" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
			" xmlns:cwmp=\"%s\">\n<soap-env:Header>"
			"<cwmp:ID soap-env:mustUnderstand=\"1\">", ns))
		return false;
	return buf_xml(b, id) &&
	       buf_printf(b, "</cwmp:ID></soap-env:Header>\n<soap-env:Body>\n");
}

static bool envelope_end(struct buf *b)
{
	return buf_printf(b, "</soap-env:Body>\n</soap-env:Envelope>\n");
}

static bool param_value(struct buf *b, const struct soap_param *p)
{
	return buf_printf(b, "<ParameterValueStruct><Name>") && buf_xml(b, p->name) &&
	       buf_printf(b, "</Name><Value xsi:type=\"%s\">",
			  p->type ? p->type : "xsd:string") &&
	       buf_xml(b, p->value) &&
	       buf_printf(b, "</Value></ParameterValueStruct>\n");
}

bool soap_inform(struct buf *b, const char *id, const struct soap_device_id *dev,
		 const struct soap_event *events, unsigned int n_events,
		 unsigned int retry_count, const char *now,
		 const struct soap_param *params, unsigned int n_params)
{
	unsigned int i;

	if (!envelope_start(b, CWMP_NS_DEFAULT, id) ||
	    !buf_printf(b, "<cwmp:Inform>\n<DeviceId><Manufacturer>") ||
	    !buf_xml(b, dev->manufacturer) || !buf_printf(b, "</Manufacturer><OUI>") ||
	    !buf_xml(b, dev->oui) || !buf_printf(b, "</OUI><ProductClass>") ||
	    !buf_xml(b, dev->product_class) || !buf_printf(b, "</ProductClass><SerialNumber>") ||
	    !buf_xml(b, dev->serial) || !buf_printf(b, "</SerialNumber></DeviceId>\n") ||
	    !buf_printf(b, "<Event soap-enc:arrayType=\"cwmp:EventStruct[%u]\">\n", n_events))
		return false;
	for (i = 0; i < n_events; i++)
		if (!buf_printf(b, "<EventStruct><EventCode>") || !buf_xml(b, events[i].code) ||
		    !buf_printf(b, "</EventCode><CommandKey>") ||
		    !buf_xml(b, events[i].key ? events[i].key : "") ||
		    !buf_printf(b, "</CommandKey></EventStruct>\n"))
			return false;
	if (!buf_printf(b, "</Event>\n<MaxEnvelopes>1</MaxEnvelopes>"
			"<CurrentTime>%s</CurrentTime><RetryCount>%u</RetryCount>\n"
			"<ParameterList soap-enc:arrayType=\"cwmp:ParameterValueStruct[%u]\">\n",
			now, retry_count, n_params))
		return false;
	for (i = 0; i < n_params; i++)
		if (!param_value(b, &params[i]))
			return false;
	return buf_printf(b, "</ParameterList>\n</cwmp:Inform>\n") && envelope_end(b);
}

bool soap_get_rpc_methods(struct buf *b, const struct soap_request *req,
			  const char *const *methods, unsigned int n)
{
	unsigned int i;

	if (!envelope_start(b, req->cwmp_ns, req->id) ||
	    !buf_printf(b, "<cwmp:GetRPCMethodsResponse>\n"
			"<MethodList soap-enc:arrayType=\"xsd:string[%u]\">\n", n))
		return false;
	for (i = 0; i < n; i++)
		if (!buf_printf(b, "<string>%s</string>\n", methods[i]))
			return false;
	return buf_printf(b, "</MethodList>\n</cwmp:GetRPCMethodsResponse>\n") &&
	       envelope_end(b);
}

bool soap_get_parameter_values(struct buf *b, const struct soap_request *req,
			       const struct soap_param *params, unsigned int n)
{
	unsigned int i;

	if (!envelope_start(b, req->cwmp_ns, req->id) ||
	    !buf_printf(b, "<cwmp:GetParameterValuesResponse>\n"
			"<ParameterList soap-enc:arrayType=\"cwmp:ParameterValueStruct[%u]\">\n", n))
		return false;
	for (i = 0; i < n; i++)
		if (!param_value(b, &params[i]))
			return false;
	return buf_printf(b, "</ParameterList>\n</cwmp:GetParameterValuesResponse>\n") &&
	       envelope_end(b);
}

bool soap_get_parameter_names(struct buf *b, const struct soap_request *req,
			      const char *const *names, const bool *writable,
			      unsigned int n)
{
	unsigned int i;

	if (!envelope_start(b, req->cwmp_ns, req->id) ||
	    !buf_printf(b, "<cwmp:GetParameterNamesResponse>\n"
			"<ParameterList soap-enc:arrayType=\"cwmp:ParameterInfoStruct[%u]\">\n", n))
		return false;
	for (i = 0; i < n; i++)
		if (!buf_printf(b, "<ParameterInfoStruct><Name>") || !buf_xml(b, names[i]) ||
		    !buf_printf(b, "</Name><Writable>%d</Writable></ParameterInfoStruct>\n",
				writable && writable[i] ? 1 : 0))
			return false;
	return buf_printf(b, "</ParameterList>\n</cwmp:GetParameterNamesResponse>\n") &&
	       envelope_end(b);
}

static unsigned int count_csv(const char *csv)
{
	unsigned int n = 1;
	const char *p;

	if (!csv || !*csv)
		return 0;
	for (p = csv; *p; p++)
		if (*p == ',')
			n++;
	return n;
}

bool soap_get_parameter_attributes(struct buf *b, const struct soap_request *req,
				   const struct soap_attr *attrs, unsigned int n)
{
	unsigned int i;

	if (!envelope_start(b, req->cwmp_ns, req->id) ||
	    !buf_printf(b, "<cwmp:GetParameterAttributesResponse>\n"
			"<ParameterList soap-enc:arrayType=\"cwmp:ParameterAttributeStruct[%u]\">\n",
			n))
		return false;
	for (i = 0; i < n; i++) {
		unsigned int n_acl = count_csv(attrs[i].access_list);
		const char *p = attrs[i].access_list;

		if (!buf_printf(b, "<ParameterAttributeStruct><Name>") ||
		    !buf_xml(b, attrs[i].name) ||
		    !buf_printf(b, "</Name><Notification>%u</Notification>"
				"<AccessList soap-enc:arrayType=\"xsd:string[%u]\">",
				attrs[i].notification, n_acl))
			return false;
		while (p && *p) {
			const char *c = strchr(p, ',');
			size_t len = c ? (size_t)(c - p) : strlen(p);
			char item[128];

			snprintf(item, sizeof(item), "%.*s", (int)len, p);
			if (!buf_printf(b, "<string>") || !buf_xml(b, item) ||
			    !buf_printf(b, "</string>"))
				return false;
			p = c ? c + 1 : NULL;
		}
		if (!buf_printf(b, "</AccessList></ParameterAttributeStruct>\n"))
			return false;
	}
	return buf_printf(b, "</ParameterList>\n</cwmp:GetParameterAttributesResponse>\n") &&
	       envelope_end(b);
}

bool soap_empty_response(struct buf *b, const struct soap_request *req,
			 const char *rpc, const char *body)
{
	return envelope_start(b, req->cwmp_ns, req->id) &&
	       buf_printf(b, "<cwmp:%s>%s</cwmp:%s>\n", rpc, body ? body : "", rpc) &&
	       envelope_end(b);
}

bool soap_fault(struct buf *b, const struct soap_request *req,
		unsigned int code, const char *text)
{
	return envelope_start(b, req->cwmp_ns, req->id) &&
	       buf_printf(b, "<soap-env:Fault><faultcode>Client</faultcode>"
			  "<faultstring>CWMP fault</faultstring><detail><cwmp:Fault>"
			  "<FaultCode>%u</FaultCode><FaultString>", code) &&
	       buf_xml(b, text) &&
	       buf_printf(b, "</FaultString></cwmp:Fault></detail></soap-env:Fault>\n") &&
	       envelope_end(b);
}

bool soap_transfer_complete(struct buf *b, const char *id, const char *command_key,
			    unsigned int fault, const char *fault_text,
			    const char *start, const char *complete)
{
	return envelope_start(b, CWMP_NS_DEFAULT, id) &&
	       buf_printf(b, "<cwmp:TransferComplete><CommandKey>") &&
	       buf_xml(b, command_key) &&
	       buf_printf(b, "</CommandKey><FaultStruct><FaultCode>%u</FaultCode>"
			  "<FaultString>", fault) &&
	       buf_xml(b, fault ? fault_text : "") &&
	       buf_printf(b, "</FaultString></FaultStruct><StartTime>%s</StartTime>"
			  "<CompleteTime>%s</CompleteTime></cwmp:TransferComplete>\n",
			  start, complete) &&
	       envelope_end(b);
}
