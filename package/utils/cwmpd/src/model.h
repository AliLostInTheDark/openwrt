/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CWMPD_MODEL_H
#define CWMPD_MODEL_H

#include "soap.h"

/* A flat table of parameters: the device's own, the values an ACS set, or a mock tree. */
struct model {
	struct soap_param *p;
	unsigned int n, cap;
};

void model_set(struct model *m, const char *name, const char *value, const char *type);
/* Add without looking for the name first: for loading a table known to be unique. */
void model_append(struct model *m, const char *name, const char *value, const char *type);
const char *model_get(const struct model *m, const char *name);
void model_free(struct model *m);
/* Remove every parameter under @path (an object name ending in '.'). */
void model_del(struct model *m, const char *path);
bool model_exists(const struct model *m, const char *path);
bool model_writable(const struct model *m, const char *name);

/*
 * Resolve the names of a GetParameterValues: an exact name or a partial path
 * ending in '.'. Returns the number of values written to @out, or -1 for an
 * unknown name (CWMP fault 9005).
 */
int model_values(const struct model *m, char *const *names, unsigned int n,
		 struct soap_param *out, unsigned int max);
/*
 * GetParameterNames: the objects and parameters under @path.
 * Returns -1 for unknown name (9005), -2 for NextLevel=1 on a parameter (9003).
 */
int model_names(const struct model *m, const char *path, bool next_level,
		const char **out, unsigned int max, char **owned);

#endif
