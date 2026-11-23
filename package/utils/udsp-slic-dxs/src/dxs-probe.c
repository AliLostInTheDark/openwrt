// SPDX-License-Identifier: BSD-2-Clause
/*
 * dxs-probe: attach the DUSLIC-XS plug-in through the udsp host and loader,
 * as libudsp does, and optionally run the init of one line.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/spi/spidev.h>

#include "host_linux.h"
#include "plugin_load.h"

static void probe_log(void *ctx, enum udsp_slic_log_level level, const char *msg)
{
	static const char *const tag[] = { "err", "warn", "info", "debug" };

	fprintf(stderr, "%s: %s\n", tag[level], msg);
}

int main(int argc, char **argv)
{
	const char *plugin = "/usr/lib/udsp/udsp-slic-dxs.so";
	struct slic_host_cfg cfg = {
		.spidev = "/dev/spidev1.0",
		.spi_mode = SPI_MODE_3,
		.cs_per_byte = true,
		.log = probe_log,
	};
	char err[SLIC_PLUGIN_ERR_MAX];
	const struct udsp_slic_ops *ops;
	struct slic_plugin *p;
	struct slic_host *h;
	struct udsp_slic *s;
	unsigned int channels;
	int opt, chan = -1, active = 0, ring = 0, ret;
	int tx_gain = 0, rx_gain = 0;
	bool gain = false;

	while ((opt = getopt(argc, argv, "d:p:i:a:r:g:")) != -1) {
		switch (opt) {
		case 'd':
			cfg.spidev = optarg;
			break;
		case 'p':
			plugin = optarg;
			break;
		case 'i':
			chan = atoi(optarg);
			break;
		case 'a':
			active = atoi(optarg);
			break;
		case 'r':
			ring = atoi(optarg);
			break;
		case 'g':
			gain = sscanf(optarg, "%d,%d", &tx_gain, &rx_gain) == 2;
			if (gain)
				break;
			/* fall through */
		default:
			fprintf(stderr,
				"usage: %s [-d spidev] [-p plugin] [-i chan [-g tx,rx] [-r ring_s] [-a watch_s]]\n",
				argv[0]);
			return 1;
		}
	}

	ret = slic_host_open(&cfg, &h);
	if (ret) {
		fprintf(stderr, "%s: %s\n", cfg.spidev, strerror(-ret));
		return 1;
	}

	if (slic_plugin_load(plugin, &p, err, sizeof(err))) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	ops = slic_plugin_ops(p);

	s = ops->attach(slic_host_hooks(h), &channels);
	if (!s) {
		fprintf(stderr, "attach failed\n");
		return 1;
	}
	printf("%s: %u channel(s)\n", ops->part_name ? ops->part_name(s) : ops->name,
	       channels);

	if (chan >= 0) {
		ret = ops->init(s, chan);
		printf("init %d: %s\n", chan, ret ? strerror(-ret) : "ok");
	}

	if (chan >= 0 && !ret && gain) {
		ret = ops->gain_set(s, chan, tx_gain, rx_gain);
		printf("gain tx %.1f dB rx %.1f dB: %s\n", tx_gain / 2.0,
		       rx_gain / 2.0, ret ? strerror(-ret) : "ok");
	}

	if (chan >= 0 && !ret && ring) {
		ret = ops->ring_start(s, chan);
		printf("ring %d s: %s\n", ring, ret ? strerror(-ret) : "ok");
		sleep(ring);
		ops->ring_stop(s, chan);
	}

	if (chan >= 0 && !ret && active) {
		bool offhook, last = false;
		int i;

		ret = ops->linefeed_set(s, chan, UDSP_LF_FWD_ACTIVE);
		printf("active, watching the hook for %d s: %s\n", active,
		       ret ? strerror(-ret) : "ok");
		for (i = 0; !ret && i < active * 20; i++) {
			ret = ops->hook_read(s, chan, &offhook);
			if (!ret && offhook != last) {
				printf("%.2f s: %s\n", i / 20.0,
				       offhook ? "off-hook" : "on-hook");
				last = offhook;
			}
			usleep(50000);
		}
		ops->linefeed_set(s, chan, UDSP_LF_OPEN);
	}

	ops->detach(s);
	slic_plugin_unload(p);
	slic_host_close(h);

	return ret ? 1 : 0;
}
