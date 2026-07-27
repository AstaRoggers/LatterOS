#include "virtio_gpu.h"

#include "hhdm.h"
#include "page_allocator.h"
#include "paging.h"
#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIRTIO_PCI_VENDOR_ID             0x1AF4U
#define VIRTIO_GPU_PCI_DEVICE_ID         0x1050U
#define VIRTIO_PCI_CAP_VENDOR            0x09U
#define VIRTIO_PCI_CAP_COMMON_CFG        1U
#define VIRTIO_PCI_CAP_NOTIFY_CFG        2U

#define PCI_STATUS_CAPABILITIES          0x0010U
#define PCI_COMMAND_MEMORY               0x0002U
#define PCI_COMMAND_BUS_MASTER           0x0004U

#define VIRTIO_STATUS_ACKNOWLEDGE         0x01U
#define VIRTIO_STATUS_DRIVER              0x02U
#define VIRTIO_STATUS_DRIVER_OK           0x04U
#define VIRTIO_STATUS_FEATURES_OK         0x08U
#define VIRTIO_STATUS_FAILED              0x80U

#define VIRTIO_F_VERSION_1_HIGH           0x00000001U

#define VIRTQ_DESC_F_NEXT                 0x0001U
#define VIRTQ_DESC_F_WRITE                0x0002U
#define VIRTIO_GPU_QUEUE_CONTROL          0U
#define VIRTIO_GPU_QUEUE_CURSOR           1U
#define VIRTIO_GPU_QUEUE_CAPACITY         64U
#define VIRTIO_GPU_POLL_LIMIT             50000000U

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO   0x0100U
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101U
#define VIRTIO_GPU_CMD_SET_SCANOUT        0x0103U
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH     0x0104U
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105U
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106U
#define VIRTIO_GPU_CMD_UPDATE_CURSOR      0x0300U
#define VIRTIO_GPU_CMD_MOVE_CURSOR        0x0301U

#define VIRTIO_GPU_RESP_OK_NODATA         0x1100U
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO   0x1101U
#define VIRTIO_GPU_RESP_ERROR_FIRST       0x1200U
#define VIRTIO_GPU_FLAG_FENCE             0x00000001U

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  1U
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  2U
#define VIRTIO_GPU_MAX_SCANOUTS           16U
#define VIRTIO_GPU_RESOURCE_SCANOUT       1U
#define VIRTIO_GPU_RESOURCE_CURSOR        2U
#define VIRTIO_GPU_CURSOR_WIDTH           64U
#define VIRTIO_GPU_CURSOR_HEIGHT          64U
#define VIRTIO_GPU_MAX_WIDTH              1280U
#define VIRTIO_GPU_MAX_HEIGHT             800U
#define VIRTIO_GPU_MAX_PIXELS \
    ((uint64_t)VIRTIO_GPU_MAX_WIDTH * VIRTIO_GPU_MAX_HEIGHT)

#define DMA_PAGE_BYTES                    4096U
#define VIRTIO_GPU_MAX_BACKING_PAGES \
    ((VIRTIO_GPU_MAX_PIXELS * sizeof(uint32_t) + DMA_PAGE_BYTES - 1U) / \
        DMA_PAGE_BYTES)
#define VIRTIO_GPU_CURSOR_BACKING_PAGES   4U
#define VIRTIO_GPU_CONTROL_REQUEST_PAGES  4U
#define VIRTIO_GPU_MAX_REQUEST_SEGMENTS   4U


typedef volatile struct __attribute__((packed))
{
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} virtio_pci_common_cfg_t;

typedef struct
{
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} virtq_descriptor_t;

typedef struct
{
    uint16_t flags;
    uint16_t index;
    uint16_t ring[VIRTIO_GPU_QUEUE_CAPACITY];
    uint16_t used_event;
} virtq_available_t;

typedef struct
{
    uint32_t id;
    uint32_t length;
} virtq_used_element_t;

typedef struct
{
    uint16_t flags;
    uint16_t index;
    virtq_used_element_t ring[VIRTIO_GPU_QUEUE_CAPACITY];
    uint16_t available_event;
} virtq_used_t;

typedef struct
{
    virtq_descriptor_t descriptors[VIRTIO_GPU_QUEUE_CAPACITY];
    virtq_available_t available;
    uint16_t used_alignment_padding;
    virtq_used_t used;
} virtq_memory_t;

typedef struct
{
    uint64_t physical;
    uint8_t *virtual_address;
} dma_page_t;

typedef struct
{
    virtq_memory_t *memory;
    uint64_t memory_physical;
    volatile uint16_t *notify;
    uint16_t queue_index;
    uint16_t size;
    uint16_t last_used;
    bool ready;
} virtio_queue_t;

typedef struct __attribute__((packed))
{
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t context_id;
    uint32_t padding;
} virtio_gpu_ctrl_header_t;

typedef struct __attribute__((packed))
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_gpu_rectangle_t;

typedef struct __attribute__((packed))
{
    virtio_gpu_rectangle_t rectangle;
    uint32_t enabled;
    uint32_t flags;
} virtio_gpu_display_mode_t;

typedef struct __attribute__((packed))
{
    virtio_gpu_ctrl_header_t header;
    virtio_gpu_display_mode_t modes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_display_info_t;

typedef struct __attribute__((packed))
{
    virtio_gpu_ctrl_header_t header;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} virtio_gpu_resource_create_2d_t;

typedef struct __attribute__((packed))
{
    uint64_t address;
    uint32_t length;
    uint32_t padding;
} virtio_gpu_memory_entry_t;

typedef struct __attribute__((packed))
{
    virtio_gpu_ctrl_header_t header;
    uint32_t resource_id;
    uint32_t entry_count;
} virtio_gpu_attach_backing_header_t;

typedef struct __attribute__((packed))
{
    virtio_gpu_ctrl_header_t header;
    virtio_gpu_rectangle_t rectangle;
    uint32_t scanout_id;
    uint32_t resource_id;
} virtio_gpu_set_scanout_t;

typedef struct __attribute__((packed))
{
    virtio_gpu_ctrl_header_t header;
    virtio_gpu_rectangle_t rectangle;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_transfer_2d_t;

typedef struct __attribute__((packed))
{
    virtio_gpu_ctrl_header_t header;
    virtio_gpu_rectangle_t rectangle;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_flush_t;

typedef struct __attribute__((packed))
{
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
} virtio_gpu_cursor_position_t;

typedef struct __attribute__((packed))
{
    virtio_gpu_ctrl_header_t header;
    virtio_gpu_cursor_position_t position;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
} virtio_gpu_cursor_command_t;

static dma_page_t control_queue_page;
static dma_page_t cursor_queue_page;
static dma_page_t control_request_pages[
    VIRTIO_GPU_CONTROL_REQUEST_PAGES
];
static dma_page_t control_response_page;
static dma_page_t cursor_request_page;
static dma_page_t cursor_response_page;
static dma_page_t scanout_backing[VIRTIO_GPU_MAX_BACKING_PAGES];
static uint32_t scanout_backing_page_count;

static const pci_device_t *gpu_pci_device;
static virtio_pci_common_cfg_t *common_configuration;
static volatile uint8_t *notify_configuration;
static uint32_t notify_configuration_length;
static uint32_t notify_multiplier;
static virtio_queue_t control_queue;
static virtio_queue_t cursor_queue;
static virtio_gpu_stats_t statistics;
static bool initialized;
static bool available;
static bool cursor_visible;
static int32_t last_cursor_x;
static int32_t last_cursor_y;
static uint64_t next_fence_id;
static const char *status_text = "not initialized";

static void memory_clear(void *pointer, size_t byte_count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < byte_count; index++)
    {
        bytes[index] = 0U;
    }
}

static void memory_copy(
    void *destination,
    const void *source,
    size_t byte_count
)
{
    uint8_t *output = destination;
    const uint8_t *input = source;

    for (size_t index = 0; index < byte_count; index++)
    {
        output[index] = input[index];
    }
}

static void memory_barrier(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void prepare_header(
    virtio_gpu_ctrl_header_t *header,
    uint32_t command_type
)
{
    memory_clear(header, sizeof(*header));
    header->type = command_type;
    header->flags = VIRTIO_GPU_FLAG_FENCE;
    header->fence_id = next_fence_id++;
}

static void prepare_cursor_header(
    virtio_gpu_ctrl_header_t *header,
    uint32_t command_type
)
{
    /*
     * Cursor commands already travel on the dedicated fast cursor queue.
     * They do not need a completion fence on every movement; the response
     * descriptor is retained, but the host may complete it without forcing
     * the display update to finish synchronously.
     */
    memory_clear(header, sizeof(*header));
    header->type = command_type;
}

static bool allocate_dma_page(dma_page_t *page)
{
    if (page == NULL)
    {
        return false;
    }

    void *physical = alloc_page();

    if (physical == NULL)
    {
        return false;
    }

    page->physical = (uint64_t)physical;
    page->virtual_address = physical_to_virtual(page->physical);
    memory_clear(page->virtual_address, DMA_PAGE_BYTES);
    return true;
}

static bool allocate_dma_pages(
    dma_page_t *pages,
    uint32_t count
)
{
    for (uint32_t index = 0; index < count; index++)
    {
        if (!allocate_dma_page(&pages[index]))
        {
            return false;
        }
    }

    return true;
}

static bool dma_pages_write(
    dma_page_t *pages,
    uint32_t page_count,
    uint32_t offset,
    const void *source,
    uint32_t byte_count
)
{
    if (
        pages == NULL ||
        page_count == 0U ||
        source == NULL ||
        (uint64_t)offset + byte_count >
            (uint64_t)page_count * DMA_PAGE_BYTES
    )
    {
        return false;
    }

    const uint8_t *input = source;
    uint32_t remaining = byte_count;
    uint32_t position = offset;

    while (remaining != 0U)
    {
        uint32_t page_index = position / DMA_PAGE_BYTES;
        uint32_t page_offset = position % DMA_PAGE_BYTES;
        uint32_t chunk = DMA_PAGE_BYTES - page_offset;

        if (chunk > remaining)
        {
            chunk = remaining;
        }

        memory_copy(
            pages[page_index].virtual_address + page_offset,
            input,
            chunk
        );

        input += chunk;
        position += chunk;
        remaining -= chunk;
    }

    return true;
}

static bool backing_write(
    dma_page_t *pages,
    uint32_t page_count,
    uint64_t offset,
    const void *source,
    uint32_t byte_count
)
{
    if (
        offset > UINT32_MAX ||
        (uint64_t)offset + byte_count >
            (uint64_t)page_count * DMA_PAGE_BYTES
    )
    {
        return false;
    }

    return dma_pages_write(
        pages,
        page_count,
        (uint32_t)offset,
        source,
        byte_count
    );
}

static bool map_mmio_range(
    uint64_t physical_address,
    uint32_t byte_count
)
{
    if (byte_count == 0U)
    {
        return false;
    }

    uint64_t first = physical_address & ~(uint64_t)(DMA_PAGE_BYTES - 1U);
    uint64_t last =
        (physical_address + byte_count + DMA_PAGE_BYTES - 1U) &
        ~(uint64_t)(DMA_PAGE_BYTES - 1U);

    for (
        uint64_t physical = first;
        physical < last;
        physical += DMA_PAGE_BYTES
    )
    {
        uint64_t virtual_address =
            (uint64_t)physical_to_virtual(physical);

        if (
            !paging_is_mapped(virtual_address) &&
            !paging_map_kernel_page(
                virtual_address,
                physical,
                true
            )
        )
        {
            return false;
        }
    }

    return true;
}

static uint64_t pci_bar_address(
    const pci_device_t *device,
    uint8_t bar_index
)
{
    if (device == NULL || bar_index >= 6U)
    {
        return 0U;
    }

    uint32_t low = device->bars[bar_index];

    if ((low & 0x01U) != 0U)
    {
        return 0U;
    }

    uint64_t address = low & 0xFFFFFFF0U;

    if (
        (low & 0x06U) == 0x04U &&
        bar_index + 1U < 6U
    )
    {
        address |= (uint64_t)device->bars[bar_index + 1U] << 32;
    }

    return address;
}

static volatile uint8_t *capability_address(
    const pci_device_t *device,
    uint8_t bar_index,
    uint32_t offset,
    uint32_t length
)
{
    uint64_t bar = pci_bar_address(device, bar_index);

    if (bar == 0U || length == 0U)
    {
        return NULL;
    }

    uint64_t physical = bar + offset;

    if (!map_mmio_range(physical, length))
    {
        return NULL;
    }

    return physical_to_virtual(physical);
}

static const pci_device_t *find_gpu_device(void)
{
    for (uint32_t index = 0; index < pci_device_count(); index++)
    {
        const pci_device_t *device = pci_get_device(index);

        if (
            device != NULL &&
            device->vendor_id == VIRTIO_PCI_VENDOR_ID &&
            device->device_id == VIRTIO_GPU_PCI_DEVICE_ID
        )
        {
            return device;
        }
    }

    return NULL;
}

static bool parse_transport_capabilities(
    const pci_device_t *device
)
{
    uint16_t pci_status = pci_config_read16(
        device->bus,
        device->device,
        device->function,
        0x06U
    );

    if ((pci_status & PCI_STATUS_CAPABILITIES) == 0U)
    {
        status_text = "PCI capability list unavailable";
        return false;
    }

    uint8_t capability = (uint8_t)(
        pci_config_read8(
            device->bus,
            device->device,
            device->function,
            0x34U
        ) & 0xFCU
    );

    common_configuration = NULL;
    notify_configuration = NULL;
    notify_configuration_length = 0U;
    notify_multiplier = 0U;

    for (uint32_t visited = 0; visited < 64U; visited++)
    {
        if (capability < 0x40U)
        {
            break;
        }

        uint8_t capability_id = pci_config_read8(
            device->bus,
            device->device,
            device->function,
            capability
        );

        uint8_t next = pci_config_read8(
            device->bus,
            device->device,
            device->function,
            (uint8_t)(capability + 1U)
        );

        uint8_t capability_length = pci_config_read8(
            device->bus,
            device->device,
            device->function,
            (uint8_t)(capability + 2U)
        );

        if (
            capability_id == VIRTIO_PCI_CAP_VENDOR &&
            capability_length >= 16U
        )
        {
            uint8_t configuration_type = pci_config_read8(
                device->bus,
                device->device,
                device->function,
                (uint8_t)(capability + 3U)
            );

            uint8_t bar_index = pci_config_read8(
                device->bus,
                device->device,
                device->function,
                (uint8_t)(capability + 4U)
            );

            uint32_t offset = pci_config_read32(
                device->bus,
                device->device,
                device->function,
                (uint8_t)(capability + 8U)
            );

            uint32_t length = pci_config_read32(
                device->bus,
                device->device,
                device->function,
                (uint8_t)(capability + 12U)
            );

            volatile uint8_t *address = capability_address(
                device,
                bar_index,
                offset,
                length
            );

            if (
                configuration_type == VIRTIO_PCI_CAP_COMMON_CFG &&
                address != NULL &&
                length >= sizeof(virtio_pci_common_cfg_t)
            )
            {
                common_configuration =
                    (virtio_pci_common_cfg_t *)address;
            }
            else if (
                configuration_type == VIRTIO_PCI_CAP_NOTIFY_CFG &&
                address != NULL &&
                capability_length >= 20U
            )
            {
                notify_configuration = address;
                notify_configuration_length = length;
                notify_multiplier = pci_config_read32(
                    device->bus,
                    device->device,
                    device->function,
                    (uint8_t)(capability + 16U)
                );
            }
        }

        capability = (uint8_t)(next & 0xFCU);
    }

    if (common_configuration == NULL)
    {
        status_text = "Virtio common configuration missing";
        return false;
    }

    if (notify_configuration == NULL || notify_multiplier == 0U)
    {
        status_text = "Virtio notification configuration missing";
        return false;
    }

    return true;
}

static bool setup_queue(
    virtio_queue_t *queue,
    uint16_t queue_index,
    dma_page_t *memory_page
)
{
    if (
        queue == NULL ||
        memory_page == NULL ||
        memory_page->virtual_address == NULL ||
        sizeof(virtq_memory_t) > DMA_PAGE_BYTES
    )
    {
        return false;
    }

    common_configuration->queue_select = queue_index;
    memory_barrier();

    uint16_t offered_size = common_configuration->queue_size;

    if (offered_size == 0U)
    {
        return false;
    }

    uint16_t queue_size = offered_size;

    if (queue_size > VIRTIO_GPU_QUEUE_CAPACITY)
    {
        queue_size = VIRTIO_GPU_QUEUE_CAPACITY;
    }

    virtq_memory_t *memory =
        (virtq_memory_t *)memory_page->virtual_address;

    memory_clear(memory, DMA_PAGE_BYTES);

    queue->memory = memory;
    queue->memory_physical = memory_page->physical;
    queue->queue_index = queue_index;
    queue->size = queue_size;
    queue->last_used = 0U;
    queue->ready = false;

    common_configuration->queue_size = queue_size;
    common_configuration->queue_msix_vector = UINT16_MAX;
    common_configuration->queue_desc =
        memory_page->physical + offsetof(virtq_memory_t, descriptors);
    common_configuration->queue_driver =
        memory_page->physical + offsetof(virtq_memory_t, available);
    common_configuration->queue_device =
        memory_page->physical + offsetof(virtq_memory_t, used);

    uint16_t notification_offset =
        common_configuration->queue_notify_off;

    uint64_t notification_byte_offset =
        (uint64_t)notification_offset * notify_multiplier;

    if (
        notification_byte_offset + sizeof(uint16_t) >
        notify_configuration_length
    )
    {
        return false;
    }

    queue->notify = (volatile uint16_t *)(
        notify_configuration + notification_byte_offset
    );

    common_configuration->queue_enable = 1U;
    memory_barrier();

    if (common_configuration->queue_enable == 0U)
    {
        return false;
    }

    queue->ready = true;
    return true;
}

static bool queue_submit_synchronous(
    virtio_queue_t *queue,
    const dma_page_t *request_pages,
    const uint32_t *request_lengths,
    uint32_t request_segment_count,
    dma_page_t *response_page,
    uint32_t response_bytes
)
{
    if (
        queue == NULL ||
        !queue->ready ||
        request_pages == NULL ||
        request_lengths == NULL ||
        request_segment_count == 0U ||
        request_segment_count > VIRTIO_GPU_MAX_REQUEST_SEGMENTS ||
        response_page == NULL ||
        response_page->virtual_address == NULL ||
        response_bytes < sizeof(virtio_gpu_ctrl_header_t) ||
        response_bytes > DMA_PAGE_BYTES ||
        request_segment_count + 1U > queue->size
    )
    {
        return false;
    }

    memory_clear(response_page->virtual_address, DMA_PAGE_BYTES);

    virtq_descriptor_t *descriptors =
        queue->memory->descriptors;

    for (
        uint32_t index = 0;
        index < request_segment_count;
        index++
    )
    {
        if (
            request_lengths[index] == 0U ||
            request_lengths[index] > DMA_PAGE_BYTES ||
            request_pages[index].virtual_address == NULL
        )
        {
            return false;
        }

        descriptors[index].address = request_pages[index].physical;
        descriptors[index].length = request_lengths[index];
        descriptors[index].flags = VIRTQ_DESC_F_NEXT;
        descriptors[index].next = (uint16_t)(index + 1U);
    }

    uint32_t response_descriptor = request_segment_count;
    descriptors[response_descriptor].address = response_page->physical;
    descriptors[response_descriptor].length = response_bytes;
    descriptors[response_descriptor].flags = VIRTQ_DESC_F_WRITE;
    descriptors[response_descriptor].next = 0U;

    uint16_t available_index = __atomic_load_n(
        &queue->memory->available.index,
        __ATOMIC_RELAXED
    );

    queue->memory->available.ring[
        available_index % queue->size
    ] = 0U;

    memory_barrier();
    __atomic_store_n(
        &queue->memory->available.index,
        (uint16_t)(available_index + 1U),
        __ATOMIC_RELEASE
    );
    memory_barrier();

    *queue->notify = queue->queue_index;
    memory_barrier();

    for (uint32_t spin = 0; spin < VIRTIO_GPU_POLL_LIMIT; spin++)
    {
        memory_barrier();

        uint16_t device_used = __atomic_load_n(
            &queue->memory->used.index,
            __ATOMIC_ACQUIRE
        );

        if (device_used != queue->last_used)
        {
            uint16_t used_slot =
                (uint16_t)(queue->last_used % queue->size);

            virtq_used_element_t element =
                queue->memory->used.ring[used_slot];

            queue->last_used++;
            memory_barrier();
            return element.id == 0U;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool response_success(
    const virtio_gpu_ctrl_header_t *response,
    uint32_t expected_type
)
{
    if (response == NULL)
    {
        return false;
    }

    statistics.last_response_type = response->type;

    if (response->type >= VIRTIO_GPU_RESP_ERROR_FIRST)
    {
        return false;
    }

    return response->type == expected_type;
}

static bool submit_control_pages(
    uint32_t request_bytes,
    uint32_t response_bytes,
    uint32_t expected_response
)
{
    uint32_t segment_count =
        (request_bytes + DMA_PAGE_BYTES - 1U) /
        DMA_PAGE_BYTES;

    if (
        segment_count == 0U ||
        segment_count > VIRTIO_GPU_CONTROL_REQUEST_PAGES
    )
    {
        return false;
    }

    uint32_t lengths[VIRTIO_GPU_MAX_REQUEST_SEGMENTS] = { 0U };
    uint32_t remaining = request_bytes;

    for (uint32_t index = 0; index < segment_count; index++)
    {
        lengths[index] = remaining > DMA_PAGE_BYTES ?
            DMA_PAGE_BYTES : remaining;
        remaining -= lengths[index];
    }

    statistics.commands++;

    if (!queue_submit_synchronous(
        &control_queue,
        control_request_pages,
        lengths,
        segment_count,
        &control_response_page,
        response_bytes
    ))
    {
        statistics.command_errors++;
        return false;
    }

    if (!response_success(
        (const virtio_gpu_ctrl_header_t *)
            control_response_page.virtual_address,
        expected_response
    ))
    {
        statistics.command_errors++;
        return false;
    }

    return true;
}

static bool submit_control(
    const void *request,
    uint32_t request_bytes,
    uint32_t response_bytes,
    uint32_t expected_response
)
{
    if (
        request == NULL ||
        request_bytes == 0U ||
        request_bytes > DMA_PAGE_BYTES
    )
    {
        return false;
    }

    memory_clear(
        control_request_pages[0].virtual_address,
        DMA_PAGE_BYTES
    );

    memory_copy(
        control_request_pages[0].virtual_address,
        request,
        request_bytes
    );

    return submit_control_pages(
        request_bytes,
        response_bytes,
        expected_response
    );
}

static bool submit_cursor(
    const virtio_gpu_cursor_command_t *request
)
{
    memory_clear(cursor_request_page.virtual_address, DMA_PAGE_BYTES);
    memory_copy(
        cursor_request_page.virtual_address,
        request,
        sizeof(*request)
    );

    uint32_t request_length = sizeof(*request);
    statistics.commands++;

    if (!queue_submit_synchronous(
        &cursor_queue,
        &cursor_request_page,
        &request_length,
        1U,
        &cursor_response_page,
        sizeof(virtio_gpu_ctrl_header_t)
    ))
    {
        statistics.command_errors++;
        return false;
    }

    if (!response_success(
        (const virtio_gpu_ctrl_header_t *)
            cursor_response_page.virtual_address,
        VIRTIO_GPU_RESP_OK_NODATA
    ))
    {
        statistics.command_errors++;
        return false;
    }

    return true;
}

static virtio_gpu_rectangle_t make_rectangle(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
)
{
    virtio_gpu_rectangle_t rectangle = {
        .x = x,
        .y = y,
        .width = width,
        .height = height
    };

    return rectangle;
}

static bool get_display_information(uint32_t *scanout_id)
{
    virtio_gpu_ctrl_header_t request;
    prepare_header(
        &request,
        VIRTIO_GPU_CMD_GET_DISPLAY_INFO
    );

    if (!submit_control(
        &request,
        sizeof(request),
        sizeof(virtio_gpu_display_info_t),
        VIRTIO_GPU_RESP_OK_DISPLAY_INFO
    ))
    {
        return false;
    }

    const virtio_gpu_display_info_t *information =
        (const virtio_gpu_display_info_t *)
            control_response_page.virtual_address;

    for (
        uint32_t index = 0;
        index < VIRTIO_GPU_MAX_SCANOUTS;
        index++
    )
    {
        if (information->modes[index].enabled != 0U)
        {
            *scanout_id = index;
            return true;
        }
    }

    return false;
}

static bool create_resource(
    uint32_t resource_id,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    const dma_page_t *backing,
    uint32_t backing_page_count,
    uint32_t byte_count
)
{
    virtio_gpu_resource_create_2d_t create;
    memory_clear(&create, sizeof(create));
    prepare_header(
        &create.header,
        VIRTIO_GPU_CMD_RESOURCE_CREATE_2D
    );
    create.resource_id = resource_id;
    create.format = format;
    create.width = width;
    create.height = height;

    if (!submit_control(
        &create,
        sizeof(create),
        sizeof(virtio_gpu_ctrl_header_t),
        VIRTIO_GPU_RESP_OK_NODATA
    ))
    {
        return false;
    }

    uint32_t request_bytes =
        sizeof(virtio_gpu_attach_backing_header_t) +
        backing_page_count * sizeof(virtio_gpu_memory_entry_t);

    if (
        backing == NULL ||
        backing_page_count == 0U ||
        request_bytes >
            VIRTIO_GPU_CONTROL_REQUEST_PAGES * DMA_PAGE_BYTES
    )
    {
        return false;
    }

    for (
        uint32_t index = 0;
        index < VIRTIO_GPU_CONTROL_REQUEST_PAGES;
        index++
    )
    {
        memory_clear(
            control_request_pages[index].virtual_address,
            DMA_PAGE_BYTES
        );
    }

    virtio_gpu_attach_backing_header_t attach;
    memory_clear(&attach, sizeof(attach));
    prepare_header(
        &attach.header,
        VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING
    );
    attach.resource_id = resource_id;
    attach.entry_count = backing_page_count;

    if (!dma_pages_write(
        control_request_pages,
        VIRTIO_GPU_CONTROL_REQUEST_PAGES,
        0U,
        &attach,
        sizeof(attach)
    ))
    {
        return false;
    }

    uint32_t remaining = byte_count;

    for (uint32_t index = 0; index < backing_page_count; index++)
    {
        virtio_gpu_memory_entry_t entry;
        entry.address = backing[index].physical;
        entry.length = remaining > DMA_PAGE_BYTES ?
            DMA_PAGE_BYTES : remaining;
        entry.padding = 0U;

        uint32_t offset =
            sizeof(attach) +
            index * sizeof(entry);

        if (!dma_pages_write(
            control_request_pages,
            VIRTIO_GPU_CONTROL_REQUEST_PAGES,
            offset,
            &entry,
            sizeof(entry)
        ))
        {
            return false;
        }

        remaining -= entry.length;
    }

    return submit_control_pages(
        request_bytes,
        sizeof(virtio_gpu_ctrl_header_t),
        VIRTIO_GPU_RESP_OK_NODATA
    );
}

static bool set_scanout_resource(
    uint32_t scanout_id,
    uint32_t resource_id,
    uint32_t width,
    uint32_t height
)
{
    virtio_gpu_set_scanout_t command;
    memory_clear(&command, sizeof(command));
    prepare_header(
        &command.header,
        VIRTIO_GPU_CMD_SET_SCANOUT
    );
    command.rectangle = make_rectangle(0U, 0U, width, height);
    command.scanout_id = scanout_id;
    command.resource_id = resource_id;

    return submit_control(
        &command,
        sizeof(command),
        sizeof(virtio_gpu_ctrl_header_t),
        VIRTIO_GPU_RESP_OK_NODATA
    );
}

static bool transfer_rectangle(
    uint32_t resource_id,
    const virtio_gpu_rectangle_t *rectangle,
    uint32_t resource_width
)
{
    virtio_gpu_transfer_2d_t command;
    memory_clear(&command, sizeof(command));
    prepare_header(
        &command.header,
        VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D
    );
    command.rectangle = *rectangle;
    command.offset =
        ((uint64_t)rectangle->y * resource_width + rectangle->x) *
        sizeof(uint32_t);
    command.resource_id = resource_id;

    if (!submit_control(
        &command,
        sizeof(command),
        sizeof(virtio_gpu_ctrl_header_t),
        VIRTIO_GPU_RESP_OK_NODATA
    ))
    {
        return false;
    }

    statistics.transfers++;
    return true;
}

static bool flush_rectangle(
    uint32_t resource_id,
    const virtio_gpu_rectangle_t *rectangle
)
{
    virtio_gpu_resource_flush_t command;
    memory_clear(&command, sizeof(command));
    prepare_header(
        &command.header,
        VIRTIO_GPU_CMD_RESOURCE_FLUSH
    );
    command.rectangle = *rectangle;
    command.resource_id = resource_id;

    if (!submit_control(
        &command,
        sizeof(command),
        sizeof(virtio_gpu_ctrl_header_t),
        VIRTIO_GPU_RESP_OK_NODATA
    ))
    {
        return false;
    }

    statistics.flushes++;
    return true;
}

static bool initialize_dma(uint32_t width, uint32_t height)
{
    uint32_t scanout_bytes = width * height * sizeof(uint32_t);
    scanout_backing_page_count =
        (scanout_bytes + DMA_PAGE_BYTES - 1U) /
        DMA_PAGE_BYTES;

    if (
        scanout_backing_page_count > VIRTIO_GPU_MAX_BACKING_PAGES
    )
    {
        return false;
    }

    return
        allocate_dma_page(&control_queue_page) &&
        allocate_dma_page(&cursor_queue_page) &&
        allocate_dma_pages(
            control_request_pages,
            VIRTIO_GPU_CONTROL_REQUEST_PAGES
        ) &&
        allocate_dma_page(&control_response_page) &&
        allocate_dma_page(&cursor_request_page) &&
        allocate_dma_page(&cursor_response_page) &&
        allocate_dma_pages(
            scanout_backing,
            scanout_backing_page_count
        );
}

static bool initialize_resources(uint32_t width, uint32_t height)
{
    uint32_t scanout_id;

    if (!get_display_information(&scanout_id))
    {
        status_text = "GET_DISPLAY_INFO failed";
        return false;
    }

    uint32_t scanout_byte_count =
        width * height * sizeof(uint32_t);

    if (!create_resource(
        VIRTIO_GPU_RESOURCE_SCANOUT,
        VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
        width,
        height,
        scanout_backing,
        scanout_backing_page_count,
        scanout_byte_count
    ))
    {
        status_text = "scanout resource creation failed";
        return false;
    }

    if (!set_scanout_resource(
        scanout_id,
        VIRTIO_GPU_RESOURCE_SCANOUT,
        width,
        height
    ))
    {
        status_text = "SET_SCANOUT failed";
        return false;
    }

    virtio_gpu_rectangle_t full =
        make_rectangle(0U, 0U, width, height);

    memory_barrier();

    if (
        !transfer_rectangle(
            VIRTIO_GPU_RESOURCE_SCANOUT,
            &full,
            width
        ) ||
        !flush_rectangle(
            VIRTIO_GPU_RESOURCE_SCANOUT,
            &full
        )
    )
    {
        status_text = "initial scanout transfer failed";
        return false;
    }

    statistics.scanout_id = scanout_id;
    statistics.scanout_ready = true;

    /*
     * Hardware cursor commands are deliberately disabled. The desktop
     * compositor renders the cursor into the normal scanout damage stream,
     * keeping cursor position, hit-testing, and visible pixels synchronized.
     */
    statistics.cursor_ready = false;
    return true;
}

static bool reset_device(void)
{
    common_configuration->device_status = 0U;
    memory_barrier();

    for (uint32_t spin = 0; spin < VIRTIO_GPU_POLL_LIMIT; spin++)
    {
        if (common_configuration->device_status == 0U)
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

bool virtio_gpu_init(uint32_t width, uint32_t height)
{
    if (initialized)
    {
        return available;
    }

    initialized = true;
    available = false;
    cursor_visible = false;
    last_cursor_x = -1;
    last_cursor_y = -1;
    next_fence_id = 1U;
    memory_clear(&statistics, sizeof(statistics));

    if (
        width == 0U ||
        height == 0U ||
        width > VIRTIO_GPU_MAX_WIDTH ||
        height > VIRTIO_GPU_MAX_HEIGHT
    )
    {
        status_text = "display mode exceeds Virtio-GPU buffer limit";
        return false;
    }

    gpu_pci_device = find_gpu_device();

    if (gpu_pci_device == NULL)
    {
        status_text = "Virtio-GPU PCI device not found";
        return false;
    }

    statistics.pci_device_found = true;
    statistics.bus = gpu_pci_device->bus;
    statistics.device = gpu_pci_device->device;
    statistics.function = gpu_pci_device->function;
    statistics.pci_device_id = gpu_pci_device->device_id;

    pci_set_command_bits(
        gpu_pci_device,
        PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER
    );

    if (!parse_transport_capabilities(gpu_pci_device))
    {
        return false;
    }

    statistics.modern_transport = true;

    if (!initialize_dma(width, height))
    {
        status_text = "Virtio-GPU DMA allocation failed";
        return false;
    }

    if (!reset_device())
    {
        status_text = "Virtio-GPU reset timed out";
        return false;
    }

    common_configuration->device_status =
        VIRTIO_STATUS_ACKNOWLEDGE;
    common_configuration->device_status |=
        VIRTIO_STATUS_DRIVER;

    common_configuration->device_feature_select = 1U;
    memory_barrier();

    uint32_t high_features =
        common_configuration->device_feature;

    if ((high_features & VIRTIO_F_VERSION_1_HIGH) == 0U)
    {
        common_configuration->device_status |=
            VIRTIO_STATUS_FAILED;
        status_text = "Virtio 1.x feature unavailable";
        return false;
    }

    common_configuration->driver_feature_select = 0U;
    memory_barrier();
    common_configuration->driver_feature = 0U;
    common_configuration->driver_feature_select = 1U;
    memory_barrier();
    common_configuration->driver_feature =
        VIRTIO_F_VERSION_1_HIGH;
    memory_barrier();

    common_configuration->device_status |=
        VIRTIO_STATUS_FEATURES_OK;
    memory_barrier();

    if (
        (common_configuration->device_status &
            VIRTIO_STATUS_FEATURES_OK) == 0U
    )
    {
        common_configuration->device_status |=
            VIRTIO_STATUS_FAILED;
        status_text = "Virtio feature negotiation rejected";
        return false;
    }

    if (!setup_queue(
        &control_queue,
        VIRTIO_GPU_QUEUE_CONTROL,
        &control_queue_page
    ))
    {
        common_configuration->device_status |=
            VIRTIO_STATUS_FAILED;
        status_text = "Virtio-GPU control queue unavailable";
        return false;
    }

    if (control_queue.size < VIRTIO_GPU_MAX_REQUEST_SEGMENTS + 1U)
    {
        common_configuration->device_status |=
            VIRTIO_STATUS_FAILED;
        status_text = "Virtio-GPU control queue is too small";
        return false;
    }

    statistics.control_queue_ready = true;
    statistics.control_queue_size = control_queue.size;

    /*
     * Do not activate cursorq until its asynchronous lifetime and interrupt
     * handling are implemented. The stable compositor cursor uses controlq
     * scanout updates only.
     */
    statistics.cursor_queue_ready = false;
    statistics.cursor_queue_size = 0U;

    common_configuration->device_status |=
        VIRTIO_STATUS_DRIVER_OK;
    memory_barrier();

    statistics.width = width;
    statistics.height = height;

    if (!initialize_resources(width, height))
    {
        common_configuration->device_status |=
            VIRTIO_STATUS_FAILED;
        return false;
    }

    available = true;
    status_text = "Virtio-GPU 2D scanout ready; cursor composited";
    return true;
}

bool virtio_gpu_available(void)
{
    return available;
}

static bool normalize_damage(
    const ui_rect_t *input,
    uint32_t width,
    uint32_t height,
    ui_rect_t *output
)
{
    if (
        input == NULL ||
        output == NULL ||
        input->width == 0U ||
        input->height == 0U
    )
    {
        return false;
    }

    int64_t left = input->x;
    int64_t top = input->y;
    int64_t right = left + input->width;
    int64_t bottom = top + input->height;

    if (
        right <= 0 ||
        bottom <= 0 ||
        left >= width ||
        top >= height
    )
    {
        return false;
    }

    if (left < 0)
    {
        left = 0;
    }

    if (top < 0)
    {
        top = 0;
    }

    if (right > width)
    {
        right = width;
    }

    if (bottom > height)
    {
        bottom = height;
    }

    output->x = (int32_t)left;
    output->y = (int32_t)top;
    output->width = (uint32_t)(right - left);
    output->height = (uint32_t)(bottom - top);
    return output->width != 0U && output->height != 0U;
}

static ui_rect_t damage_union(
    const ui_rect_t *first,
    const ui_rect_t *second
)
{
    int32_t left = first->x < second->x ? first->x : second->x;
    int32_t top = first->y < second->y ? first->y : second->y;
    int64_t first_right = (int64_t)first->x + first->width;
    int64_t second_right = (int64_t)second->x + second->width;
    int64_t first_bottom = (int64_t)first->y + first->height;
    int64_t second_bottom = (int64_t)second->y + second->height;
    int64_t right = first_right > second_right ?
        first_right : second_right;
    int64_t bottom = first_bottom > second_bottom ?
        first_bottom : second_bottom;

    ui_rect_t result = {
        .x = left,
        .y = top,
        .width = (uint32_t)(right - left),
        .height = (uint32_t)(bottom - top)
    };

    return result;
}

bool virtio_gpu_present(
    const uint32_t *source,
    uint32_t source_stride,
    const ui_rect_t *rectangles,
    uint32_t rectangle_count
)
{
    if (
        !available ||
        source == NULL ||
        source_stride < statistics.width ||
        rectangles == NULL ||
        rectangle_count == 0U
    )
    {
        return false;
    }

    ui_rect_t transfer_damage = { 0, 0, 0, 0 };
    bool have_damage = false;
    uint32_t maximum = rectangle_count;

    if (maximum > VIRTIO_GPU_MAX_DAMAGE_RECTS)
    {
        maximum = VIRTIO_GPU_MAX_DAMAGE_RECTS;
    }

    for (uint32_t index = 0; index < maximum; index++)
    {
        ui_rect_t rectangle;

        if (!normalize_damage(
            &rectangles[index],
            statistics.width,
            statistics.height,
            &rectangle
        ))
        {
            continue;
        }

        for (uint32_t row = 0; row < rectangle.height; row++)
        {
            const uint32_t *input = &source[
                (uint64_t)(uint32_t)(rectangle.y + (int32_t)row) *
                    source_stride +
                (uint32_t)rectangle.x
            ];

            uint64_t destination_offset =
                ((uint64_t)(uint32_t)(rectangle.y + (int32_t)row) *
                    statistics.width +
                    (uint32_t)rectangle.x) *
                sizeof(uint32_t);

            if (!backing_write(
                scanout_backing,
                scanout_backing_page_count,
                destination_offset,
                input,
                rectangle.width * sizeof(uint32_t)
            ))
            {
                statistics.command_errors++;
                return false;
            }
        }

        statistics.presented_pixels +=
            (uint64_t)rectangle.width * rectangle.height;

        if (!have_damage)
        {
            transfer_damage = rectangle;
            have_damage = true;
        }
        else
        {
            transfer_damage = damage_union(
                &transfer_damage,
                &rectangle
            );
        }
    }

    if (!have_damage)
    {
        return false;
    }

    memory_barrier();

    virtio_gpu_rectangle_t transfer = make_rectangle(
        (uint32_t)transfer_damage.x,
        (uint32_t)transfer_damage.y,
        transfer_damage.width,
        transfer_damage.height
    );

    if (
        !transfer_rectangle(
            VIRTIO_GPU_RESOURCE_SCANOUT,
            &transfer,
            statistics.width
        ) ||
        !flush_rectangle(
            VIRTIO_GPU_RESOURCE_SCANOUT,
            &transfer
        )
    )
    {
        return false;
    }

    statistics.presented_frames++;
    return true;
}

static bool submit_cursor_position(
    uint32_t command_type,
    uint32_t resource_id,
    int32_t x,
    int32_t y
)
{
    if (!available || !statistics.cursor_ready)
    {
        return false;
    }

    if (x < 0)
    {
        x = 0;
    }

    if (y < 0)
    {
        y = 0;
    }

    if (statistics.width != 0U && x >= (int32_t)statistics.width)
    {
        x = (int32_t)statistics.width - 1;
    }

    if (statistics.height != 0U && y >= (int32_t)statistics.height)
    {
        y = (int32_t)statistics.height - 1;
    }

    virtio_gpu_cursor_command_t command;
    memory_clear(&command, sizeof(command));
    prepare_cursor_header(&command.header, command_type);
    command.position.scanout_id = statistics.scanout_id;
    command.position.x = (uint32_t)x;
    command.position.y = (uint32_t)y;
    command.resource_id = resource_id;

    return submit_cursor(&command);
}

bool virtio_gpu_cursor_show(int32_t x, int32_t y)
{
    if (!available || !statistics.cursor_ready)
    {
        return false;
    }

    if (!submit_cursor_position(
        VIRTIO_GPU_CMD_UPDATE_CURSOR,
        VIRTIO_GPU_RESOURCE_CURSOR,
        x,
        y
    ))
    {
        return false;
    }

    cursor_visible = true;
    last_cursor_x = x;
    last_cursor_y = y;
    statistics.cursor_updates++;
    return true;
}

bool virtio_gpu_cursor_move(int32_t x, int32_t y)
{
    if (!cursor_visible)
    {
        return virtio_gpu_cursor_show(x, y);
    }

    if (x == last_cursor_x && y == last_cursor_y)
    {
        return true;
    }

    if (!submit_cursor_position(
        VIRTIO_GPU_CMD_MOVE_CURSOR,
        VIRTIO_GPU_RESOURCE_CURSOR,
        x,
        y
    ))
    {
        return false;
    }

    last_cursor_x = x;
    last_cursor_y = y;
    statistics.cursor_moves++;
    return true;
}

bool virtio_gpu_cursor_hide(void)
{
    if (!available || !statistics.cursor_ready)
    {
        return false;
    }

    if (!cursor_visible)
    {
        return true;
    }

    if (!submit_cursor_position(
        VIRTIO_GPU_CMD_UPDATE_CURSOR,
        0U,
        0,
        0
    ))
    {
        return false;
    }

    cursor_visible = false;
    last_cursor_x = -1;
    last_cursor_y = -1;
    statistics.cursor_updates++;
    return true;
}

bool virtio_gpu_cursor_available(void)
{
    return false;
}

void virtio_gpu_get_stats(virtio_gpu_stats_t *output)
{
    if (output == NULL)
    {
        return;
    }

    *output = statistics;
}

const char *virtio_gpu_status_text(void)
{
    return status_text;
}
