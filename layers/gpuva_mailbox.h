/* The mailbox between d3d12_xs and VK_LAYER_REBOX_gpuva_identity: the last 64 KiB of the GPU
 * window, committed by xmem.dll. d3d12_xs asks for the next host-visible allocation of exactly
 * `size` bytes to sit at `address` (title memory it is making GPU-visible); the layer gives it
 * that device address, maps it there, and writes the address to `result`. The layer reads it only
 * with REBOX_GPUVA_MAILBOX=1 (rebox-run sets it). Plain C for both sides. */
#pragma once

#include <stdint.h>

#define GPUVA_MAILBOX_MAGIC 0x41565047584f4252ull /* "RBOXGPVA" */
#define GPUVA_MAILBOX_SIZE 0x10000ull

struct gpuva_mailbox
{
    uint64_t magic;
    volatile uint64_t address; /* 0: no request */
    volatile uint64_t size;
    volatile uint64_t result;  /* the address used, or 0 */
};
