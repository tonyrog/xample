
EPX_DIR=$(HOME)/erlang/epx
EPX_REL=$(EPX_DIR)/priv

CC = gcc

OSNAME := $(shell uname -s)
ifeq ($(OSNAME), Linux)
LDFLAGS = -lrt
endif

EPX_LDFLAGS += $(shell $(EPX_REL)/epx-config --libs)
EPX_CFLAGS  += $(shell $(EPX_REL)/epx-config --cflags)

PNG_LDFLAGS  = $(shell libpng-config --ldflags)
PNG_CFLAGS   = $(shell libpng-config --cflags)

CFLAGS += $(EPX_CFLAGS) $(PNG_CFLAGS)
LDFLAGS += $(EPX_LDFLAGS) $(PNG_LDFLAGS)

OBJS = xample_scope.o xample_mem.o

xample_scope: $(OBJS)
	$(CC)  $(LDFLAGS) -g -o $@ $(OBJS) $(LDFLAGS)

xample_mem.o:	../c_src/xample_mem.c
	$(CC) -c $(CFLAGS) -o $@ $<
