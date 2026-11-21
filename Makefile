# Builds the shim DLLs with mingw. Stubs for exports the title imports but we
# haven't implemented come from tools/scan_imports.py --gen-stubs into $(GEN).

# Not CC: dev shells export CC as the native compiler.
MINGW_CC ?= x86_64-w64-mingw32-gcc
BUILD   ?= build
GEN     ?= generated
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wno-unused-function -std=gnu11 -D__USE_MINGW_ANSI_STDIO=1
LDFLAGS += -shared -static-libgcc -Wl,--enable-stdcall-fixup

SHIMS   := xmem d3d12_xs xaudio2_9
COMMON  := shims/common/rebox_log.c

all: $(SHIMS:%=$(BUILD)/%.dll)

.SECONDEXPANSION:
$(BUILD)/%.dll: $$(wildcard shims/$$*/*.c) $(COMMON) shims/common/rebox_log.h shims/$$*/$$*.def $$(wildcard $(GEN)/$$*_stubs.*)
	@mkdir -p $(BUILD)
	cat shims/$*/$*.def $(wildcard $(GEN)/$*_stubs.def) > $(BUILD)/$*.def
	$(MINGW_CC) $(CFLAGS) -o $@ $(wildcard shims/$*/*.c) $(wildcard $(GEN)/$*_stubs.c) $(COMMON) $(BUILD)/$*.def $(LDFLAGS)

# Smoke test executable, run under Wine by tests/run-smoke.sh.
$(BUILD)/smoke.exe: tests/smoke.c
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

$(BUILD)/librebox_gpuva_identity.so: layers/gpuva_identity.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -fPIC -shared -fvisibility=hidden -o $@ $< -lpthread

$(BUILD)/VK_LAYER_REBOX_gpuva_identity.json: layers/VK_LAYER_REBOX_gpuva_identity.json
	@mkdir -p $(BUILD)
	cp $< $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean layer
