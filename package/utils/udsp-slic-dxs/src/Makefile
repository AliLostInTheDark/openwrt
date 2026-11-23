# SPDX-License-Identifier: BSD-2-Clause

UDSP_SRC ?= /usr/share/udsp/src
CFLAGS += -Wall -Wextra -Wno-unused-parameter -std=gnu11

PLUGIN_OBJS := plugin.o dxs_bus.o dxs_fw.o dxs_bbd.o dxs_line.o dxs_pcm.o

all: udsp-slic-dxs.so dxs-probe

%.o: %.c dxs.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $<

udsp-slic-dxs.so: $(PLUGIN_OBJS)
	$(CC) $(LDFLAGS) -shared -o $@ $^ -lm

dxs-probe: dxs-probe.c $(UDSP_SRC)/host_linux.c $(UDSP_SRC)/plugin_load.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(UDSP_INC) $(LDFLAGS) -o $@ $^ -ldl -lpthread

clean:
	rm -f *.o udsp-slic-dxs.so dxs-probe

.PHONY: all clean
