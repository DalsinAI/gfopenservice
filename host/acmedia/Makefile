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
SYSVA = $(shell pkg-config --exists libva 2>/dev/null && echo 1)
SYSCFLAGS = $(CFLAGS) $(shell pkg-config --cflags $(FFLIBS)) $(if $(SYSVA),,-DACM_NO_VAAPI_CAPS)
SYSLIBS = $(shell pkg-config --libs $(FFLIBS)) -lpthread
# FFmpeg, dav1d and libopenmpt statically; libc, libstdc++ (libopenmpt), libm,
# libz, libdrm, and libva or libudev from the host
WLIBS   = -Wl,-Bstatic $(shell $(PKG) --libs-only-L $(FFLIBS)) -lavformat -lavcodec -lswresample -lswscale -lavutil \
          -L$(PREFIX)/lib -ldav1d -lopenmpt -Wl,-Bdynamic \
          $(shell $(PKG) --libs-only-l $(FFLIBS) | tr ' ' '\n' | grep -vE '^-l(avformat|avcodec|swresample|swscale|avutil|dav1d|openmpt)$$' | sort -u | tr '\n' ' ') \
          -lstdc++ -lm -lpthread

WSRC = src/acm_worker.c src/acm_sandbox.c src/acm_msg.c src/acm_ffmap.c src/acm_tags.c src/acm_caps.c
BSRC = src/acm_broker.c src/acm_msg.c src/acm_tags.c src/acm_plane.c

# acm_planetest (M4K step 2: worker dma-bufs on a KMS overlay plane) needs the
# host's libdrm headers; skipped where there are none
DRMCF  := $(shell pkg-config --cflags libdrm 2>/dev/null)
DRMLIB := $(shell pkg-config --libs libdrm 2>/dev/null)
PLANETEST := $(if $(DRMLIB),$(OUT)/acm_planetest)

# libacmedia.so, the runtime's backend (M4K step 4), and its test against a stand-in worker: no FFmpeg either
RSRC = src/acm_runtime.c $(BSRC)
RUNTIME = $(OUT)/libacmedia.so $(OUT)/acm_fakeworker $(OUT)/acm_runtime_test

all: $(OUT)/acmedia-worker $(OUT)/acmedia-worker-test $(OUT)/acm_test $(OUT)/acm_plane_test $(PLANETEST) $(RUNTIME)

# what builds and tests without FFmpeg: make runtime OUT=...
runtime: $(OUT)/acm_plane_test $(RUNTIME)

# Host-system FFmpeg, dynamically linked. This is the normal portable worker:
# the decoder libraries stay host packages and are not copied into GFOS, a
# pack or a ROM. The pinned static build remains the Pi hardware validation
# build because it carries the Raspberry Pi request decoder.
system: $(OUT)/acmedia-worker-system $(OUT)/acm_test runtime

$(OUT):
	mkdir -p $@

$(OUT)/acmedia-worker: $(WSRC) src/*.h | $(OUT)
	$(CC) $(WCFLAGS) -o $@ $(WSRC) $(WLIBS)

$(OUT)/acmedia-worker-test: $(WSRC) src/*.h | $(OUT)
	$(CC) $(WCFLAGS) -DACM_TEST_HOOKS -o $@ $(WSRC) $(WLIBS)

$(OUT)/acmedia-worker-system: $(WSRC) src/*.h | $(OUT)
	$(CC) $(SYSCFLAGS) -o $@ $(WSRC) $(SYSLIBS)

$(OUT)/acm_test: tests/acm_test.c $(BSRC) src/*.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/acm_test.c $(BSRC)

# the plane route against a stand-in acscreen: no worker, FFmpeg or display needed
$(OUT)/acm_plane_test: tests/acm_plane_test.c $(BSRC) src/*.h | $(OUT)
	$(CC) $(CFLAGS) -pthread -o $@ tests/acm_plane_test.c $(BSRC)

$(OUT)/acm_planetest: tests/acm_planetest.c $(BSRC) src/*.h | $(OUT)
	$(CC) $(CFLAGS) $(DRMCF) -o $@ tests/acm_planetest.c $(BSRC) $(DRMLIB)

$(OUT)/libacmedia.so: $(RSRC) src/*.h | $(OUT)
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=hidden -pthread -o $@ $(RSRC) -ldl

$(OUT)/acm_fakeworker: tests/acm_fakeworker.c src/acm_msg.c src/*.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/acm_fakeworker.c src/acm_msg.c

$(OUT)/acm_runtime_test: tests/acm_runtime_test.c src/*.h | $(OUT)
	$(CC) $(CFLAGS) -pthread -o $@ tests/acm_runtime_test.c -ldl

clean:
	rm -f $(OUT)/acmedia-worker $(OUT)/acmedia-worker-test $(OUT)/acmedia-worker-system $(OUT)/acm_test $(OUT)/acm_plane_test $(OUT)/acm_planetest $(RUNTIME)

.PHONY: all clean runtime system
