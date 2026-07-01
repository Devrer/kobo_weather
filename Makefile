# Build for Kobo (cross-compile via koxtoolchain).
# Override CROSS / MBEDTLS_PREFIX / FBINK_PREFIX as needed.
#
# First-time FBInk build (run once):
#   make fbink

CROSS           ?= arm-kobov4-linux-gnueabihf
CC              := $(CROSS)-gcc
STRIP           := $(CROSS)-strip

MBEDTLS_PREFIX  ?= $(HOME)/install/mbedtls-kobo
FBINK_PREFIX    ?= $(HOME)/install/fbink-kobo

CA_BUNDLE_URL   ?= https://curl.se/ca/cacert.pem
DEPLOY_PATH     ?= /mnt/onboard/.adds/kobo_weather

CFLAGS  := -O2 -Wall -Wextra -std=c11 \
           -ffunction-sections -fdata-sections \
           -D_GNU_SOURCE \
           -MMD -MP \
           -I. -Isrc \
           -I$(MBEDTLS_PREFIX)/include -I$(FBINK_PREFIX)/include
LDFLAGS := -Wl,--gc-sections
LDLIBS  := -L$(MBEDTLS_PREFIX)/lib \
           -Wl,-Bstatic -lmbedtls -lmbedx509 -lmbedcrypto -Wl,-Bdynamic \
           -L$(FBINK_PREFIX)/lib -Wl,-Bstatic -lfbink -Wl,-Bdynamic \
           -lm -lrt -lpthread \
           -Wl,-Bstatic -li2c -Wl,-Bdynamic

SRCS := src/main.c \
        src/launcher.c \
        src/config.c \
        src/fetch_service.c \
        src/geo_locate.c \
        src/cache.c \
        src/weather.c \
        src/i18n.c \
        src/ui.c \
        src/ui_text.c \
        src/ui_calib.c \
        src/ui_icons.c \
        src/ui_widgets.c \
        src/ui_format.c \
        src/ui_refresh.c \
        src/screen_main.c \
        src/screen_hourly.c \
        src/screen_settings.c \
        src/screen_demo.c \
        src/ui_touch.c \
        src/fb.c \
        src/input.c \
        src/sysutil.c \
        src/system_ops.c \
        src/powersave.c \
        src/wifi.c \
        src/wpa_ctrl.c \
        src/widgets/scroll.c \
        http_client.c \
        cJSON.c

OBJS := $(SRCS:.c=.o)
BIN  := kobo_weather

.PHONY: all clean distclean fbink install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
	$(STRIP) $@
	@ls -lh $@

http_client.o: http_client.c http_client.h ca_bundle.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- CA bundle generated at build time ----------------------------------
ca_bundle.pem:
	curl -fsSL $(CA_BUNDLE_URL) -o $@

ca_bundle.h: ca_bundle.pem
	cp $< $<.tmp && printf '\0' >> $<.tmp && \
	  xxd -i $<.tmp > $@ && \
	  sed -i 's/ca_bundle_pem_tmp/ca_bundle_pem/g' $@ && \
	  rm $<.tmp

# ---- Build FBInk with OpenType support (run once) ----------------------
FBINK_SRC ?= libs/FBInk

fbink:
	@if [ ! -d "$(FBINK_SRC)" ]; then \
	    echo "Cloning FBInk..."; \
	    git clone --depth 1 https://github.com/NiLuJe/FBInk $(FBINK_SRC); \
	fi
	$(MAKE) -C $(FBINK_SRC) CROSS_TC=$(CROSS) MINIMAL=1 OPENTYPE=1 IMAGE=1 KOBO=1 staticlib
	mkdir -p $(FBINK_PREFIX)/lib $(FBINK_PREFIX)/include
	cp $(FBINK_SRC)/release/libfbink.a $(FBINK_PREFIX)/lib/
	cp $(FBINK_SRC)/fbink.h $(FBINK_PREFIX)/include/
	@echo "FBInk installed to $(FBINK_PREFIX)/"

# ---- Host-side debug build for http_client ------------------------------
test_httpget: http_client.c ca_bundle.h
	cc -O0 -g -Wall -Wextra -DHTTPC_DEBUG -o $@ http_client.c \
	   -lmbedtls -lmbedx509 -lmbedcrypto

# ---- Deploy over SSH (optional) -----------------------------------------
KOBO_IP ?= 192.168.1.xxx

install: $(BIN)
	ssh root@$(KOBO_IP) "mkdir -p $(DEPLOY_PATH)/fonts"
	scp $(BIN) weather.conf root@$(KOBO_IP):$(DEPLOY_PATH)/
	scp fonts/DejaVuSans.ttf root@$(KOBO_IP):$(DEPLOY_PATH)/fonts/
	[ -f fonts/DejaVuSans-Bold.ttf ] && scp fonts/DejaVuSans-Bold.ttf root@$(KOBO_IP):$(DEPLOY_PATH)/fonts/ || true
	ssh root@$(KOBO_IP) "chmod +x $(DEPLOY_PATH)/$(BIN)"
	@echo "Deployed to $(KOBO_IP):$(DEPLOY_PATH)"

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN) test_httpget

distclean: clean
	rm -f ca_bundle.h ca_bundle.pem

-include $(OBJS:.o=.d)
