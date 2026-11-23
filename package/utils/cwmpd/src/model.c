// SPDX-License-Identifier: GPL-2.0-only
/* A flat parameter table and the two lookups CWMP needs. */

#include <stdlib.h>
#include <string.h>

#include "model.h"

static bool model_grow(struct model *m)
{
	struct soap_param *p;
	unsigned int cap;

	if (m->n < m->cap)
		return true;
	cap = m->cap ? m->cap * 2 : 32;
	p = realloc(m->p, cap * sizeof(*p));
	if (!p)
		return false;
	m->p = p;
	m->cap = cap;
	return true;
}

void model_append(struct model *m, const char *name, const char *value, const char *type)
{
	struct soap_param *p;

	if (!model_grow(m))
		return;
	p = &m->p[m->n];
	p->name = strdup(name);
	p->value = strdup(value ? value : "");
	p->type = type;
	if (!p->name || !p->value) {
		free((char *)p->name);
		free((char *)p->value);
		return;
	}
	m->n++;
}

void model_set(struct model *m, const char *name, const char *value, const char *type)
{
	unsigned int i;
	char *v;

	for (i = 0; i < m->n; i++)
		if (!strcmp(m->p[i].name, name))
			break;
	if (i == m->n) {
		model_append(m, name, value, type);
		return;
	}
	v = strdup(value ? value : "");
	if (!v)
		return;
	free((char *)m->p[i].value);
	m->p[i].value = v;
	m->p[i].type = type;
}

const char *model_get(const struct model *m, const char *name)
{
	unsigned int i;

	for (i = 0; i < m->n; i++)
		if (!strcmp(m->p[i].name, name))
			return m->p[i].value;
	return NULL;
}

void model_free(struct model *m)
{
	unsigned int i;

	for (i = 0; i < m->n; i++) {
		free((char *)m->p[i].name);
		free((char *)m->p[i].value);
	}
	free(m->p);
	memset(m, 0, sizeof(*m));
}

static bool is_obj(const char *name)
{
	size_t len = strlen(name);

	return len > 0 && name[len - 1] == '.';
}

static bool under(const char *name, const char *path)
{
	return !strncmp(name, path, strlen(path));
}

bool model_exists(const struct model *m, const char *path)
{
	bool obj = !*path || is_obj(path);
	unsigned int i;

	for (i = 0; i < m->n; i++)
		if (obj ? under(m->p[i].name, path) :
			  (!is_obj(m->p[i].name) && !strcmp(m->p[i].name, path)))
			return true;
	return false;
}

static bool ends_with(const char *s, const char *sfx)
{
	size_t slen = strlen(s), xlen = strlen(sfx);

	return slen >= xlen && !strcmp(s + slen - xlen, sfx);
}

static bool digits_segment(const char *s, size_t len)
{
	size_t i;

	if (!len)
		return false;
	for (i = 0; i < len; i++)
		if (s[i] < '0' || s[i] > '9')
			return false;
	return true;
}

bool model_writable(const struct model *m, const char *name)
{
	size_t len = strlen(name);

	if (is_obj(name)) {
		const char *p;
		unsigned int i;

		if (len < 2)
			return false;
		/* An instance object (ends with ".<digits>.") is writable/deletable. */
		p = name + len - 2;
		while (p > name && *p != '.')
			p--;
		if (*p == '.' && digits_segment(p + 1, (size_t)((name + len - 1) - (p + 1))))
			return true;
		/* A table object whose direct children are instance numbers is writable. */
		for (i = 0; i < m->n; i++) {
			const char *c, *dot;

			if (!under(m->p[i].name, name) || strlen(m->p[i].name) <= len)
				continue;
			c = m->p[i].name + len;
			dot = strchr(c, '.');
			if (dot && digits_segment(c, (size_t)(dot - c)))
				return true;
		}
		return false;
	}

	if (ends_with(name, "NumberOfEntries") || strstr(name, ".Stats.") ||
	    ends_with(name, ".SpecVersion") || ends_with(name, ".RootDataModelVersion") ||
	    ends_with(name, "DeviceInfo.Manufacturer") ||
	    ends_with(name, "DeviceInfo.ManufacturerOUI") ||
	    ends_with(name, "DeviceInfo.ProductClass") ||
	    ends_with(name, "DeviceInfo.SerialNumber") ||
	    ends_with(name, "DeviceInfo.HardwareVersion") ||
	    ends_with(name, "DeviceInfo.SoftwareVersion") ||
	    ends_with(name, "DeviceInfo.UpTime") ||
	    ends_with(name, "ManagementServer.ParameterKey") ||
	    ends_with(name, "ManagementServer.ConnectionRequestURL") ||
	    ends_with(name, "ManagementServer.NATDetected") ||
	    ends_with(name, "ManagementServer.UDPConnectionRequestAddress") ||
	    ends_with(name, ".BytesSent") || ends_with(name, ".BytesReceived") ||
	    ends_with(name, ".PacketsSent") || ends_with(name, ".PacketsReceived"))
		return false;
	return true;
}

void model_del(struct model *m, const char *path)
{
	unsigned int i, j = 0;

	for (i = 0; i < m->n; i++) {
		if (under(m->p[i].name, path)) {
			free((char *)m->p[i].name);
			free((char *)m->p[i].value);
			continue;
		}
		m->p[j++] = m->p[i];
	}
	m->n = j;
}

int model_values(const struct model *m, char *const *names, unsigned int n,
		 struct soap_param *out, unsigned int max)
{
	unsigned int i, j, count = 0;

	for (i = 0; i < n; i++) {
		size_t len = strlen(names[i]);
		bool partial = !len || names[i][len - 1] == '.';
		bool found = false;

		for (j = 0; j < m->n; j++) {
			bool obj = is_obj(m->p[j].name);

			if (partial ? !under(m->p[j].name, names[i]) :
				      (obj || strcmp(m->p[j].name, names[i])))
				continue;
			found = true;
			if (!obj && count < max)
				out[count++] = m->p[j];
		}
		if (!found)
			return -1;
	}
	return count;
}

static bool listed(const char **out, unsigned int n, const char *s, size_t len)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (strlen(out[i]) == len && !strncmp(out[i], s, len))
			return true;
	return false;
}

/*
 * Objects are the prefixes of parameter names up to a '.'. With NextLevel
 * only the direct children of @path are named.
 */
int model_names(const struct model *m, const char *path, bool next_level,
		const char **out, unsigned int max, char **owned)
{
	size_t plen = strlen(path);
	unsigned int i, n = 0, o = 0;
	bool matched = false;

	if (plen && path[plen - 1] != '.') {
		if (!model_get(m, path))
			return -1;
		if (next_level)
			return -2;		/* TR-069 A.3.2.3: 9003 for a parameter */
		out[0] = path;
		return 1;
	}
	/* A full listing names the object asked for as well. */
	if (plen && !next_level && n < max)
		out[n++] = path;
	for (i = 0; i < m->n; i++) {
		const char *name = m->p[i].name, *dot;

		if (!under(name, path))
			continue;
		matched = true;
		for (dot = strchr(name + plen, '.'); dot; dot = strchr(dot + 1, '.')) {
			size_t len = dot - name + 1;

			if (!listed(out, n, name, len) && n < max && o < max) {
				owned[o] = strndup(name, len);
				if (owned[o])
					out[n++] = owned[o++];
			}
			if (next_level)
				break;
		}
		if (!is_obj(name) && (!next_level || !strchr(name + plen, '.')) &&
		    n < max && !listed(out, n, name, strlen(name)))
			out[n++] = name;
	}
	if (!matched && plen)
		return -1;
	return n;
}
