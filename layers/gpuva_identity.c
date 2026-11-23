/* VK_LAYER_REBOX_gpuva_identity: give host-visible memory the same GPU and
 * CPU address.
 *
 * Console titles treat D3D12_GPU_VIRTUAL_ADDRESS and CPU pointers as one
 * address space (docs/d3d12x.md). This host-side (Linux) layer sits under
 * winevulkan and vkd3d-proton. For every host-visible allocation it picks an
 * address A in a reserved window, allocates with a capture/replay device
 * address of A, and maps it at CPU address A (VK_EXT_map_memory_placed).
 * Buffers bound at offset X then have device address A + X and CPU address
 * A + X, so every CPU-visible D3D12 heap, including vkd3d-proton's
 * suballocations, is identity-mapped without changing vkd3d-proton.
 *
 * Needs a driver that accepts lower-half replay addresses: stock RADV refuses
 * them, .#radv-rebox (patches/mesa-radv-low-va-replay.patch) accepts them.
 * If the driver refuses, the layer logs once and passes allocations through.
 *
 * The window comes from REBOX_GPUVA_WINDOW=BASE:SIZE (hex), default
 * 0x200000000000:0x10000000000 (1 TiB at 32 TiB). xmem.dll reserves the same
 * range inside Wine so Wine never hands it out. */

#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#include "gpuva_mailbox.h"

#define LAYER_NAME "VK_LAYER_REBOX_gpuva_identity"
#define EXPORT __attribute__((visibility("default")))

static void layer_log(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "[rebox gpuva] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ window */

struct range { uint64_t start, end; };

static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static struct range *free_ranges;
static size_t free_count, free_cap;
static uint64_t window_base, window_size;
/* Next-fit: addresses are handed out moving forward and freed ranges are
 * only reused after wrapping around. RADV releases a freed allocation's GPU
 * VA range lazily, so an address reused straight away gets refused. */
static uint64_t window_cursor;

static void window_init_locked(void)
{
    const char *env = getenv("REBOX_GPUVA_WINDOW");
    void *p;

    if (window_size)
        return;
    window_base = 0x200000000000ull;
    window_size = 0x10000000000ull;
    if (env && *env)
    {
        char *end;
        uint64_t base = strtoull(env, &end, 16), size = window_size;
        if (*end == ':')
            size = strtoull(end + 1, NULL, 16);
        if (base && size)
            window_base = base, window_size = size;
    }
    free_cap = 64;
    free_ranges = calloc(free_cap, sizeof(*free_ranges));
    /* The last 64 KiB is the mailbox (gpuva_mailbox.h), never handed out. */
    free_ranges[0] = (struct range){ window_base, window_base + window_size - GPUVA_MAILBOX_SIZE };
    free_count = 1;
    window_cursor = window_base;

    /* Under Wine, xmem.dll has normally reserved this already and the call
     * fails with EEXIST; natively, keep other mappings out of it. */
    p = mmap((void *)window_base, window_size, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    layer_log("window %#" PRIx64 "+%#" PRIx64 "%s", window_base, window_size,
              p == (void *)window_base ? " (reserved here)" : " (already reserved)");
    if (p != MAP_FAILED && p != (void *)window_base)
        munmap(p, window_size);
}

static uint64_t window_alloc(uint64_t size, uint64_t align)
{
    uint64_t addr = 0;

    pthread_mutex_lock(&window_lock);
    window_init_locked();
    for (int pass = 0; pass < 2 && !addr; pass++)
    for (size_t i = 0; i < free_count; i++)
    {
        uint64_t from = free_ranges[i].start;
        uint64_t start;

        if (pass == 0)
        {
            if (free_ranges[i].end <= window_cursor)
                continue;
            if (from < window_cursor)
                from = window_cursor;
        }
        start = (from + align - 1) & ~(align - 1);
        if (start + size > free_ranges[i].end)
            continue;
        addr = start;
        window_cursor = start + size;
        if (start > free_ranges[i].start && start + size < free_ranges[i].end)
        {
            /* Split: keep the head, insert the tail after it. */
            if (free_count == free_cap)
                free_ranges = realloc(free_ranges, (free_cap *= 2) * sizeof(*free_ranges));
            memmove(&free_ranges[i + 2], &free_ranges[i + 1], (free_count - i - 1) * sizeof(*free_ranges));
            free_ranges[i + 1] = (struct range){ start + size, free_ranges[i].end };
            free_ranges[i].end = start;
            free_count++;
        }
        else if (start > free_ranges[i].start)
            free_ranges[i].end = start;
        else if (start + size < free_ranges[i].end)
            free_ranges[i].start = start + size;
        else
        {
            memmove(&free_ranges[i], &free_ranges[i + 1], (free_count - i - 1) * sizeof(*free_ranges));
            free_count--;
        }
        break;
    }
    pthread_mutex_unlock(&window_lock);
    return addr;
}

/* Move the cursor past a region the driver still considers in use. */
static void window_skip(uint64_t bytes)
{
    pthread_mutex_lock(&window_lock);
    window_cursor += bytes;
    if (window_cursor >= window_base + window_size)
        window_cursor = window_base;
    pthread_mutex_unlock(&window_lock);
}

static void window_free(uint64_t start, uint64_t size)
{
    uint64_t end = start + size;
    size_t i;

    pthread_mutex_lock(&window_lock);
    for (i = 0; i < free_count && free_ranges[i].start < start; i++)
        ;
    if (i > 0 && free_ranges[i - 1].end == start)
    {
        free_ranges[i - 1].end = end;
        if (i < free_count && free_ranges[i].start == end)
        {
            free_ranges[i - 1].end = free_ranges[i].end;
            memmove(&free_ranges[i], &free_ranges[i + 1], (free_count - i - 1) * sizeof(*free_ranges));
            free_count--;
        }
    }
    else if (i < free_count && free_ranges[i].start == end)
        free_ranges[i].start = start;
    else
    {
        if (free_count == free_cap)
            free_ranges = realloc(free_ranges, (free_cap *= 2) * sizeof(*free_ranges));
        memmove(&free_ranges[i + 1], &free_ranges[i], (free_count - i) * sizeof(*free_ranges));
        free_ranges[i] = (struct range){ start, end };
        free_count++;
    }
    pthread_mutex_unlock(&window_lock);
}

/* --------------------------------------------------------------- dispatch */

static void *dispatch_key(const void *object)
{
    return *(void **)object;
}

struct instance_data
{
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
    PFN_vkGetPhysicalDeviceFeatures2 features2;
    PFN_vkGetPhysicalDeviceProperties2 properties2;
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties;
    PFN_vkEnumerateDeviceExtensionProperties enum_extensions;
    struct instance_data *next;
};

struct memory
{
    VkDeviceMemory handle;
    uint64_t address, size, reserved; /* reserved = size + guard, in the window */
    int mapped;
    int adopted; /* at title memory outside the window (the mailbox's request) */
    struct memory *next;
};

#define MEMORY_BUCKETS 1024

struct device_data
{
    void *key;
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
    PFN_vkAllocateMemory allocate;
    PFN_vkFreeMemory free;
    PFN_vkMapMemory map;
    PFN_vkUnmapMemory unmap;
    PFN_vkMapMemory2KHR map2;
    PFN_vkUnmapMemory2KHR unmap2;
    uint32_t host_visible_types;
    int identity, unmap_reserve;
    /* Per memory type: identity allocations that worked, and types the
     * driver never places at a requested lower-half address (RADV keeps
     * 32-bit-addressable memory, e.g. for descriptor buffers, in its own
     * range). */
    uint32_t types_ok, types_refused;
    pthread_mutex_t lock;
    struct memory *memories[MEMORY_BUCKETS];
    struct device_data *next;
};

static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static struct instance_data *instances;
static struct device_data *devices;

static struct instance_data *get_instance(const void *object)
{
    void *key = dispatch_key(object);
    struct instance_data *i;

    pthread_mutex_lock(&global_lock);
    for (i = instances; i && i->key != key; i = i->next)
        ;
    pthread_mutex_unlock(&global_lock);
    return i;
}

static struct device_data *get_device(const void *object)
{
    void *key = dispatch_key(object);
    struct device_data *d;

    pthread_mutex_lock(&global_lock);
    for (d = devices; d && d->key != key; d = d->next)
        ;
    pthread_mutex_unlock(&global_lock);
    return d;
}

static unsigned bucket(VkDeviceMemory m)
{
    uint64_t h = (uint64_t)(uintptr_t)m;
    return (unsigned)((h ^ (h >> 17) ^ (h >> 31)) % MEMORY_BUCKETS);
}

/* Caller holds d->lock. */
static struct memory **find_memory(struct device_data *d, VkDeviceMemory m)
{
    struct memory **p = &d->memories[bucket(m)];
    while (*p && (*p)->handle != m)
        p = &(*p)->next;
    return p;
}

/* --------------------------------------------------------------- instance */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkInstance *instance)
{
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)info->pNext;
    PFN_vkGetInstanceProcAddr gipa;
    struct instance_data *data;
    PFN_vkCreateInstance create;
    VkResult vr;

    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                      && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    if (!chain)
        return VK_ERROR_INITIALIZATION_FAILED;
    gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if ((vr = create(info, alloc, instance)) != VK_SUCCESS)
        return vr;
    layer_log("loaded (instance %p)", (void *)*instance);

    data = calloc(1, sizeof(*data));
    data->key = dispatch_key(*instance);
    data->instance = *instance;
    data->gipa = gipa;
#define GET(field, name) data->field = (void *)gipa(*instance, name)
    GET(destroy, "vkDestroyInstance");
    GET(features2, "vkGetPhysicalDeviceFeatures2");
    if (!data->features2)
        GET(features2, "vkGetPhysicalDeviceFeatures2KHR");
    GET(properties2, "vkGetPhysicalDeviceProperties2");
    if (!data->properties2)
        GET(properties2, "vkGetPhysicalDeviceProperties2KHR");
    GET(memory_properties, "vkGetPhysicalDeviceMemoryProperties");
    GET(enum_extensions, "vkEnumerateDeviceExtensionProperties");
#undef GET
    pthread_mutex_lock(&global_lock);
    data->next = instances;
    instances = data;
    pthread_mutex_unlock(&global_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    struct instance_data *data = get_instance(instance), **p;

    if (!data)
        return;
    data->destroy(instance, alloc);
    pthread_mutex_lock(&global_lock);
    for (p = &instances; *p && *p != data; p = &(*p)->next)
        ;
    if (*p)
        *p = data->next;
    pthread_mutex_unlock(&global_lock);
    free(data);
}

/* ----------------------------------------------------------------- device */

static int has_extension(struct instance_data *inst, VkPhysicalDevice phys, const char *name)
{
    uint32_t n = 0;
    VkExtensionProperties *exts;
    int found = 0;

    inst->enum_extensions(phys, NULL, &n, NULL);
    exts = calloc(n ? n : 1, sizeof(*exts));
    inst->enum_extensions(phys, NULL, &n, exts);
    for (uint32_t i = 0; i < n && !found; i++)
        found = !strcmp(exts[i].extensionName, name);
    free(exts);
    return found;
}

static VkBaseOutStructure *find_struct(const void *chain, VkStructureType type)
{
    VkBaseOutStructure *s = (VkBaseOutStructure *)chain;
    while (s && s->sType != type)
        s = s->pNext;
    return s;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkDevice *device)
{
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)info->pNext;
    struct instance_data *inst = get_instance(phys);
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkCreateDevice create;
    struct device_data *data;
    VkDeviceCreateInfo ci = *info;
    VkResult vr;
    int identity = 0, unmap_reserve = 0;

    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                      && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    if (!chain || !inst)
        return VK_ERROR_INITIALIZATION_FAILED;
    gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    create = (PFN_vkCreateDevice)gipa(inst->instance, "vkCreateDevice");

    /* What does the driver support? */
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed_caps = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT };
    VkPhysicalDeviceBufferDeviceAddressFeatures bda_caps = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &bda_caps };
    int have_placed = has_extension(inst, phys, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
    int have_map2 = has_extension(inst, phys, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
    if (have_placed)
        bda_caps.pNext = &placed_caps;
    inst->features2(phys, &f2);
    identity = have_placed && have_map2 && placed_caps.memoryMapPlaced && bda_caps.bufferDeviceAddress
               && bda_caps.bufferDeviceAddressCaptureReplay;
    unmap_reserve = identity && placed_caps.memoryUnmapReserve;

    /* Enable what we need on top of what the application asked for. The
     * structs we add live on this stack frame for the duration of the call. */
    const char **exts = NULL;
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT };
    VkPhysicalDeviceBufferDeviceAddressFeatures bda_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    if (identity)
    {
        VkPhysicalDeviceVulkan12Features *v12 = (void *)find_struct(info->pNext,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        VkPhysicalDeviceBufferDeviceAddressFeatures *bda = (void *)find_struct(info->pNext,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *placed = (void *)find_struct(info->pNext,
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT);
        uint32_t n = 0;

        if (v12)
            v12->bufferDeviceAddress = v12->bufferDeviceAddressCaptureReplay = VK_TRUE;
        else if (bda)
            bda->bufferDeviceAddress = bda->bufferDeviceAddressCaptureReplay = VK_TRUE;
        else
        {
            bda_enable.bufferDeviceAddress = bda_enable.bufferDeviceAddressCaptureReplay = VK_TRUE;
            bda_enable.pNext = (void *)ci.pNext;
            ci.pNext = &bda_enable;
        }
        if (placed)
        {
            placed->memoryMapPlaced = VK_TRUE;
            placed->memoryUnmapReserve = unmap_reserve;
        }
        else
        {
            placed_enable.memoryMapPlaced = VK_TRUE;
            placed_enable.memoryUnmapReserve = unmap_reserve;
            placed_enable.pNext = (void *)ci.pNext;
            ci.pNext = &placed_enable;
        }

        exts = calloc(info->enabledExtensionCount + 2, sizeof(*exts));
        for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
            exts[n++] = info->ppEnabledExtensionNames[i];
        const char *want[] = { VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME };
        for (int w = 0; w < 2; w++)
        {
            int listed = 0;
            for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
                listed |= !strcmp(info->ppEnabledExtensionNames[i], want[w]);
            if (!listed)
                exts[n++] = want[w];
        }
        ci.enabledExtensionCount = n;
        ci.ppEnabledExtensionNames = exts;
    }
    else
        layer_log("passing through: VK_EXT_map_memory_placed %d, VK_KHR_map_memory2 %d, memoryMapPlaced %u, "
                  "bufferDeviceAddress %u, bufferDeviceAddressCaptureReplay %u", have_placed, have_map2,
                  placed_caps.memoryMapPlaced, bda_caps.bufferDeviceAddress, bda_caps.bufferDeviceAddressCaptureReplay);

    vr = create(phys, &ci, alloc, device);
    free(exts);
    if (vr != VK_SUCCESS)
        return vr;

    data = calloc(1, sizeof(*data));
    data->key = dispatch_key(*device);
    data->device = *device;
    data->gdpa = gdpa;
    data->identity = identity;
    data->unmap_reserve = unmap_reserve;
    pthread_mutex_init(&data->lock, NULL);
#define GET(field, name) data->field = (void *)gdpa(*device, name)
    GET(destroy, "vkDestroyDevice");
    GET(allocate, "vkAllocateMemory");
    GET(free, "vkFreeMemory");
    GET(map, "vkMapMemory");
    GET(unmap, "vkUnmapMemory");
    GET(map2, "vkMapMemory2KHR");
    if (!data->map2)
        GET(map2, "vkMapMemory2");
    GET(unmap2, "vkUnmapMemory2KHR");
    if (!data->unmap2)
        GET(unmap2, "vkUnmapMemory2");
#undef GET
    if (!data->map2 || !data->unmap2)
        data->identity = 0;

    VkPhysicalDeviceMemoryProperties mp;
    inst->memory_properties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            data->host_visible_types |= 1u << i;

    if (identity && !data->identity)
        layer_log("passing through: vkMapMemory2/vkUnmapMemory2 not available on the device");
    if (data->identity)
    {
        pthread_mutex_lock(&window_lock);
        window_init_locked();
        pthread_mutex_unlock(&window_lock);
        layer_log("identity mapping enabled for host-visible memory (types %#x)", data->host_visible_types);
    }
    pthread_mutex_lock(&global_lock);
    data->next = devices;
    devices = data;
    pthread_mutex_unlock(&global_lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    struct device_data *data = get_device(device), **p;

    if (!data)
        return;
    data->destroy(device, alloc);
    pthread_mutex_lock(&global_lock);
    for (p = &devices; *p && *p != data; p = &(*p)->next)
        ;
    if (*p)
        *p = data->next;
    pthread_mutex_unlock(&global_lock);
    for (unsigned b = 0; b < MEMORY_BUCKETS; b++)
        while (data->memories[b])
        {
            struct memory *m = data->memories[b];
            data->memories[b] = m->next;
            window_free(m->address, m->reserved);
            free(m);
        }
    free(data);
}

/* ----------------------------------------------------------------- memory */

/* Allocations that import, export or already request an address keep their
 * own semantics. */
static int wants_identity(struct device_data *d, const VkMemoryAllocateInfo *info)
{
    static const VkStructureType skip[] = {
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO,
    };

    static int logged_type, logged_struct[sizeof(skip) / sizeof(*skip)];

    if (!d->identity)
        return 0;
    if (__atomic_load_n(&d->types_refused, __ATOMIC_RELAXED) & (1u << info->memoryTypeIndex))
        return 0;
    if (!(d->host_visible_types & (1u << info->memoryTypeIndex)))
    {
        if (!__atomic_exchange_n(&logged_type, 1, __ATOMIC_RELAXED))
            layer_log("skipping device-local allocations (first: type %u, %#" PRIx64 " bytes)",
                      info->memoryTypeIndex, (uint64_t)info->allocationSize);
        return 0;
    }
    for (unsigned i = 0; i < sizeof(skip) / sizeof(*skip); i++)
        if (find_struct(info->pNext, skip[i]))
        {
            if (!__atomic_exchange_n(&logged_struct[i], 1, __ATOMIC_RELAXED))
                layer_log("skipping host-visible allocation carrying sType %d (type %u, %#" PRIx64 " bytes)",
                          skip[i], info->memoryTypeIndex, (uint64_t)info->allocationSize);
            return 0;
        }
    return 1;
}

/* ------------------------------------------------------------- mailbox */

/* The mailbox, if REBOX_GPUVA_MAILBOX=1 and it is readable (xmem.dll commits it). Read through
 * process_vm_readv, so an unmapped page is an error, not a fault. */
static struct gpuva_mailbox *mailbox_get(void)
{
    static int state; /* 0 unknown, 1 on, -1 off */
    static struct gpuva_mailbox *box;
    struct gpuva_mailbox copy;
    struct iovec local = { &copy, sizeof(copy) }, remote;
    const char *env;

    if (state)
        return state > 0 ? box : NULL;
    env = getenv("REBOX_GPUVA_MAILBOX");
    pthread_mutex_lock(&window_lock);
    window_init_locked();
    pthread_mutex_unlock(&window_lock);
    box = (struct gpuva_mailbox *)(uintptr_t)(window_base + window_size - GPUVA_MAILBOX_SIZE);
    remote = (struct iovec){ box, sizeof(*box) };
    if (!env || strcmp(env, "1"))
        state = -1;
    else if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != sizeof(copy) || copy.magic != GPUVA_MAILBOX_MAGIC)
    {
        layer_log("mailbox at %p isn't there: title memory can't be made GPU-visible", (void *)box);
        state = -1;
    }
    else
    {
        layer_log("mailbox at %p", (void *)box);
        state = 1;
    }
    return state > 0 ? box : NULL;
}

/* The address requested for an allocation of this size, or 0. */
static uint64_t mailbox_request(uint64_t size)
{
    struct gpuva_mailbox *box = mailbox_get();
    uint64_t address;

    if (!box || !(address = __atomic_load_n(&box->address, __ATOMIC_ACQUIRE)))
        return 0;
    if (__atomic_load_n(&box->size, __ATOMIC_RELAXED) != size)
        return 0;
    /* Taken: a second allocation of the same size doesn't get it too. */
    if (!__atomic_compare_exchange_n(&box->address, &address, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return 0;
    return address;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
        const VkAllocationCallbacks *alloc, VkDeviceMemory *memory)
{
    struct device_data *d = get_device(device);
    VkMemoryAllocateFlagsInfo *flags, own_flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    VkMemoryOpaqueCaptureAddressAllocateInfo oc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO };
    VkMemoryAllocateInfo ai;
    VkMemoryAllocateFlags saved_flags = 0;
    uint64_t size, align, address, adopt;
    struct memory *m;
    VkResult vr;

    if (!wants_identity(d, info))
        return d->allocate(device, info, alloc, memory);
    adopt = mailbox_request(info->allocationSize);

    /* Placed maps cover whole allocations, so round sizes to the alignment
     * we use for addresses. RADV maps a guard page past each allocation on
     * GPUs with the SMEM out-of-bounds erratum (RADEON_FLAG_VM_PAD_1PAGE),
     * so leave a gap of one alignment unit before the next one. */
    align = info->allocationSize >= (2u << 20) ? (2u << 20) : (64u << 10);
    size = (info->allocationSize + align - 1) & ~(align - 1);

    ai = *info;
    ai.allocationSize = size;
    if ((flags = (void *)find_struct(info->pNext, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO)))
    {
        /* Not ours to keep: modify for the call, then restore. */
        saved_flags = flags->flags;
        flags->flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
        oc.pNext = (void *)ai.pNext;
    }
    else
    {
        own_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
        own_flags.pNext = (void *)ai.pNext;
        oc.pNext = &own_flags;
    }
    ai.pNext = &oc;

    /* A refused address may just be taken in the driver's GPU VA space; try
     * a few fresh ones (keeping the refused ranges out of the window) before
     * concluding the driver can't do lower-half addresses at all. */
    if (adopt)
    {
        /* Title memory: exactly there, or not at all (the caller sees result 0). */
        size = info->allocationSize;
        oc.opaqueCaptureAddress = adopt;
        vr = d->allocate(device, &ai, alloc, memory);
        if (flags)
            flags->flags = saved_flags;
        if (vr != VK_SUCCESS)
        {
            layer_log("driver refused device address %#" PRIx64 " for title memory (%#" PRIx64 " bytes): %d",
                      adopt, size, vr);
            return d->allocate(device, info, alloc, memory);
        }
        m = calloc(1, sizeof(*m));
        m->handle = *memory;
        m->address = adopt;
        m->size = size;
        m->adopted = 1;
        pthread_mutex_lock(&d->lock);
        m->next = d->memories[bucket(*memory)];
        d->memories[bucket(*memory)] = m;
        pthread_mutex_unlock(&d->lock);
        __atomic_store_n(&mailbox_get()->result, adopt, __ATOMIC_RELEASE);
        return VK_SUCCESS;
    }
    for (int attempt = 0;; attempt++)
    {
        if (!(address = window_alloc(size + align, align)))
        {
            layer_log("window exhausted (%#" PRIx64 " bytes); allocation not identity-mapped", size);
            vr = VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS;
            break;
        }
        oc.opaqueCaptureAddress = address;
        vr = d->allocate(device, &ai, alloc, memory);
        if (vr != VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
            break;
        layer_log("driver refused device address %#" PRIx64 " (%#" PRIx64 " bytes, type %u)", address, size,
                  info->memoryTypeIndex);
        if (!(__atomic_load_n(&d->types_ok, __ATOMIC_RELAXED) & (1u << info->memoryTypeIndex)) || attempt == 3)
        {
            window_free(address, size + align);
            break;
        }
        /* Probably a range the driver hasn't released yet: jump well past it.
         * The refused range stays allocated so it isn't tried again soon. */
        window_skip(256ull << 20);
    }
    if (flags)
        flags->flags = saved_flags;

    if (vr == VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
    {
        uint32_t bit = 1u << info->memoryTypeIndex;

        /* A type that has never worked won't: stop trying it. A type that
         * has worked just hit taken ranges; keep trying it next time. */
        if (!(__atomic_load_n(&d->types_ok, __ATOMIC_RELAXED) & bit))
        {
            uint32_t refused = __atomic_or_fetch(&d->types_refused, bit, __ATOMIC_RELAXED);
            layer_log("memory type %u is never identity-mapped from now on", info->memoryTypeIndex);
            if ((refused & d->host_visible_types) == d->host_visible_types)
                layer_log("no memory type accepts lower-half device addresses: identity mapping is off "
                          "(needs .#radv-rebox, see docs/d3d12x.md)");
        }
        return d->allocate(device, info, alloc, memory);
    }
    if (vr != VK_SUCCESS)
    {
        window_free(address, size + align);
        return vr;
    }
    __atomic_or_fetch(&d->types_ok, 1u << info->memoryTypeIndex, __ATOMIC_RELAXED);

    {
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            layer_log("first identity allocation at %#" PRIx64 " (%#" PRIx64 " bytes, type %u)",
                      address, size, info->memoryTypeIndex);
    }
    m = calloc(1, sizeof(*m));
    m->handle = *memory;
    m->address = address;
    m->size = size;
    m->reserved = size + align;
    pthread_mutex_lock(&d->lock);
    m->next = d->memories[bucket(*memory)];
    d->memories[bucket(*memory)] = m;
    pthread_mutex_unlock(&d->lock);
    return VK_SUCCESS;
}

/* Caller holds d->lock. Keeps the CPU range reserved after unmapping. */
static void unmap_identity(struct device_data *d, struct memory *m)
{
    VkMemoryUnmapInfoKHR ui = { .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = m->handle,
        .flags = d->unmap_reserve ? VK_MEMORY_UNMAP_RESERVE_BIT_EXT : 0 };

    if (!m->mapped)
        return;
    d->unmap2(d->device, &ui);
    m->mapped = 0;
    if (m->adopted) /* the title's memory again: readable and writable, its contents gone */
        mmap((void *)(uintptr_t)m->address, m->size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    else if (!d->unmap_reserve)
        mmap((void *)(uintptr_t)m->address, m->size, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

static VKAPI_ATTR void VKAPI_CALL layer_FreeMemory(VkDevice device, VkDeviceMemory memory,
        const VkAllocationCallbacks *alloc)
{
    struct device_data *d = get_device(device);
    struct memory **p, *m = NULL;

    if (memory)
    {
        pthread_mutex_lock(&d->lock);
        if (*(p = find_memory(d, memory)))
        {
            m = *p;
            *p = m->next;
            unmap_identity(d, m);
        }
        pthread_mutex_unlock(&d->lock);
    }
    d->free(device, memory, alloc);
    if (m)
    {
        if (!m->adopted)
            window_free(m->address, m->reserved);
        free(m);
    }
}

/* Returns VK_INCOMPLETE if the memory isn't ours (caller passes through). */
static VkResult map_identity(struct device_data *d, VkDeviceMemory memory, VkDeviceSize offset, void **data)
{
    struct memory *m;
    VkResult vr = VK_SUCCESS;

    pthread_mutex_lock(&d->lock);
    if (!(m = *find_memory(d, memory)))
    {
        pthread_mutex_unlock(&d->lock);
        return VK_INCOMPLETE;
    }
    if (!m->mapped && m->adopted)
    {
        /* Title memory: the allocation is mapped somewhere else first and filled with the title's
         * bytes, then mapped over them in one mmap, so other threads never see the pages empty. */
        VkMemoryMapPlacedInfoEXT pi = { .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT };
        VkMemoryMapInfoKHR mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi,
            .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = memory, .offset = 0, .size = VK_WHOLE_SIZE };
        VkMemoryUnmapInfoKHR ui = { .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = memory };
        void *temp = mmap(NULL, m->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0), *p = NULL;

        if (temp != MAP_FAILED)
        {
            pi.pPlacedAddress = temp;
            if ((vr = d->map2(d->device, &mi, &p)) == VK_SUCCESS)
            {
                memcpy(temp, (void *)(uintptr_t)m->address, m->size);
                d->unmap2(d->device, &ui);
            }
            munmap(temp, m->size);
        }
        else
            vr = VK_ERROR_MEMORY_MAP_FAILED;
        if (vr == VK_SUCCESS)
        {
            pi.pPlacedAddress = (void *)(uintptr_t)m->address;
            if ((vr = d->map2(d->device, &mi, &p)) == VK_SUCCESS)
                m->mapped = 1;
        }
        if (vr != VK_SUCCESS)
            layer_log("placed map of title memory at %#" PRIx64 " failed: %d", m->address, vr);
    }
    if (!m->mapped)
    {
        VkMemoryMapPlacedInfoEXT pi = { .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,
            .pPlacedAddress = (void *)(uintptr_t)m->address };
        VkMemoryMapInfoKHR mi = { .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi,
            .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = memory, .offset = 0, .size = VK_WHOLE_SIZE };
        void *p = NULL;

        if ((vr = d->map2(d->device, &mi, &p)) == VK_SUCCESS)
            m->mapped = 1;
        else
            layer_log("placed map at %#" PRIx64 " failed: %d", m->address, vr);
    }
    if (m->mapped)
        *data = (void *)(uintptr_t)(m->address + offset);
    pthread_mutex_unlock(&d->lock);
    return vr;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_MapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
        VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
    struct device_data *d = get_device(device);
    VkResult vr = map_identity(d, memory, offset, data);

    return vr == VK_INCOMPLETE ? d->map(device, memory, offset, size, flags, data) : vr;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_MapMemory2(VkDevice device, const VkMemoryMapInfoKHR *info, void **data)
{
    struct device_data *d = get_device(device);
    VkResult vr = VK_INCOMPLETE;

    /* A caller asking for its own placement (winevulkan under WoW64) wins. */
    if (!(info->flags & VK_MEMORY_MAP_PLACED_BIT_EXT))
        vr = map_identity(d, info->memory, info->offset, data);
    if (vr != VK_INCOMPLETE)
        return vr;
    if (d->map2)
        return d->map2(device, info, data);
    return d->map(device, info->memory, info->offset, info->size, info->flags, data);
}

/* Identity mappings stay until the memory is freed: vkd3d-proton maps heaps
 * persistently, and remapping could not move them anyway. */
static int is_identity(struct device_data *d, VkDeviceMemory memory)
{
    int found;

    pthread_mutex_lock(&d->lock);
    found = *find_memory(d, memory) != NULL;
    pthread_mutex_unlock(&d->lock);
    return found;
}

static VKAPI_ATTR void VKAPI_CALL layer_UnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    struct device_data *d = get_device(device);

    if (!is_identity(d, memory))
        d->unmap(device, memory);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_UnmapMemory2(VkDevice device, const VkMemoryUnmapInfoKHR *info)
{
    struct device_data *d = get_device(device);

    if (is_identity(d, info->memory))
        return VK_SUCCESS;
    if (d->unmap2)
        return d->unmap2(device, info);
    d->unmap(device, info->memory);
    return VK_SUCCESS;
}

/* ---------------------------------------------------------- proc address */

static PFN_vkVoidFunction device_hook(const char *name)
{
    static const struct { const char *name; PFN_vkVoidFunction fn; } hooks[] = {
        { "vkDestroyDevice", (PFN_vkVoidFunction)layer_DestroyDevice },
        { "vkAllocateMemory", (PFN_vkVoidFunction)layer_AllocateMemory },
        { "vkFreeMemory", (PFN_vkVoidFunction)layer_FreeMemory },
        { "vkMapMemory", (PFN_vkVoidFunction)layer_MapMemory },
        { "vkUnmapMemory", (PFN_vkVoidFunction)layer_UnmapMemory },
        { "vkMapMemory2", (PFN_vkVoidFunction)layer_MapMemory2 },
        { "vkMapMemory2KHR", (PFN_vkVoidFunction)layer_MapMemory2 },
        { "vkUnmapMemory2", (PFN_vkVoidFunction)layer_UnmapMemory2 },
        { "vkUnmapMemory2KHR", (PFN_vkVoidFunction)layer_UnmapMemory2 },
    };
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(*hooks); i++)
        if (!strcmp(name, hooks[i].name))
            return hooks[i].fn;
    return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device, const char *name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    struct instance_data *inst;
    PFN_vkVoidFunction fn;

    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)layer_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)layer_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)layer_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)layer_CreateDevice;
    if (!instance || !(inst = get_instance(instance)))
        return device_hook(name);
    /* Only hook what exists below: callers probe for vkMapMemory2KHR and
     * would otherwise take our hook as proof the extension is there. */
    if (!(fn = inst->gipa(instance, name)))
        return NULL;
    return device_hook(name) ? device_hook(name) : fn;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device, const char *name)
{
    struct device_data *d;
    PFN_vkVoidFunction fn;

    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)layer_GetDeviceProcAddr;
    if (!(d = get_device(device)))
        return device_hook(name);
    if (!(fn = d->gdpa(device, name)))
        return NULL;
    return device_hook(name) ? device_hook(name) : fn;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL layer_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v->loaderLayerInterfaceVersion > 2)
        v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
