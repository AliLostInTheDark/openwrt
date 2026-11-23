// SPDX-License-Identifier: GPL-2.0-only
/*
 * collectd xPON optical transceiver DDM telemetry plugin via standard Linux hwmon.
 *
 * Values use the collectd types.db units: temperature in degC, voltage in V,
 * current in A and power in W (hwmon reports mA and uW).
 *
 * Signed-off-by: Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DATA_MAX_NAME_LEN 128

typedef unsigned long long counter_t;
typedef double gauge_t;
typedef int64_t derive_t;
typedef uint64_t absolute_t;
typedef uint64_t cdtime_t;

typedef union value_u {
	counter_t counter;
	gauge_t gauge;
	derive_t derive;
	absolute_t absolute;
} value_t;

typedef struct value_list_s {
	value_t *values;
	size_t values_len;
	cdtime_t time;
	cdtime_t interval;
	char host[DATA_MAX_NAME_LEN];
	char plugin[DATA_MAX_NAME_LEN];
	char plugin_instance[DATA_MAX_NAME_LEN];
	char type[DATA_MAX_NAME_LEN];
	char type_instance[DATA_MAX_NAME_LEN];
	void *meta;
} value_list_t;

int plugin_register_config(const char *name,
			   int (*callback)(const char *key, const char *val),
			   const char **keys, int keys_num);
int plugin_register_read(const char *name, int (*callback)(void));
int plugin_dispatch_values(const value_list_t *vl);

static char *xpon_device;

static const char *config_keys[] = {
	"Device",
};

static int xpon_config(const char *key, const char *value)
{
	if (strcasecmp(key, "Device") == 0) {
		char *tmp = strdup(value);

		if (!tmp)
			return 1;
		free(xpon_device);
		xpon_device = tmp;
	}
	return 0;
}

static void xpon_submit(const char *dev, const char *type,
			const char *type_instance, gauge_t val)
{
	value_t values[] = {
		{ .gauge = val },
	};
	value_list_t vl = {
		.values = values,
		.values_len = 1,
	};

	snprintf(vl.plugin, sizeof(vl.plugin), "xpon");
	snprintf(vl.plugin_instance, sizeof(vl.plugin_instance), "%s", dev);
	snprintf(vl.type, sizeof(vl.type), "%s", type);
	if (type_instance)
		snprintf(vl.type_instance, sizeof(vl.type_instance), "%s", type_instance);

	plugin_dispatch_values(&vl);
}

static bool hwmon_read_long(const char *dir, const char *attr, long *out)
{
	char path[256], buf[64];
	FILE *fh;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	fh = fopen(path, "r");
	if (!fh)
		return false;
	if (!fgets(buf, sizeof(buf), fh)) {
		fclose(fh);
		return false;
	}
	fclose(fh);
	*out = strtol(buf, NULL, 10);
	return true;
}

static int xpon_read_dev(const char *dev)
{
	char hwdir[256], hpath[256];
	struct dirent *ent;
	long val = 0;
	DIR *d;

	snprintf(hwdir, sizeof(hwdir), "/sys/class/net/%s/device/hwmon", dev);
	d = opendir(hwdir);
	if (!d)
		return -1;
	hpath[0] = '\0';
	while ((ent = readdir(d)) != NULL) {
		if (strncmp(ent->d_name, "hwmon", 5) == 0) {
			snprintf(hpath, sizeof(hpath), "%s/%s", hwdir, ent->d_name);
			break;
		}
	}
	closedir(d);
	if (hpath[0] == '\0')
		return -1;

	if (hwmon_read_long(hpath, "temp1_input", &val))
		xpon_submit(dev, "temperature", NULL, (gauge_t)val / 1000.0);
	if (hwmon_read_long(hpath, "in0_input", &val) && val > 0)
		xpon_submit(dev, "voltage", NULL, (gauge_t)val / 1000.0);
	if (hwmon_read_long(hpath, "curr1_input", &val))
		xpon_submit(dev, "current", NULL, (gauge_t)val / 1000.0);
	if (hwmon_read_long(hpath, "power1_input", &val))
		xpon_submit(dev, "power", "tx", (gauge_t)val / 1000000.0);
	if (hwmon_read_long(hpath, "power2_input", &val))
		xpon_submit(dev, "power", "rx", (gauge_t)val / 1000000.0);

	return 0;
}

static int xpon_read(void)
{
	if (xpon_device && xpon_device[0] != '\0')
		return xpon_read_dev(xpon_device);

	return xpon_read_dev("pon0");
}

void module_register(void)
{
	plugin_register_config("xpon", xpon_config, config_keys, 1);
	plugin_register_read("xpon", xpon_read);
}
