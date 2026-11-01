PS5_HOST ?= 192.168.68.112
PS5_PORT ?= 9021
TITLE    := ps5-smbd
ELF      := $(TITLE).elf
VERSION  := 0.3.0

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

SRCS := src/main.c src/fs.c src/rpc.c src/names.c src/wsd.c src/shares.c
HDRS := $(wildcard src/*.h)

CFLAGS := -Wall -Wextra -O2 -std=gnu11 -DSMBD_VERSION='"$(VERSION)"' -Isrc

all: $(ELF)

require-sdk:
	@test -n "$(PS5_PAYLOAD_SDK)" || { echo "PS5_PAYLOAD_SDK is undefined; export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk"; exit 1; }

.deps/ps5/lib/libsmb2.a:
	tools/build-libsmb2.sh ps5

.deps/host/lib/libsmb2.a:
	tools/build-libsmb2.sh host

$(ELF): require-sdk .deps/ps5/lib/libsmb2.a $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -I.deps/ps5/include -o $@ $(SRCS) .deps/ps5/lib/libdcerpc.a .deps/ps5/lib/libsmb2.a -lkernel_sys -lSceSystemService -lpthread

# The same server on this computer, on port 4450, sharing two folders under build/ (for tests).
host: .deps/host/lib/libsmb2.a $(SRCS) $(HDRS)
	mkdir -p build/data build/mnt
	cc $(CFLAGS) -DSMBD_HOST -DSMBD_PORT=4450 -DSMBD_DATA='"$(CURDIR)/build/data"' -DSMBD_MNT='"$(CURDIR)/build/mnt"' -DSMBD_TRACE_FILE='"$(CURDIR)/build/smbd.log"' \
	  -I.deps/host/include -o ps5-smbd-host $(SRCS) \
	  -DSMBD_NBNS_PORT=1370 -DSMBD_MDNS_PORT=53530 -DSMBD_LLMNR_PORT=53550 -DSMBD_WSD_PORT=37020 -DSMBD_WSD_HTTP_PORT=53570 \
	  .deps/host/lib/libdcerpc.a .deps/host/lib/libsmb2.a -lpthread

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

# The same server on the PS3: one share rooted at /, as a package. libsmb2 is built with its PS3 makefile.
PS3DEV ?= /data/sahlberg/PS3/ps3dev
PSL1GHT ?= $(PS3DEV)
PS3_CC := $(PS3DEV)/ppu/bin/ppu-gcc
PS3_TITLE := ps3-smbd
PS3_ELF := $(PS3_TITLE).elf
PS3_PKG := $(PS3_TITLE).pkg
PS3_APPID := SMBD00001
PS3_CONTENTID := UP0001-SMBD00001_00-0000000000000000
PS3_HOSTLIB ?= /data/sahlberg/PS3/lib
PS3_PYTHON ?= python2.7
PS3_LIBSMB2_COMMIT := 565460de50d97c451d2692d9fab48314b47e453a
PS3_SRCS := src/main.c src/fs.c src/rpc.c src/names.c src/wsd.c src/shares.c src/ps3.c
PS3_BUILD := build/ps3

.deps/ps3/lib/libsmb2.a:
	PS3DEV=$(PS3DEV) PSL1GHT=$(PSL1GHT) tools/build-libsmb2.sh ps3

.deps/ps3/lib/libdcerpc.a: .deps/ps3/lib/libsmb2.a
	@test -f $@

$(PS3_ELF): .deps/ps3/lib/libsmb2.a .deps/ps3/lib/libdcerpc.a $(PS3_SRCS) $(HDRS)
	export PS3DEV="$(PS3DEV)" PSL1GHT="$(PSL1GHT)"; \
	export PATH="$(PS3DEV)/bin:$(PS3DEV)/ppu/bin:$$PATH"; \
	$(PS3_CC) -mcpu=cell -mhard-float -Wall -Wextra -O2 -std=gnu11 \
	  -DPS3_PPU_PLATFORM -DSMBD_VERSION='"$(VERSION)"' \
	  -Isrc/ps3 -Isrc -I.deps/ps3/include \
	  -I.deps/build-ps3/libsmb2-$(PS3_LIBSMB2_COMMIT)/include \
	  -I$(PSL1GHT)/ppu/include \
	  -o $@ $(PS3_SRCS) \
	  -L.deps/ps3/lib -ldcerpc -lsmb2 \
	  -lnet -lnetctl -lsysutil -lsysmodule -lio -lsysfs -llv2 -lrt -lm

ps3: $(PS3_PKG)

$(PS3_PKG): $(PS3_ELF)
	export PATH="$(PS3DEV)/bin:$(PS3DEV)/ppu/bin:$$PATH"; \
	export LD_LIBRARY_PATH="$(PS3_HOSTLIB):$$LD_LIBRARY_PATH"; \
	rm -rf $(PS3_BUILD) && mkdir -p $(PS3_BUILD)/pkg/USRDIR && \
	ppu-strip $(PS3_ELF) -o $(PS3_BUILD)/$(PS3_ELF) && \
	sprxlinker $(PS3_BUILD)/$(PS3_ELF) && \
	make_self_npdrm $(PS3_BUILD)/$(PS3_ELF) $(PS3_BUILD)/pkg/USRDIR/EBOOT.BIN $(PS3_CONTENTID) && \
	cp $(PS3DEV)/bin/ICON0.PNG $(PS3_BUILD)/pkg/ICON0.PNG && \
	$(PS3_PYTHON) $(PS3DEV)/bin/sfo.py --title "$(PS3_TITLE)" --appid "$(PS3_APPID)" \
	  -f $(PS3DEV)/bin/sfo.xml $(PS3_BUILD)/pkg/PARAM.SFO && \
	$(PS3_PYTHON) $(PS3DEV)/bin/pkg.py --contentid $(PS3_CONTENTID) $(PS3_BUILD)/pkg/ $(PS3_PKG) && \
	cp $(PS3_PKG) $(PS3_TITLE).gnpdrm.pkg && \
	package_finalize $(PS3_TITLE).gnpdrm.pkg && \
	cp $(PS3_TITLE).gnpdrm.pkg $(PS3_PKG)

clean:
	rm -f $(ELF) ps5-smbd-host $(PS3_ELF) $(PS3_PKG) $(PS3_TITLE).gnpdrm.pkg
	rm -rf $(PS3_BUILD)

.PHONY: all require-sdk host deploy clean ps3
