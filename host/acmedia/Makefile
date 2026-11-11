# ACMedia stage a: the worker, its test build and the harness. build.sh builds
# FFmpeg first and calls this with PREFIX (FFmpeg's) and OUT.
# MIT, Copyright (c) 2026 AmiCradle Team.
ARCH   ?= $(shell uname -m)
OUT    ?= build/$(ARCH)
PREFIX ?= $(OUT)/prefix
CC     ?= cc
PKG     = PKG_CONFIG_PATH=$(PREFIX)/lib/pkgconfig pkg-config --static
FFLIBS  = libavformat libavcodec libswresample libswscale libavutil
CFLAGS += -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -std=gnu11
WCFLAGS = $(CFLAGS) $(shell $(PKG) --cflags $(FFLIBS))
# FFmpeg, dav1d and libopenmpt statically; libc, libstdc++ (libopenmpt), libm,
# libz, libdrm, and libva or libudev from the host
WLIBS   = -Wl,-Bstatic $(shell $(PKG) --libs-only-L $(FFLIBS)) -lavformat -lavcodec -lswresample -lswscale -lavutil \
          -L$(PREFIX)/lib -ldav1d -lopenmpt -Wl,-Bdynamic \
          $(shell $(PKG) --libs-only-l $(FFLIBS) | tr ' ' '\n' | grep -vE '^-l(avformat|avcodec|swresample|swscale|avutil|dav1d|openmpt)$$' | sort -u | tr '\n' ' ') \
          -lstdc++ -lm -lpthread

WSRC = src/acm_worker.c src/acm_sandbox.c src/acm_msg.c src/acm_ffmap.c src/acm_tags.c src/acm_caps.c
BSRC = src/acm_broker.c src/acm_msg.c src/acm_tags.c

# acm_planetest (M4K step 2: worker dma-bufs on a KMS overlay plane) needs the
# host's libdrm headers; skipped where there are none
DRMCF  := $(shell pkg-config --cflags libdrm 2>/dev/null)
DRMLIB := $(shell pkg-config --libs libdrm 2>/dev/null)
PLANETEST := $(if $(DRMLIB),$(OUT)/acm_planetest)

all: $(OUT)/acmedia-worker $(OUT)/acmedia-worker-test $(OUT)/acm_test $(PLANETEST)

$(OUT)/acmedia-worker: $(WSRC) src/*.h
	$(CC) $(WCFLAGS) -o $@ $(WSRC) $(WLIBS)

$(OUT)/acmedia-worker-test: $(WSRC) src/*.h
	$(CC) $(WCFLAGS) -DACM_TEST_HOOKS -o $@ $(WSRC) $(WLIBS)

$(OUT)/acm_test: tests/acm_test.c $(BSRC) src/*.h
	$(CC) $(CFLAGS) -o $@ tests/acm_test.c $(BSRC)

$(OUT)/acm_planetest: tests/acm_planetest.c $(BSRC) src/*.h
	$(CC) $(CFLAGS) $(DRMCF) -o $@ tests/acm_planetest.c $(BSRC) $(DRMLIB)

clean:
	rm -f $(OUT)/acmedia-worker $(OUT)/acmedia-worker-test $(OUT)/acm_test $(OUT)/acm_planetest

.PHONY: all clean
