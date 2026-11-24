# Builds the shim DLLs with mingw. Stubs for exports the title imports but we
# haven't implemented come from tools/scan_imports.py --gen-stubs into $(GEN).

# Not CC: dev shells export CC as the native compiler.
MINGW_CC ?= x86_64-w64-mingw32-gcc
BUILD   ?= build
GEN     ?= generated
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wno-unused-function -std=gnu11 -D__USE_MINGW_ANSI_STDIO=1
LDFLAGS += -shared -static-libgcc -Wl,--enable-stdcall-fixup

SHIMS   := xmem d3d12_xs xaudio2_9 amd_fidelityfx_loader_xbox pixevt xcrdapi gameinput acphal xapu chad
COMMON  := shims/common/rebox_log.c

# Whole stub DLLs for imports nobody provides (scan_imports.py --gen-stubs).
STUB_DLLS := $(notdir $(wildcard $(GEN)/dlls/*))

all: shims layer $(BUILD)/smoke.exe

# What the flake's shims package builds (it has no tests/).
shims: $(SHIMS:%=$(BUILD)/%.dll) $(STUB_DLLS:%=$(BUILD)/stub/%.dll) $(if $(XODUS_IDL),$(BUILD)/xgameruntime.dll)

# xgameruntime.dll: americas-recomp's Gaming Runtime stand-in (shims/xgameruntime/README.md). Its
# interface table is generated from the Xodus project's IDLs (LGPL-2.1, not in this repository:
# XODUS_IDL, which the flake sets); without them it isn't built and WineGDK's runtime is used.
MINGW_CXX ?= x86_64-w64-mingw32-g++
PYTHON ?= python3
XGR := shims/xgameruntime
XGR_SRC := $(wildcard $(XGR)/runtime/*.cpp) $(wildcard $(XGR)/runtime/gtx/*.cpp)
# test_accessibility reads the Windows caption settings through WinRT headers mingw cannot compile.
XGR_TESTS := $(patsubst $(XGR)/tests/%.cpp,$(BUILD)/xgr/%.exe,$(filter-out %/test_accessibility.cpp,$(wildcard $(XGR)/tests/test_*.cpp)))

$(BUILD)/xgr/table.json: $(XGR)/idl_table.py $(wildcard $(XODUS_IDL)/*.idl)
	@mkdir -p $(BUILD)/xgr
	$(PYTHON) -I $(XGR)/idl_table.py $(XODUS_IDL) $@

$(BUILD)/xgr/gen/xgr_stub.cpp: $(BUILD)/xgr/table.json $(XGR)/gen_stub.py
	$(PYTHON) -I $(XGR)/gen_stub.py $< $(BUILD)/xgr/gen

$(BUILD)/xgameruntime.dll: $(BUILD)/xgr/gen/xgr_stub.cpp $(XGR_SRC) $(wildcard $(XGR)/runtime/*.h) $(wildcard $(XGR)/runtime/gtx/*.h)
	$(MINGW_CXX) -std=c++20 -O2 -shared -DXGR_WITH_RUNTIME -I$(XGR)/runtime -o $@ $< $(XGR_SRC) \
		$(BUILD)/xgr/gen/xgameruntime.def -static -static-libgcc -static-libstdc++ \
		-lshell32 -lole32 -luser32 -ladvapi32 -ldxgi -luuid

# Its tests (tools/xbox/platform/xgr in americas-recomp), run by tests/run-xgr.sh.
xgr-tests: $(BUILD)/xgameruntime.dll $(XGR_TESTS)
$(BUILD)/xgr/%.exe: $(XGR)/tests/%.cpp
	$(MINGW_CXX) -std=c++20 -O1 "-DE_ILLEGAL_METHOD_CALL=((HRESULT)0x8000000EL)" -o $@ $< -static -static-libgcc -static-libstdc++ \
		-ladvapi32 -luser32 -lole32 -lshell32 -lruntimeobject

.PHONY: all shims xgr-tests

# XMA2 decoding in xaudio2_9 needs FFMPEG_XMA2 (the flake's ffmpeg-xma2:
# static mingw libavcodec/libavutil); without it XMA2 plays as silence.
ifneq ($(FFMPEG_XMA2),)
CFLAGS_xaudio2_9 := -DHAVE_FFMPEG_XMA2 -I$(FFMPEG_XMA2)/include
LIBS_xaudio2_9 := -L$(FFMPEG_XMA2)/lib -lavcodec -lswresample -lavutil -lbcrypt
endif
# It asks mmdevapi whether there is an audio device.
LIBS_xaudio2_9 += -lole32
# Opus decoding in xapu needs the same FFmpeg build (its Opus decoder); without it, silence.
ifneq ($(FFMPEG_XMA2),)
CFLAGS_xapu := -DHAVE_FFMPEG_OPUS -I$(FFMPEG_XMA2)/include
LIBS_xapu := -L$(FFMPEG_XMA2)/lib -lavcodec -lswresample -lavutil -lbcrypt
endif

# acphal decodes the title's XMA contexts with the same FFmpeg build; without it, silence of the
# same length.
ifneq ($(FFMPEG_XMA2),)
CFLAGS_acphal := -DHAVE_FFMPEG_XMA2 -I$(FFMPEG_XMA2)/include
LIBS_acphal := -L$(FFMPEG_XMA2)/lib -lavcodec -lswresample -lavutil -lbcrypt
endif

# The DirectStorage shim talks to the BCPack helper over a localhost socket.
LIBS_xcrdapi := -lws2_32

# d3d12_xs detiles console textures with AMD's addrlib (Mesa, MIT): ADDRLIB is Mesa's
# src/amd/addrlib, which the flake sets. Without it detile.cpp builds as stubs.
ifneq ($(ADDRLIB),)
ADDRLIB_SRC := $(shell find $(ADDRLIB)/src -name '*.cpp')
ADDRLIB_OBJ := $(patsubst %.cpp,$(BUILD)/addrlib/%.o,$(notdir $(ADDRLIB_SRC)))
ADDRLIB_FLAGS := -std=c++17 -O2 -mbmi -DADDR_FASTCALL= -DLITTLEENDIAN_CPU -DDEBUG=0 -I$(ADDRLIB)/inc \
	-I$(ADDRLIB)/src -I$(ADDRLIB)/src/core $(patsubst %,-I$(ADDRLIB)/src/chip/%,gfx9 gfx10 gfx11 gfx12 r800) \
	-I$(ADDRLIB)/../common
vpath %.cpp $(sort $(dir $(ADDRLIB_SRC)))
$(BUILD)/addrlib/%.o: %.cpp
	@mkdir -p $(BUILD)/addrlib
	$(MINGW_CXX) $(ADDRLIB_FLAGS) -c $< -o $@
DETILE_FLAGS := -DREBOX_ADDRLIB $(ADDRLIB_FLAGS)
else
ADDRLIB_OBJ :=
DETILE_FLAGS := -std=c++17 -O2
endif
$(BUILD)/detile.o: shims/d3d12_xs/detile.cpp shims/d3d12_xs/detile.h
	@mkdir -p $(BUILD)
	$(MINGW_CXX) $(DETILE_FLAGS) -c $< -o $@
LIBS_d3d12_xs := $(BUILD)/detile.o $(ADDRLIB_OBJ) -static -lstdc++
$(BUILD)/d3d12_xs.dll: $(BUILD)/detile.o $(ADDRLIB_OBJ)

# helpers/textures/texcheck.py's detiler: d3d12_xs's detile.cpp and addrlib in a program.
$(BUILD)/texcheck.exe: tools/texcheck.c $(BUILD)/detile.o $(ADDRLIB_OBJ)
	$(MINGW_CC) $(CFLAGS) -o $@ $< $(BUILD)/detile.o $(ADDRLIB_OBJ) -static -lstdc++

.SECONDEXPANSION:
$(BUILD)/%.dll: $$(wildcard shims/$$*/*.c) $$(wildcard shims/$$*/*.h) anyps5/rebox_anyps5.h $(COMMON) shims/common/rebox_log.h shims/$$*/$$*.def $$(wildcard $(GEN)/$$*_stubs.*)
	@mkdir -p $(BUILD)
	cat shims/$*/$*.def $(wildcard $(GEN)/$*_stubs.def) > $(BUILD)/$*.def
	$(MINGW_CC) $(CFLAGS) $(CFLAGS_$*) -o $@ $(wildcard shims/$*/*.c) $(wildcard $(GEN)/$*_stubs.c) $(COMMON) $(BUILD)/$*.def $(LDFLAGS) $(LIBS_$*)

$(BUILD)/stub/%.dll: $(GEN)/dlls/$$*/stubs.c $(GEN)/dlls/$$*/$$*.def $(COMMON) shims/common/rebox_log.h
	@mkdir -p $(BUILD)/stub
	$(MINGW_CC) $(CFLAGS) -Ishims/common -o $@ $(GEN)/dlls/$*/stubs.c $(COMMON) $(GEN)/dlls/$*/$*.def $(LDFLAGS)

# Smoke test executable, run under Wine by tests/run-smoke.sh.
$(BUILD)/smoke.exe: tests/smoke.c $(GEN)/test_shaders.h anyps5/rebox_anyps5.h shims/d3d12_xs/console_compile.h
	@mkdir -p $(BUILD)
	$(MINGW_CC) $(CFLAGS) -o $@ $<

# Native Linux probe: can the Vulkan driver map memory at the same GPU and CPU
# address? Run it on the target GPU (see the comment at its top).
$(BUILD)/gpuva_probe: tests/gpuva_probe.c
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -o $@ $< -lvulkan

# Native Vulkan layer giving host-visible memory identical GPU and CPU
# addresses. Enable with VK_ADD_LAYER_PATH=$(BUILD) and
# VK_INSTANCE_LAYERS=VK_LAYER_REBOX_gpuva_identity.
layer: $(BUILD)/librebox_gpuva_identity.so $(BUILD)/VK_LAYER_REBOX_gpuva_identity.json

$(BUILD)/librebox_gpuva_identity.so: layers/gpuva_identity.c layers/gpuva_mailbox.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -fPIC -shared -fvisibility=hidden -o $@ $< -lpthread

$(BUILD)/VK_LAYER_REBOX_gpuva_identity.json: layers/VK_LAYER_REBOX_gpuva_identity.json
	@mkdir -p $(BUILD)
	cp $< $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean layer
