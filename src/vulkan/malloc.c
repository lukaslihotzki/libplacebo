/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */

#include "malloc.h"
#include "command.h"
#include "utils.h"

#ifdef VK_HAVE_UNIX
#include <errno.h>
#include <strings.h>
#include <unistd.h>
#endif

// Controls the multiplication factor for new slab allocations. The new slab
// will always be allocated such that the size of the slab is this factor times
// the previous slab. Higher values make it grow faster.
#define PLVK_HEAP_SLAB_GROWTH_RATE 4

// Controls the minimum slab size, to reduce the frequency at which very small
// slabs would need to get allocated when allocating the first few buffers.
// (Default: 1 MB)
#define PLVK_HEAP_MINIMUM_SLAB_SIZE (1 << 20)

// Controls the maximum slab size, to reduce the effect of unbounded slab
// growth exhausting memory. If the application needs a single allocation
// that's bigger than this value, it will be allocated directly from the
// device. (Default: 256 MB)
#define PLVK_HEAP_MAXIMUM_SLAB_SIZE (1 << 28)

// Controls the minimum free region size, to reduce thrashing the free space
// map with lots of small buffers during uninit. (Default: 1 KB)
#define PLVK_HEAP_MINIMUM_REGION_SIZE (1 << 10)

// Represents a region of available memory
struct vk_region {
    size_t start; // first offset in region
    size_t end;   // first offset *not* in region
};

static inline size_t region_len(struct vk_region r)
{
    return r.end - r.start;
}

// A single slab represents a contiguous region of allocated memory. Actual
// allocations are served as slices of this. Slabs are organized into linked
// lists, which represent individual heaps.
struct vk_slab {
    VkDeviceMemory mem;   // underlying device allocation
    size_t size;          // total size of `slab`
    size_t used;          // number of bytes actually in use (for GC accounting)
    bool dedicated;       // slab is allocated specifically for one object
    bool imported;        // slab represents an imported memory allocation
    // free space map: a sorted list of memory regions that are available
    struct vk_region *regions;
    int num_regions;
    // optional, depends on the memory type:
    VkBuffer buffer;        // buffer spanning the entire slab
    void *data;             // mapped memory corresponding to `mem`
    bool coherent;          // mapped memory is coherent
    union pl_handle handle; // handle associated with this device memory
    enum pl_handle_type handle_type;
};

// Represents a single memory heap. We keep track of a vk_heap for each
// combination of buffer type and memory selection parameters. This shouldn't
// actually be that many in practice, because some combinations simply never
// occur, and others will generally be the same for the same objects.
//
// Note: `vk_heap` addresses are not immutable, so we musn't expose any dangling
// references to a `vk_heap` from e.g. `vk_memslice.priv = vk_slab`.
struct vk_heap {
    VkBufferUsageFlags usage;    // the buffer usage type (or 0)
    VkMemoryPropertyFlags flags; // the memory type flags (or 0)
    uint32_t typeBits;           // the memory type index requirements (or 0)
    enum pl_handle_type handle_type; // handle type available for this heap
    struct vk_slab **slabs;      // array of slabs sorted by size
    int num_slabs;
};

// The overall state of the allocator, which keeps track of a vk_heap for each
// memory type.
struct vk_malloc {
    struct vk_ctx *vk;
    VkPhysicalDeviceMemoryProperties props;
    VkDeviceSize host_ptr_align;
    struct vk_heap *heaps;
    int num_heaps;
};

static void slab_free(struct vk_ctx *vk, struct vk_slab *slab)
{
    if (!slab)
        return;

    pl_assert(slab->used == 0);
    if (!slab->imported) {
        vk->DestroyBuffer(vk->dev, slab->buffer, VK_ALLOC);

        switch (slab->handle_type) {
        case PL_HANDLE_FD:
        case PL_HANDLE_DMA_BUF:
#ifdef VK_HAVE_UNIX
            if (slab->handle.fd > -1)
                close(slab->handle.fd);
#endif
            break;
        case PL_HANDLE_WIN32:
#ifdef VK_HAVE_WIN32
            if (slab->handle.handle != NULL)
                CloseHandle(slab->handle.handle);
#endif
            break;
        case PL_HANDLE_WIN32_KMT:
            // PL_HANDLE_WIN32_KMT is just an identifier. It doesn't get closed.
            break;
        case PL_HANDLE_HOST_PTR:
            // Implicitly unmapped
            break;
        }

        PL_INFO(vk, "Freed slab of size %zu", (size_t) slab->size);
    } else {
        PL_DEBUG(vk, "Unimporting slab of size %zu from fd: %d",
                 (size_t) slab->size, slab->handle.fd);
    }

    // also implicitly unmaps the memory if needed
    vk->FreeMemory(vk->dev, slab->mem, VK_ALLOC);

    talloc_free(slab);
}

static bool find_best_memtype(struct vk_malloc *ma, uint32_t typeBits,
                              VkMemoryPropertyFlags flags,
                              VkMemoryType *out_type, int *out_index)
{
    struct vk_ctx *vk = ma->vk;

    // The vulkan spec requires memory types to be sorted in the "optimal"
    // order, so the first matching type we find will be the best/fastest one.
    for (int i = 0; i < ma->props.memoryTypeCount; i++) {
        // The memory type flags must include our properties
        if ((ma->props.memoryTypes[i].propertyFlags & flags) != flags)
            continue;
        // The memory type must be supported by the requirements (bitfield)
        if (typeBits && !(typeBits & (1 << i)))
            continue;
        *out_type = ma->props.memoryTypes[i];
        *out_index = i;
        return true;
    }

    PL_ERR(vk, "Found no memory type matching property flags 0x%x and type "
           "bits 0x%x!", (unsigned)flags, (unsigned)typeBits);
    return false;
}

static bool buf_external_check(struct vk_ctx *vk, VkBufferUsageFlags usage,
                               enum pl_handle_type handle_type, bool import)
{
    if (!handle_type)
        return true;

    if (!vk->GetPhysicalDeviceExternalBufferPropertiesKHR)
        return false;

    VkPhysicalDeviceExternalBufferInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO_KHR,
        .usage = usage,
        .handleType = vk_mem_handle_type(handle_type),
    };

    VkExternalBufferPropertiesKHR props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES_KHR,
    };

    pl_assert(info.handleType);
    vk->GetPhysicalDeviceExternalBufferPropertiesKHR(vk->physd, &info, &props);
    return vk_external_mem_check(&props.externalMemoryProperties, handle_type,
                                 import);
}

static struct vk_slab *slab_alloc(struct vk_malloc *ma, struct vk_heap *heap,
                                  size_t size)
{
    struct vk_ctx *vk = ma->vk;
    struct vk_slab *slab = talloc_ptrtype(NULL, slab);
    *slab = (struct vk_slab) {
        .size = size,
        .handle_type = heap->handle_type,
    };

    TARRAY_APPEND(slab, slab->regions, slab->num_regions, (struct vk_region) {
        .start = 0,
        .end   = slab->size,
    });

    switch (slab->handle_type) {
    case PL_HANDLE_FD:
    case PL_HANDLE_DMA_BUF:
        slab->handle.fd = -1;
        break;
    case PL_HANDLE_WIN32:
    case PL_HANDLE_WIN32_KMT:
        slab->handle.handle = NULL;
        break;
    case PL_HANDLE_HOST_PTR:
        slab->handle.ptr = NULL;
        break;
    }

    VkExportMemoryAllocateInfoKHR ext_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR,
        .handleTypes = vk_mem_handle_type(slab->handle_type),
    };

    VkMemoryAllocateInfo minfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = heap->handle_type ? &ext_info : NULL,
        .allocationSize = slab->size,
    };

    uint32_t typeBits = heap->typeBits ? heap->typeBits : UINT32_MAX;
    if (heap->usage) {
        // FIXME: Since we can't keep track of queue family ownership properly,
        // and we don't know in advance what types of queue families this buffer
        // will belong to, we're forced to share all of our buffers between all
        // command pools.
        uint32_t qfs[3] = {0};
        for (int i = 0; i < vk->num_pools; i++)
            qfs[i] = vk->pools[i]->qf;

        VkExternalMemoryBufferCreateInfoKHR ext_buf_info = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO_KHR,
            .handleTypes = ext_info.handleTypes,
        };

        VkBufferCreateInfo binfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = heap->handle_type ? &ext_buf_info : NULL,
            .size  = slab->size,
            .usage = heap->usage,
            .sharingMode = vk->num_pools > 1 ? VK_SHARING_MODE_CONCURRENT
                                             : VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = vk->num_pools,
            .pQueueFamilyIndices = qfs,
        };

        if (!buf_external_check(vk, binfo.usage, slab->handle_type, false)) {
            PL_ERR(vk, "Failed allocating shared memory buffer: possibly "
                   "the handle type is unsupported?");
            goto error;
        }

        VK(vk->CreateBuffer(vk->dev, &binfo, VK_ALLOC, &slab->buffer));
        VK_NAME(BUFFER, slab->buffer, "slab");

        VkMemoryRequirements reqs = {0};
        vk->GetBufferMemoryRequirements(vk->dev, slab->buffer, &reqs);
        minfo.allocationSize = reqs.size; // this can be larger than slab->size
        typeBits &= reqs.memoryTypeBits;  // this can restrict the types
    }

    VkMemoryType type;
    int index;
    if (!find_best_memtype(ma, typeBits, heap->flags, &type, &index))
        goto error;

    PL_INFO(vk, "Allocating %zu memory of type 0x%x (id %d) in heap %d",
            (size_t) slab->size, (unsigned) type.propertyFlags, index,
            (int) type.heapIndex);

    minfo.memoryTypeIndex = index;
    VK(vk->AllocateMemory(vk->dev, &minfo, VK_ALLOC, &slab->mem));

    if (heap->flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        VK(vk->MapMemory(vk->dev, slab->mem, 0, VK_WHOLE_SIZE, 0, &slab->data));
        slab->coherent = heap->flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    if (slab->buffer)
        VK(vk->BindBufferMemory(vk->dev, slab->buffer, slab->mem, 0));

#ifdef VK_HAVE_UNIX
    if (slab->handle_type == PL_HANDLE_FD ||
        slab->handle_type == PL_HANDLE_DMA_BUF)
    {
        VkMemoryGetFdInfoKHR fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = slab->mem,
            .handleType = ext_info.handleTypes,
        };

        VK(vk->GetMemoryFdKHR(vk->dev, &fd_info, &slab->handle.fd));
    }
#endif

#ifdef VK_HAVE_WIN32
    if (slab->handle_type == PL_HANDLE_WIN32 ||
        slab->handle_type == PL_HANDLE_WIN32_KMT)
    {
        VkMemoryGetWin32HandleInfoKHR handle_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
            .memory = slab->mem,
            .handleType = ext_info.handleTypes,
        };

        VK(vk->GetMemoryWin32HandleKHR(vk->dev, &handle_info,
                                       &slab->handle.handle));
    }
#endif

    return slab;

error:
    slab_free(vk, slab);
    return NULL;
}

static void insert_region(struct vk_slab *slab, struct vk_region region)
{
    if (region.start == region.end)
        return;

    bool big_enough = region_len(region) >= PLVK_HEAP_MINIMUM_REGION_SIZE;

    // Find the index of the first region that comes after this
    for (int i = 0; i < slab->num_regions; i++) {
        struct vk_region *r = &slab->regions[i];

        // Check for a few special cases which can be coalesced
        if (r->end == region.start) {
            // The new region is at the tail of this region. In addition to
            // modifying this region, we also need to coalesce all the following
            // regions for as long as possible
            r->end = region.end;

            struct vk_region *next = &slab->regions[i+1];
            while (i+1 < slab->num_regions && r->end == next->start) {
                r->end = next->end;
                TARRAY_REMOVE_AT(slab->regions, slab->num_regions, i+1);
            }
            return;
        }

        if (r->start == region.end) {
            // The new region is at the head of this region. We don't need to
            // do anything special here - because if this could be further
            // coalesced backwards, the previous loop iteration would already
            // have caught it.
            r->start = region.start;
            return;
        }

        if (r->start > region.start) {
            // The new region comes somewhere before this region, so insert
            // it into this index in the array.
            if (big_enough) {
                TARRAY_INSERT_AT(slab, slab->regions, slab->num_regions,
                                    i, region);
            }
            return;
        }
    }

    // If we've reached the end of this loop, then all of the regions
    // come before the new region, and are disconnected - so append it
    if (big_enough)
        TARRAY_APPEND(slab, slab->regions, slab->num_regions, region);
}

static void heap_uninit(struct vk_ctx *vk, struct vk_heap *heap)
{
    for (int i = 0; i < heap->num_slabs; i++)
        slab_free(vk, heap->slabs[i]);

    talloc_free(heap->slabs);
    *heap = (struct vk_heap){0};
}

struct vk_malloc *vk_malloc_create(struct vk_ctx *vk)
{
    struct vk_malloc *ma = talloc_zero(NULL, struct vk_malloc);
    ma->vk = vk;

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
    };

    VkPhysicalDeviceProperties2 dprops = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &host_props,
    };

    vk->GetPhysicalDeviceProperties2KHR(vk->physd, &dprops);
    vk->GetPhysicalDeviceMemoryProperties(vk->physd, &ma->props);
    ma->host_ptr_align = host_props.minImportedHostPointerAlignment;

    PL_INFO(vk, "Memory heaps supported by device:");
    for (int i = 0; i < ma->props.memoryHeapCount; i++) {
        VkMemoryHeap heap = ma->props.memoryHeaps[i];
        PL_INFO(vk, "    heap %d: flags 0x%x size %zu",
                i, (unsigned) heap.flags, (size_t) heap.size);
    }

    PL_INFO(vk, "Memory types supported by device:");
    for (int i = 0; i < ma->props.memoryTypeCount; i++) {
        VkMemoryType type = ma->props.memoryTypes[i];
        PL_INFO(vk, "    type %d: flags 0x%x heap %d",
                i, (unsigned) type.propertyFlags, (int) type.heapIndex);
    }

    return ma;
}

void vk_malloc_destroy(struct vk_malloc **ma_ptr)
{
    struct vk_malloc *ma = *ma_ptr;
    if (!ma)
        return;

    for (int i = 0; i < ma->num_heaps; i++)
        heap_uninit(ma->vk, &ma->heaps[i]);

    TA_FREEP(ma_ptr);
}

pl_handle_caps vk_malloc_handle_caps(struct vk_malloc *ma, bool import)
{
    struct vk_ctx *vk = ma->vk;
    pl_handle_caps caps = 0;

    for (int i = 0; vk_mem_handle_list[i]; i++) {
        // Try seeing if we could allocate a "basic" buffer using these
        // capabilities, with no fancy buffer usage. More specific checks will
        // happen down the line at VkBuffer creation time, but this should give
        // us a rough idea of what the driver supports.
        enum pl_handle_type type = vk_mem_handle_list[i];
        if (buf_external_check(vk, VK_BUFFER_USAGE_TRANSFER_DST_BIT, type, import))
            caps |= type;
    }

    return caps;
}

void vk_free_memslice(struct vk_malloc *ma, struct vk_memslice slice)
{
    struct vk_ctx *vk = ma->vk;
    struct vk_slab *slab = slice.priv;
    if (!slab)
        return;

    pl_assert(slab->used >= slice.size);
    slab->used -= slice.size;

    PL_DEBUG(vk, "Freeing slice %zu + %zu from slab with size %zu",
             (size_t) slice.offset, (size_t) slice.size, (size_t) slab->size);

    if (slab->dedicated) {
        // If the slab was purpose-allocated for this memslice, we can just
        // free it here
        slab_free(vk, slab);
    } else {
        // Return the allocation to the free space map
        insert_region(slab, (struct vk_region) {
            .start = slice.offset,
            .end   = slice.offset + slice.size,
        });
    }
}

// reqs: can be NULL
static struct vk_heap *find_heap(struct vk_malloc *ma, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags flags,
                                 enum pl_handle_type handle_type,
                                 VkMemoryRequirements *reqs)
{
    pl_assert(PL_ISPOT(handle_type));
    int typeBits = reqs ? reqs->memoryTypeBits : 0;

    for (int i = 0; i < ma->num_heaps; i++) {
        if (ma->heaps[i].usage != usage)
            continue;
        if (ma->heaps[i].flags != flags)
            continue;
        if (ma->heaps[i].typeBits != typeBits)
            continue;
        if (ma->heaps[i].handle_type != handle_type)
            continue;
        return &ma->heaps[i];
    }

    // Not found => add it
    TARRAY_GROW(ma, ma->heaps, ma->num_heaps + 1);
    struct vk_heap *heap = &ma->heaps[ma->num_heaps++];
    *heap = (struct vk_heap) {
        .usage    = usage,
        .flags    = flags,
        .typeBits = typeBits,
        .handle_type = handle_type,
    };
    return heap;
}

static inline bool region_fits(struct vk_region r, size_t size, size_t align)
{
    return PL_ALIGN(r.start, align) + size <= r.end;
}

// Finds the best-fitting region in a heap. If the heap is too small or too
// fragmented, a new slab will be allocated under the hood.
static bool heap_get_region(struct vk_malloc *ma, struct vk_heap *heap,
                            size_t size, size_t align,
                            struct vk_slab **out_slab, int *out_index)
{
    struct vk_slab *slab = NULL;

    // If the allocation is very big, serve it directly instead of bothering
    // with the heap
    if (size > PLVK_HEAP_MAXIMUM_SLAB_SIZE) {
        slab = slab_alloc(ma, heap, size);
        if (slab)
            slab->dedicated = true;
        *out_slab = slab;
        *out_index = 0;
        return !!slab;
    }

    for (int i = 0; i < heap->num_slabs; i++) {
        slab = heap->slabs[i];
        if (slab->size < size)
            continue;

        // Attempt a best fit search
        int best = -1;
        for (int n = 0; n < slab->num_regions; n++) {
            struct vk_region r = slab->regions[n];
            if (!region_fits(r, size, align))
                continue;
            if (best >= 0 && region_len(r) > region_len(slab->regions[best]))
                continue;
            best = n;
        }

        if (best >= 0) {
            *out_slab = slab;
            *out_index = best;
            return true;
        }
    }

    // Otherwise, allocate a new vk_slab and append it to the list.
    size_t cur_size = PL_MAX(size, slab ? slab->size : 0);
    size_t slab_size = PLVK_HEAP_SLAB_GROWTH_RATE * cur_size;
    slab_size = PL_MAX(PLVK_HEAP_MINIMUM_SLAB_SIZE, slab_size);
    slab_size = PL_MIN(PLVK_HEAP_MAXIMUM_SLAB_SIZE, slab_size);
    pl_assert(slab_size >= size);
    slab = slab_alloc(ma, heap, slab_size);
    if (!slab)
        return false;
    TARRAY_APPEND(NULL, heap->slabs, heap->num_slabs, slab);

    // Return the only region there is in a newly allocated slab
    pl_assert(slab->num_regions == 1);
    *out_slab = slab;
    *out_index = 0;
    return true;
}

static bool slice_heap(struct vk_malloc *ma, struct vk_heap *heap, size_t size,
                       size_t alignment, struct vk_memslice *out)
{
    struct vk_ctx *vk = ma->vk;
    struct vk_slab *slab;
    int index;
    alignment = pl_lcm(alignment, vk->limits.bufferImageGranularity);
    if (!heap_get_region(ma, heap, size, alignment, &slab, &index))
        return false;

    struct vk_region reg = slab->regions[index];
    TARRAY_REMOVE_AT(slab->regions, slab->num_regions, index);
    VkDeviceSize offset = PL_ALIGN(reg.start, alignment);
    *out = (struct vk_memslice) {
        .vkmem = slab->mem,
        .offset = offset,
        .size = size,
        .priv = slab,
        .shared_mem = {
            .handle = slab->handle,
            .offset = offset,
            .size = slab->size,
        },
    };

    if (slab->data) {
        out->data = (uint8_t *) slab->data + offset;
        out->coherent = slab->coherent;
    }

    PL_DEBUG(vk, "Sub-allocating slice %zu + %zu from slab with size %zu",
             (size_t) out->offset, (size_t) out->size, (size_t) slab->size);

    size_t out_end = out->offset + out->size;
    insert_region(slab, (struct vk_region) { reg.start, out->offset });
    insert_region(slab, (struct vk_region) { out_end, reg.end });

    slab->used += size;
    return true;
}

bool vk_malloc_generic(struct vk_malloc *ma, VkMemoryRequirements reqs,
                       VkMemoryPropertyFlags flags,
                       enum pl_handle_type handle_type,
                       struct vk_memslice *out)
{
    struct vk_heap *heap = find_heap(ma, 0, flags, handle_type, &reqs);
    return slice_heap(ma, heap, reqs.size, reqs.alignment, out);
}

bool vk_malloc_buffer(struct vk_malloc *ma, VkBufferUsageFlags bufFlags,
                      VkMemoryPropertyFlags memFlags, VkDeviceSize size,
                      VkDeviceSize alignment, enum pl_handle_type handle_type,
                      struct vk_bufslice *out)
{
    struct vk_heap *heap = find_heap(ma, bufFlags, memFlags, handle_type, NULL);
    if (!slice_heap(ma, heap, size, alignment, &out->mem))
        return false;

    struct vk_slab *slab = out->mem.priv;
    out->buf = slab->buffer;

    return true;
}

bool vk_malloc_import(struct vk_malloc *ma, VkMemoryRequirements reqs,
                      enum pl_handle_type handle_type,
                      const struct pl_shared_mem *shared_mem,
                      struct vk_memslice *out)
{
    struct vk_ctx *vk = ma->vk;
    VkExternalMemoryHandleTypeFlagBitsKHR vk_handle_type;
    vk_handle_type = vk_mem_handle_type(handle_type);

    struct vk_slab *slab = NULL;

    if (reqs.size > shared_mem->size) {
        PL_ERR(vk, "Imported object requires memory larger than the provided "
               "size!");
        return false;
    }

    VkImportMemoryFdInfoKHR fdinfo = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = vk_handle_type,
        .fd = -1,
    };

    VkImportMemoryHostPointerInfoEXT ptrinfo = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = vk_handle_type,
    };

    VkMemoryAllocateInfo ainfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = shared_mem->size,
    };

    switch (handle_type) {
#ifdef VK_HAVE_UNIX
    case PL_HANDLE_DMA_BUF: {
        if (!vk->GetMemoryFdPropertiesKHR) {
            PL_ERR(vk, "Importing PL_HANDLE_DMA_BUF requires %s.",
                   VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
            return false;
        }

        VkMemoryFdPropertiesKHR fdprops = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        };

        VK(vk->GetMemoryFdPropertiesKHR(vk->dev,
                                        vk_handle_type,
                                        shared_mem->handle.fd,
                                        &fdprops));

        // We dup() the fd to make it safe to import the same original fd
        // multiple times.
        fdinfo.fd = dup(shared_mem->handle.fd);
        if (fdinfo.fd == -1) {
            PL_ERR(vk, "Failed to dup() fd (%d) when importing memory: %s",
                   fdinfo.fd, strerror(errno));
            return false;
        }

        reqs.memoryTypeBits &= fdprops.memoryTypeBits;
        ainfo.pNext = &fdinfo;
        break;
    }
#else // !VK_HAVE_UNIX
    case PL_HANDLE_DMA_BUF:
        PL_ERR(vk, "PL_HANDLE_DMA_BUF requires building with UNIX support!");
        return false;
#endif

    case PL_HANDLE_HOST_PTR: {
        if (!vk->GetMemoryHostPointerPropertiesEXT) {
            PL_ERR(vk, "Importing PL_HANDLE_HOST_PTR requires %s.",
                   VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
            return false;
        }

        uintptr_t iptr = (uintptr_t) shared_mem->handle.ptr;
        if (PL_ALIGN2(iptr, ma->host_ptr_align) != iptr) {
            PL_ERR(vk, "Imported host pointer %p does not adhere to the "
                   "alignment requirements required to import pointers: %zu",
                   shared_mem->handle.ptr, (size_t) ma->host_ptr_align);
            return false;
        }

        VkMemoryHostPointerPropertiesEXT ptrprops = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
        };

        VK(vk->GetMemoryHostPointerPropertiesEXT(vk->dev, vk_handle_type,
                                                 shared_mem->handle.ptr,
                                                 &ptrprops));

        ptrinfo.pHostPointer = shared_mem->handle.ptr;
        reqs.memoryTypeBits &= ptrprops.memoryTypeBits;
        ainfo.pNext = &ptrinfo;
        break;
    }

    case PL_HANDLE_FD:
    case PL_HANDLE_WIN32:
    case PL_HANDLE_WIN32_KMT:
        PL_ERR(vk, "vk_malloc_import: unsupported handle type %d", handle_type);
        return false;
    }

    pl_assert(ainfo.pNext);

    // We pick the first compatible memory type because we have no other basis
    // for choosing if there is more than one available.
    int first_mem_type = ffs(reqs.memoryTypeBits);
    if (!first_mem_type) {
        PL_ERR(vk, "No compatible memory types offered for imported memory");
        return false;
    }

    VkDeviceMemory vkmem = NULL;
    ainfo.memoryTypeIndex = first_mem_type - 1;
    VK(vk->AllocateMemory(vk->dev, &ainfo, VK_ALLOC, &vkmem));

    slab = talloc_ptrtype(NULL, slab);
    *slab = (struct vk_slab) {
        .mem = vkmem,
        .dedicated = true,
        .imported = true,
        .size = shared_mem->size,
        .used = shared_mem->size,
        .handle_type = handle_type,
    };

    *out = (struct vk_memslice) {
        .vkmem = vkmem,
        .size = shared_mem->size,
        .offset = shared_mem->offset,
        .shared_mem = *shared_mem,
        .priv = slab,
    };

    switch (handle_type) {
    case PL_HANDLE_DMA_BUF:
    case PL_HANDLE_FD:
        PL_DEBUG(vk, "Imported %zu of memory from fd: %d",
                 (size_t) slab->size, shared_mem->handle.fd);
        // fd ownership is transferred at this point.
        slab->handle.fd = fdinfo.fd;
        fdinfo.fd = -1;
        break;
    case PL_HANDLE_HOST_PTR:
        PL_DEBUG(vk, "Imported %zu of memory from ptr: %p",
                 (size_t) slab->size, shared_mem->handle.ptr);
        break;
    case PL_HANDLE_WIN32:
    case PL_HANDLE_WIN32_KMT:
        break;
    }

    VkMemoryPropertyFlags flags = ma->props.memoryTypes[ainfo.memoryTypeIndex].propertyFlags;
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        VK(vk->MapMemory(vk->dev, slab->mem, 0, VK_WHOLE_SIZE, 0, &slab->data));
        slab->coherent = flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        out->data = (uint8_t *) slab->data + out->offset;
        out->coherent = slab->coherent;
    }

    return true;

error:
    if (fdinfo.fd > -1)
        close(fdinfo.fd);
    talloc_free(slab);
    return false;
}
