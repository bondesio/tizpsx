CC := $(HOME)/tizen-studio/tools/arm-linux-gnueabi-gcc-9.2/bin/arm-tizen-linux-gnueabi-gcc-9.2.0
TIZEN := $(HOME)/tizen-studio/tools/ide/bin/tizen
LIBGCC := $(shell $(CC) -print-libgcc-file-name)
VERSION := 0.3.0
PACKAGE := org.pablo.tizpsx-$(VERSION)-arm.tpk
EXE := tizpsx
TV_LIB_DIR ?= build/tv-libs
TV_LIBS := libcapi-appfw-application.so.0 libcapi-media-audio-io.so.0 libelementary.so.1 libevas.so.1 libecore.so.1 libecore_input.so.1 libeina.so.1 libstorage.so.0.1 libc.so.6
LIBS := $(addprefix $(TV_LIB_DIR)/,$(TV_LIBS))

# Read-only link inputs copied from the TV firmware (not packaged).
$(LIBS):
	mkdir -p $(TV_LIB_DIR)
	ssh q60a-tv-root 'tar -chf - $(addprefix /usr/lib/,$(TV_LIBS)) 2>/dev/null' | tar -xf - -C $(TV_LIB_DIR) --strip-components=2
MATH_LIB ?= build/tv-libs/libm.so.6
CORE_ROOT := ../vendor/pcsx_rearmed
CORE_ARCHIVE := $(CORE_ROOT)/pcsx_rearmed_tizpsx.a
CORE_HEADERS ?= /usr/arm-linux-gnueabihf/include
CORE_GCC_INCLUDE := $(shell $(CC) -print-file-name=include)
CORE_GCC_FIXED := $(dir $(CORE_GCC_INCLUDE))include-fixed
CORE_SYS_CPPFLAGS := -I$(abspath build/core-headers) -I$(CORE_HEADERS) -I$(CORE_GCC_INCLUDE) -I$(CORE_GCC_FIXED)
CORE_CPPFLAGS := $(CORE_SYS_CPPFLAGS) -I$(CORE_ROOT)/include -I$(CORE_ROOT)/deps/libretro-common/include
CORE_CC := $(CC) --sysroot=/ -nostdinc $(CORE_SYS_CPPFLAGS)
CFLAGS := -nostdinc -I include -I build/core-headers -I $(CORE_HEADERS) -I $(CORE_GCC_INCLUDE) -I $(CORE_GCC_FIXED) -std=gnu11 -O2 -fPIE -fno-strict-aliasing -c -Wall -Wno-unused-function -Wno-unused-variable

.PHONY: all clean
all: out/$(PACKAGE)

build/ui.o: ui/psx_main.c ui/video_convert.h perf_overlay.h psx_core.h library.h build/core-headers/gnu/stubs-soft.h $(wildcard include/*.h)
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/library.o: library.c library.h $(wildcard include/*.h)
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/input.o: input.c input.h psx_core.h $(wildcard include/*.h)
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/start.o: start.S
	mkdir -p build
	$(CC) -nostdinc -c $< -o $@

build/core-headers/gnu/stubs-soft.h: $(CORE_HEADERS)/gnu/stubs-hard.h
	mkdir -p build/core-headers/gnu
	cp $< $@

$(CORE_ARCHIVE): $(shell find $(CORE_ROOT) -type f \( -name '*.c' -o -name '*.S' \)) build/core-headers/gnu/stubs-soft.h
	$(MAKE) -C $(CORE_ROOT) -f Makefile.libretro \
	  CC='$(CORE_CC)' CC_AS='$(CORE_CC)' AR=$(HOME)/tizen-studio/tools/arm-linux-gnueabi-gcc-9.2/bin/arm-linux-gnueabi-ar \
	  CPPFLAGS='-nostdinc $(CORE_SYS_CPPFLAGS)' platform=armv7-neon-softfloat \
	  STATIC_LINKING=1 TARGET=pcsx_rearmed_tizpsx.a DYNAREC=ari64 \
	  CFLAGS_LAST=-U_FILE_OFFSET_BITS \
	  USE_ASYNC_CDROM=0 USE_ASYNC_GPU=0 USE_ASYNC_SPU=0 USE_LIBRETRO_VFS=0 \
	  NDRC_THREAD=0 HAVE_PHYSICAL_CDROM=0

build/core_adapter.o: core/pcsx_rearmed_adapter.c psx_core.h $(CORE_ARCHIVE)
	mkdir -p build
	$(CORE_CC) $(CORE_CPPFLAGS) -std=gnu11 -O2 -fPIE -fno-strict-aliasing -c $< -o $@

build/stat_compat.o: core/stat_compat.c
	mkdir -p build
	$(CORE_CC) $(CORE_CPPFLAGS) -std=gnu11 -O2 -fPIE -c $< -o $@

build/tv-libs/libdl.so.2:
	mkdir -p build/tv-libs
	ssh q60a-tv-root 'cat /usr/lib/libdl-2.30.so' > $@

build/tv-libs/libpthread.so.0:
	mkdir -p build/tv-libs
	ssh q60a-tv-root 'cat /usr/lib/libpthread-2.30.so' > $@

build/tv-libs/libm.so.6:
	mkdir -p build/tv-libs
	ssh q60a-tv-root 'cat /usr/lib/libm-2.30.so' > $@

build/$(EXE): build/start.o build/ui.o build/library.o build/input.o build/core_adapter.o build/stat_compat.o $(CORE_ARCHIVE) $(LIBS) $(MATH_LIB) build/tv-libs/libdl.so.2 build/tv-libs/libpthread.so.0
	$(CC) -nostdlib -pie -Wl,-e,_start -Wl,--dynamic-linker=/lib/ld-linux.so.3 -Wl,--gc-sections -Wl,--allow-shlib-undefined -o $@ build/start.o build/ui.o build/library.o build/input.o build/core_adapter.o build/stat_compat.o $(CORE_ARCHIVE) $(LIBS) $(MATH_LIB) build/tv-libs/libdl.so.2 build/tv-libs/libpthread.so.0 $(LIBGCC)

out/$(PACKAGE): build/$(EXE) tizen-manifest.xml shared/res/icon.png
	rm -rf stage
	mkdir -p stage/bin stage/shared/res out
	cp build/$(EXE) stage/bin/$(EXE)
	cp tizen-manifest.xml stage/tizen-manifest.xml
	cp shared/res/icon.png stage/shared/res/icon.png
	rm -f $@
	cd stage && zip -q -r ../$@ bin shared tizen-manifest.xml
	$(TIZEN) package -t tpk -s tvdev -- $@

clean:
	rm -rf build stage out
