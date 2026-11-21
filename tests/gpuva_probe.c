/* Can this Vulkan driver give memory the same GPU and CPU address?
 *
 * Console titles treat D3D12_GPU_VIRTUAL_ADDRESS and CPU pointers as one
 * address space (docs/d3d12x.md), so the D3D12.X layer needs identity-mapped
 * memory. This native Linux program tries these ways to get it:
 *
 *   import:      take host memory at a chosen CPU address (VK_EXT_external_memory_host)
 *                and ask for that same device address through capture/replay
 *                (VkMemoryOpaqueCaptureAddressAllocateInfo). RADV 26.x ignores the
 *                request for imported memory.
 *   placed:      allocate device memory, read its device address, then map it at
 *                that CPU address (VK_EXT_map_memory_placed). RADV 26.x allocates
 *                in the upper half (0xffff8...), where the CPU can't map.
 *   replay-low:  allocate ordinary memory at a requested device address equal to
 *                the CPU base, then map it there.
 *   replay-high: as replay-low, but request the base with the upper-half bits
 *                set. Not identity: a fallback where GPU = CPU | 0xffff800000000000.
 *
 * Each strategy then checks that vkCmdFillBuffer writes are visible at the
 * CPU address. Build and run on the machine with the real GPU:
 *
 *   nix develop -c make build/gpuva_probe && build/gpuva_probe [CPU_BASE_HEX]
 *
 * CPU_BASE defaults to 0x200000000000 (32 TiB), well away from Wine's
 * allocations and, hopefully, from the driver's own GPU VA allocations. */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <vulkan/vulkan.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define SIZE (2u << 20)
#define PATTERN 0x5ca71e77u

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static int software;
static VkCommandPool pool;

static struct
{
    VkBool32 bda, capture_replay, host_import, placed;
    VkDeviceSize host_align;
} caps;

static int has_ext(const VkExtensionProperties *exts, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(exts[i].extensionName, name))
            return 1;
    return 0;
}

static int init(void)
{
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkPhysicalDeviceProperties props;
    VkExtensionProperties *exts;
    uint32_t n = 1, next;

    VkPhysicalDevice devs[16];

    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
        return 0;
    n = 16;
    vkEnumeratePhysicalDevices(instance, &n, devs);
    if (!n)
        return 0;
    /* Prefer real hardware: on a software rasteriser device addresses are host
     * pointers, so identity is trivially true and proves nothing. */
    phys = devs[0];
    for (uint32_t i = 0; i < n; i++)
    {
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
        {
            phys = devs[i];
            break;
        }
    }
    vkGetPhysicalDeviceProperties(phys, &props);
    software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
    vkGetPhysicalDeviceProperties2(phys, &props2);
    printf("device: %s (%s %s)\nICD: %s\n%s", props.deviceName, drv.driverName, drv.driverInfo,
           getenv("VK_ICD_FILENAMES") ? getenv("VK_ICD_FILENAMES") : "(system default)",
           software ? "WARNING: software device; results say nothing about real GPUs."
                      " Unset VK_ICD_FILENAMES or check that a GPU driver is installed.\n" : "");

    vkEnumerateDeviceExtensionProperties(phys, NULL, &next, NULL);
    exts = calloc(next, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &next, exts);

    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT };
    VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12 };
    int have_placed = has_ext(exts, next, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
    int have_host = has_ext(exts, next, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (have_placed)
        v12.pNext = &placed;
    vkGetPhysicalDeviceFeatures2(phys, &f2);

    caps.bda = v12.bufferDeviceAddress;
    caps.capture_replay = v12.bufferDeviceAddressCaptureReplay;
    caps.placed = have_placed && placed.memoryMapPlaced;
    caps.host_import = have_host;
    if (have_host)
    {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp };
        vkGetPhysicalDeviceProperties2(phys, &p2);
        caps.host_align = hp.minImportedHostPointerAlignment;
    }
    printf("bufferDeviceAddress %u, captureReplay %u, external_memory_host %u (align %#" PRIx64 "), "
           "memoryMapPlaced %u\n", caps.bda, caps.capture_replay, caps.host_import,
           (uint64_t)caps.host_align, caps.placed);
    if (!caps.bda)
        return 0;

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (queue_family = 0; queue_family < nq; queue_family++)
        if (qf[queue_family].queueFlags & VK_QUEUE_TRANSFER_BIT
            || qf[queue_family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            break;

    const char *enable[3];
    uint32_t nenable = 0;
    if (have_host)
        enable[nenable++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (caps.placed)
    {
        /* Placed maps go through vkMapMemory2, which is core only from 1.4. */
        enable[nenable++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
        enable[nenable++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
    }

    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT want_placed = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
        .memoryMapPlaced = caps.placed,
    };
    VkPhysicalDeviceVulkan12Features want12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = caps.placed ? &want_placed : NULL,
        .bufferDeviceAddress = VK_TRUE,
        .bufferDeviceAddressCaptureReplay = caps.capture_replay,
    };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &want12,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nenable, .ppEnabledExtensionNames = enable };
    if (vkCreateDevice(phys, &dci, NULL, &device) != VK_SUCCESS)
        return 0;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = queue_family };
    vkCreateCommandPool(device, &pci, NULL, &pool);
    return 1;
}

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;

    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkBuffer make_buffer(VkDeviceSize size, uint64_t replay_address)
{
    VkBufferOpaqueCaptureAddressCreateInfo oc = { .sType = VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO,
        .opaqueCaptureAddress = replay_address };
    VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VkBuffer buf;

    if (replay_address)
    {
        ext.pNext = &oc;
        bci.flags = VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
    }
    if (caps.host_import)
        bci.pNext = &ext;
    return vkCreateBuffer(device, &bci, NULL, &buf) == VK_SUCCESS ? buf : VK_NULL_HANDLE;
}

static VkDeviceAddress address_of(VkBuffer buf)
{
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf };
    return vkGetBufferDeviceAddress(device, &ai);
}

/* Fill the buffer on the GPU, then read it back through the CPU pointer. */
static int gpu_fill_visible(VkBuffer buf, const volatile uint32_t *cpu)
{
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    VkCommandBuffer cb;

    vkAllocateCommandBuffers(device, &cai, &cb);
    vkBeginCommandBuffer(cb, &cbi);
    vkCmdFillBuffer(cb, buf, 0, SIZE, PATTERN);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cb);
    return cpu[0] == PATTERN && cpu[SIZE / 4 - 1] == PATTERN;
}

static int try_import(uint64_t base)
{
    VkMemoryHostPointerPropertiesEXT hpp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    PFN_vkGetMemoryHostPointerPropertiesEXT get_hpp;
    VkMemoryRequirements req;
    VkDeviceMemory mem;
    VkBuffer buf;
    void *cpu;
    int ok;

    printf("\n[import] host memory at %#" PRIx64 ", device address requested equal\n", base);
    if (!caps.host_import)
    {
        printf("  skipped: no VK_EXT_external_memory_host\n");
        return 0;
    }
    cpu = mmap((void *)base, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (cpu != (void *)base)
    {
        printf("  skipped: CPU range not free\n");
        return 0;
    }
    get_hpp = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT");
    get_hpp(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, cpu, &hpp);

    /* Without capture/replay we can still see whether the driver happens to
     * pick the CPU address (software rasterisers do). */
    uint64_t replay = caps.capture_replay ? base : 0;
    if (!(buf = make_buffer(SIZE, replay)))
    {
        printf("  FAIL: vkCreateBuffer\n");
        return 0;
    }
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryOpaqueCaptureAddressAllocateInfo oc = { .sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO,
        .opaqueCaptureAddress = replay };
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .pNext = replay ? &oc : NULL,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = cpu };
    VkMemoryAllocateFlagsInfo fl = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &imp,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
               | (replay ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT : 0) };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fl,
        .allocationSize = SIZE, .memoryTypeIndex = find_type(req.memoryTypeBits & hpp.memoryTypeBits, 0) };
    VkResult vr = vkAllocateMemory(device, &mai, NULL, &mem);
    if (vr != VK_SUCCESS)
    {
        printf("  FAIL: vkAllocateMemory %d%s\n", vr,
               vr == VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS ? " (requested device address unavailable)" : "");
        return 0;
    }
    vkBindBufferMemory(device, buf, mem, 0);
    VkDeviceAddress va = address_of(buf);
    ok = gpu_fill_visible(buf, cpu);
    printf("  device address %#" PRIx64 " %s CPU address%s; GPU writes %s at CPU address\n",
           (uint64_t)va, va == base ? "==" : "!=",
           replay && va != base ? " (requested address IGNORED)" : "", ok ? "visible" : "NOT visible");
    vkDestroyBuffer(device, buf, NULL);
    vkFreeMemory(device, mem, NULL);
    munmap(cpu, SIZE);
    return va == base && ok;
}

/* Allocate ordinary host-visible device memory, optionally at a requested
 * device address (capture/replay), and map it at cpu_want (0: at its own
 * device address) with VK_EXT_map_memory_placed. Returns the device address
 * on success with GPU writes visible through the CPU mapping, else 0. */
static uint64_t try_alloc_map(const char *label, uint64_t replay, uint64_t cpu_want)
{
    PFN_vkMapMemory2KHR map2;
    PFN_vkUnmapMemory2KHR unmap2;
    VkMemoryRequirements req;
    VkDeviceMemory mem;
    VkBuffer buf;
    void *cpu = NULL;
    int ok;

    printf("\n[%s] device memory", label);
    if (replay)
        printf(" requested at %#" PRIx64, replay);
    if (cpu_want)
        printf(", mapped at CPU %#" PRIx64 "\n", cpu_want);
    else
        printf(", mapped at its own device address\n");
    if (!caps.placed || (replay && !caps.capture_replay))
    {
        printf("  skipped: needs memoryMapPlaced%s\n", replay ? " and captureReplay" : "");
        return 0;
    }
    caps.host_import = 0; /* plain buffer, no external memory info */
    buf = make_buffer(SIZE, replay);
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryOpaqueCaptureAddressAllocateInfo oc = { .sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO,
        .opaqueCaptureAddress = replay };
    VkMemoryAllocateFlagsInfo fl = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .pNext = replay ? &oc : NULL,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
               | (replay ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT : 0) };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fl,
        .allocationSize = req.size, .memoryTypeIndex = find_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkResult vr = vkAllocateMemory(device, &mai, NULL, &mem);
    if (vr != VK_SUCCESS)
    {
        printf("  FAIL: vkAllocateMemory %d%s\n", vr,
               vr == VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS ? " (requested device address refused)" : "");
        vkDestroyBuffer(device, buf, NULL);
        return 0;
    }
    vkBindBufferMemory(device, buf, mem, 0);
    VkDeviceAddress va = address_of(buf);
    printf("  device address %#" PRIx64 "%s\n", (uint64_t)va,
           replay ? (va == replay ? " (as requested)" : " (request IGNORED)") : "");
    if (!cpu_want)
        cpu_want = va;

    void *probe = mmap((void *)(uintptr_t)cpu_want, req.size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (probe != (void *)(uintptr_t)cpu_want)
    {
        printf("  FAIL: CPU range %#" PRIx64 " unavailable (kernel half or in use)\n", cpu_want);
        if (probe != MAP_FAILED)
            munmap(probe, req.size);
        vkDestroyBuffer(device, buf, NULL);
        vkFreeMemory(device, mem, NULL);
        return 0;
    }
    /* Placed maps replace whatever is mapped there, so keep the reservation. */
    if (!(map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(device, "vkMapMemory2KHR")))
        map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(device, "vkMapMemory2");
    if (!(unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(device, "vkUnmapMemory2KHR")))
        unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(device, "vkUnmapMemory2");
    VkMemoryMapPlacedInfoEXT pi = { .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = probe };
    VkMemoryMapInfoKHR mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi,
        .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = mem, .offset = 0, .size = VK_WHOLE_SIZE };
    vr = map2 && unmap2 ? map2(device, &mi, &cpu) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (vr != VK_SUCCESS)
    {
        printf("  FAIL: vkMapMemory2 placed %d%s\n", vr, map2 && unmap2 ? "" : " (vkMapMemory2 not found)");
        munmap(probe, req.size);
        vkDestroyBuffer(device, buf, NULL);
        vkFreeMemory(device, mem, NULL);
        return 0;
    }
    ok = gpu_fill_visible(buf, cpu);
    printf("  mapped at %p; GPU writes %s at CPU address\n", cpu, ok ? "visible" : "NOT visible");
    VkMemoryUnmapInfoKHR ui = { .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem };
    unmap2(device, &ui);
    vkDestroyBuffer(device, buf, NULL);
    vkFreeMemory(device, mem, NULL);
    return ok && cpu == (void *)(uintptr_t)cpu_want ? va : 0;
}

/* Allocate and map the way vkd3d-proton does (host-visible, device address,
 * plain vkMapMemory). Identity only with VK_LAYER_REBOX_gpuva_identity. */
static int try_plain(void)
{
    VkMemoryRequirements req;
    VkDeviceMemory mem;
    VkBuffer buf;
    void *cpu = NULL;
    int ok;

    printf("\n[plain] host-visible allocation + vkMapMemory, as vkd3d-proton does (needs the layer)\n");
    caps.host_import = 0;
    buf = make_buffer(SIZE, 0);
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryAllocateFlagsInfo fl = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fl,
        .allocationSize = req.size, .memoryTypeIndex = find_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    if (vkAllocateMemory(device, &mai, NULL, &mem) != VK_SUCCESS)
    {
        printf("  FAIL: vkAllocateMemory\n");
        return 0;
    }
    vkBindBufferMemory(device, buf, mem, 0);
    VkDeviceAddress va = address_of(buf);
    if (vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &cpu) != VK_SUCCESS)
    {
        printf("  FAIL: vkMapMemory\n");
        return 0;
    }
    ok = gpu_fill_visible(buf, cpu);
    printf("  device address %#" PRIx64 ", mapped at %p (%s); GPU writes %s\n", (uint64_t)va, cpu,
           (uint64_t)(uintptr_t)cpu == va ? "identity" : "different", ok ? "visible" : "NOT visible");
    vkUnmapMemory(device, mem);
    vkDestroyBuffer(device, buf, NULL);
    vkFreeMemory(device, mem, NULL);
    return ok && (uint64_t)(uintptr_t)cpu == va;
}

/* RADV hands out sign-extended upper-half addresses. If the GPU only ever sees
 * those, a constant offset between GPU and CPU addresses is the fallback. */
#define HIGH_HALF 0xffff800000000000ull

int main(int argc, char **argv)
{
    uint64_t base = argc > 1 ? strtoull(argv[1], NULL, 16) : 0x200000000000ull;
    uint64_t placed, low, high;
    int imp, plain;

    if (!init())
    {
        printf("FAIL: no Vulkan device with bufferDeviceAddress\n");
        return 2;
    }
    imp = try_import(base);
    placed = try_alloc_map("placed", 0, 0);
    low = try_alloc_map("replay-low", base, base);
    high = try_alloc_map("replay-high", HIGH_HALF | base, base);
    plain = try_plain();

    printf("\nimport:      %s\n", imp ? "IDENTITY OK" : "no");
    printf("placed:      %s\n", placed ? "IDENTITY OK" : "no");
    printf("replay-low:  %s\n", low == base ? "IDENTITY OK" : "no");
    printf("replay-high: %s\n", high == (HIGH_HALF | base) ? "OFFSET OK (gpu = cpu | 0xffff800000000000)" : "no");
    printf("plain:       %s\n", plain ? "IDENTITY OK" : "no (expected without the layer)");
    if (software)
        printf("(software device: not meaningful)\n");
    return imp || placed || low == base || plain ? 0 : high ? 3 : 1;
}
