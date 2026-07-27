#include "nvme.h"

#include "hhdm.h"
#include "kstdio.h"
#include "memory.h"
#include "page_allocator.h"
#include "paging.h"
#include "pci.h"
#include "physical_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_CLASS_STORAGE          0x01U
#define PCI_SUBCLASS_NVM           0x08U
#define PCI_PROGIF_NVME            0x02U
#define PCI_COMMAND_MEMORY         0x0002U
#define PCI_COMMAND_BUS_MASTER     0x0004U

#define NVME_MMIO_BYTES            0x4000U
#define NVME_ADMIN_QUEUE_DEPTH     16U
#define NVME_IO_QUEUE_DEPTH        32U
#define NVME_MAX_NAMESPACES        4U
#define NVME_PAGE_BYTES            4096U
#define NVME_TEST_LBA              128U
#define NVME_POLL_LIMIT            50000000U

#define NVME_CC_EN                 (1U << 0)
#define NVME_CC_CSS_NVM            (0U << 4)
#define NVME_CC_MPS_4K             (0U << 7)
#define NVME_CC_AMS_RR             (0U << 11)
#define NVME_CC_SHN_NONE           (0U << 14)
#define NVME_CC_IOSQES_64          (6U << 16)
#define NVME_CC_IOCQES_16          (4U << 20)

#define NVME_CSTS_RDY              (1U << 0)
#define NVME_CSTS_CFS              (1U << 1)

#define NVME_ADMIN_DELETE_IO_SQ    0x00U
#define NVME_ADMIN_CREATE_IO_SQ    0x01U
#define NVME_ADMIN_GET_LOG_PAGE    0x02U
#define NVME_ADMIN_DELETE_IO_CQ    0x04U
#define NVME_ADMIN_CREATE_IO_CQ    0x05U
#define NVME_ADMIN_IDENTIFY        0x06U
#define NVME_ADMIN_SET_FEATURES    0x09U

#define NVME_FEATURE_NUM_QUEUES    0x07U

#define NVME_NVM_FLUSH             0x00U
#define NVME_NVM_WRITE             0x01U
#define NVME_NVM_READ              0x02U

#define NVME_IDENTIFY_NAMESPACE    0x00U
#define NVME_IDENTIFY_CONTROLLER   0x01U

#define NVME_QUEUE_PHYS_CONTIGUOUS (1U << 0)
#define NVME_CQ_INTERRUPT_ENABLE   (1U << 1)

#define NVME_STATUS_PHASE          0x0001U
#define NVME_STATUS_CODE_MASK      0xFFFEU


typedef volatile struct
{
    uint64_t cap;
    uint32_t vs;
    uint32_t intms;
    uint32_t intmc;
    uint32_t cc;
    uint32_t reserved0;
    uint32_t csts;
    uint32_t nssr;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;
    uint32_t cmbloc;
    uint32_t cmbsz;
    uint32_t bpinfo;
    uint32_t bprsel;
    uint64_t bpmbl;
    uint64_t cmbmsc;
    uint32_t cmbsts;
    uint32_t reserved1[885];
    uint32_t doorbells[];
} nvme_registers_t;

typedef struct __attribute__((packed))
{
    uint8_t opcode;
    uint8_t flags;
    uint16_t command_id;
    uint32_t namespace_id;
    uint64_t reserved0;
    uint64_t metadata_pointer;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_submission_t;

typedef struct __attribute__((packed))
{
    uint32_t result;
    uint32_t reserved;
    uint16_t submission_head;
    uint16_t submission_id;
    uint16_t command_id;
    uint16_t status;
} nvme_completion_t;

typedef struct
{
    nvme_submission_t *submission;
    volatile nvme_completion_t *completion;
    uint64_t submission_physical;
    uint64_t completion_physical;
    uint16_t depth;
    uint16_t submission_tail;
    uint16_t completion_head;
    uint16_t next_command_id;
    uint8_t completion_phase;
    uint16_t queue_id;
} nvme_queue_t;

typedef struct
{
    uint32_t namespace_id;
    uint32_t sector_size;
    uint64_t sector_count;
    char name[64];
    block_device_t block_device;
    uint64_t read_commands;
    uint64_t write_commands;
    uint64_t flush_commands;
    uint64_t errors;
} nvme_namespace_t;

typedef struct
{
    const pci_device_t *pci_device;
    nvme_registers_t *registers;
    uint64_t bar_physical;
    uint32_t version;
    uint32_t doorbell_stride;
    uint16_t maximum_queue_entries;
    uint8_t timeout_units;
    uint8_t mdts;
    uint32_t controller_namespace_count;
    char serial[21];
    char model[41];
    char firmware[9];

    nvme_queue_t admin_queue;
    nvme_queue_t io_queue;

    uint64_t identify_physical;
    uint8_t *identify_buffer;
    uint64_t io_physical;
    uint8_t *io_buffer;

    bool initialized;
} nvme_controller_t;

static nvme_controller_t controller;
static nvme_namespace_t namespaces[NVME_MAX_NAMESPACES];
static uint32_t namespace_count;
static uint8_t self_test_original[NVME_PAGE_BYTES]
    __attribute__((aligned(64)));
static uint8_t self_test_pattern[NVME_PAGE_BYTES]
    __attribute__((aligned(64)));
static uint8_t self_test_verify[NVME_PAGE_BYTES]
    __attribute__((aligned(64)));

static uint32_t read_u32(const uint8_t *buffer)
{
    return
        (uint32_t)buffer[0] |
        ((uint32_t)buffer[1] << 8) |
        ((uint32_t)buffer[2] << 16) |
        ((uint32_t)buffer[3] << 24);
}

static uint64_t read_u64(const uint8_t *buffer)
{
    return
        (uint64_t)read_u32(buffer) |
        ((uint64_t)read_u32(buffer + 4) << 32);
}

static void trim_ascii(
    char *destination,
    uint32_t capacity,
    const uint8_t *source,
    uint32_t length
)
{
    if (destination == NULL || capacity == 0)
    {
        return;
    }

    while (length > 0 && source[length - 1] == ' ')
    {
        length--;
    }

    uint32_t count = length;

    if (count >= capacity)
    {
        count = capacity - 1U;
    }

    for (uint32_t index = 0; index < count; index++)
    {
        uint8_t value = source[index];
        destination[index] =
            (value >= 32U && value <= 126U) ?
                (char)value : '?';
    }

    destination[count] = '\0';
}

static void append_uint(
    char *text,
    uint32_t capacity,
    uint32_t *length,
    uint32_t value
)
{
    char reverse[11];
    uint32_t count = 0;

    if (value == 0)
    {
        if (*length + 1U < capacity)
        {
            text[(*length)++] = '0';
        }
        return;
    }

    while (value > 0 && count < sizeof(reverse))
    {
        reverse[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }

    while (count > 0 && *length + 1U < capacity)
    {
        text[(*length)++] = reverse[--count];
    }
}

static void build_namespace_name(
    nvme_namespace_t *namespace_state
)
{
    const char prefix[] = "NVMe namespace ";
    uint32_t length = 0;

    while (
        prefix[length] != '\0' &&
        length + 1U < sizeof(namespace_state->name)
    )
    {
        namespace_state->name[length] = prefix[length];
        length++;
    }

    append_uint(
        namespace_state->name,
        sizeof(namespace_state->name),
        &length,
        namespace_state->namespace_id
    );

    namespace_state->name[length] = '\0';
}

static bool map_mmio_range(
    uint64_t physical_address,
    uint32_t byte_count
)
{
    uint64_t first =
        physical_address &
        ~(uint64_t)(PAGE_SIZE - 1U);

    uint64_t last =
        (physical_address + byte_count + PAGE_SIZE - 1U) &
        ~(uint64_t)(PAGE_SIZE - 1U);

    for (
        uint64_t physical = first;
        physical < last;
        physical += PAGE_SIZE
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

static bool allocate_dma_page(
    uint64_t *physical,
    void **virtual_address
)
{
    void *page = alloc_page();

    if (page == NULL)
    {
        return false;
    }

    *physical = (uint64_t)page;
    *virtual_address = physical_to_virtual(*physical);
    memset(*virtual_address, 0, PAGE_SIZE);
    return true;
}

static bool wait_ready(bool expected)
{
    uint32_t limit = NVME_POLL_LIMIT;

    if (controller.timeout_units != 0)
    {
        uint64_t scaled =
            (uint64_t)controller.timeout_units * 5000000ULL;

        if (scaled < limit)
        {
            limit = (uint32_t)scaled;
        }
    }

    for (uint32_t spin = 0; spin < limit; spin++)
    {
        uint32_t status = controller.registers->csts;

        if (status & NVME_CSTS_CFS)
        {
            return false;
        }

        bool ready = (status & NVME_CSTS_RDY) != 0;

        if (ready == expected)
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static volatile uint32_t *submission_doorbell(
    uint16_t queue_id
)
{
    uint64_t offset =
        0x1000ULL +
        (uint64_t)(2U * queue_id) *
            controller.doorbell_stride;

    return (volatile uint32_t *)(
        (uint8_t *)controller.registers + offset
    );
}

static volatile uint32_t *completion_doorbell(
    uint16_t queue_id
)
{
    uint64_t offset =
        0x1000ULL +
        (uint64_t)(2U * queue_id + 1U) *
            controller.doorbell_stride;

    return (volatile uint32_t *)(
        (uint8_t *)controller.registers + offset
    );
}

static void queue_reset(
    nvme_queue_t *queue,
    uint16_t queue_id,
    uint16_t depth
)
{
    queue->depth = depth;
    queue->submission_tail = 0;
    queue->completion_head = 0;
    queue->next_command_id = 1;
    queue->completion_phase = 1;
    queue->queue_id = queue_id;

    memset(
        queue->submission,
        0,
        (size_t)depth * sizeof(nvme_submission_t)
    );

    memset(
        (void *)queue->completion,
        0,
        (size_t)depth * sizeof(nvme_completion_t)
    );
}

static bool queue_allocate(
    nvme_queue_t *queue,
    uint16_t queue_id,
    uint16_t requested_depth
)
{
    void *submission_virtual = NULL;
    void *completion_virtual = NULL;

    if (
        !allocate_dma_page(
            &queue->submission_physical,
            &submission_virtual
        ) ||
        !allocate_dma_page(
            &queue->completion_physical,
            &completion_virtual
        )
    )
    {
        return false;
    }

    uint16_t maximum_submission =
        (uint16_t)(
            PAGE_SIZE / sizeof(nvme_submission_t)
        );

    uint16_t maximum_completion =
        (uint16_t)(
            PAGE_SIZE / sizeof(nvme_completion_t)
        );

    uint16_t depth = requested_depth;

    if (depth > controller.maximum_queue_entries)
    {
        depth = controller.maximum_queue_entries;
    }

    if (depth > maximum_submission)
    {
        depth = maximum_submission;
    }

    if (depth > maximum_completion)
    {
        depth = maximum_completion;
    }

    if (depth < 2U)
    {
        return false;
    }

    queue->submission = submission_virtual;
    queue->completion = completion_virtual;
    queue_reset(queue, queue_id, depth);
    return true;
}

static bool queue_submit(
    nvme_queue_t *queue,
    const nvme_submission_t *command,
    uint32_t *result
)
{
    if (
        queue == NULL ||
        command == NULL ||
        queue->submission == NULL ||
        queue->completion == NULL
    )
    {
        return false;
    }

    uint16_t command_id = queue->next_command_id++;

    if (queue->next_command_id == 0)
    {
        queue->next_command_id = 1;
    }

    nvme_submission_t submitted = *command;
    submitted.command_id = command_id;

    queue->submission[queue->submission_tail] = submitted;

    __atomic_thread_fence(__ATOMIC_RELEASE);

    queue->submission_tail++;

    if (queue->submission_tail >= queue->depth)
    {
        queue->submission_tail = 0;
    }

    *submission_doorbell(queue->queue_id) =
        queue->submission_tail;

    for (uint32_t spin = 0; spin < NVME_POLL_LIMIT; spin++)
    {
        nvme_completion_t completion =
            queue->completion[queue->completion_head];

        uint8_t phase =
            (uint8_t)(completion.status & NVME_STATUS_PHASE);

        if (phase != queue->completion_phase)
        {
            __asm__ volatile("pause");
            continue;
        }

        if (completion.command_id != command_id)
        {
            return false;
        }

        if (result != NULL)
        {
            *result = completion.result;
        }

        bool successful =
            (completion.status & NVME_STATUS_CODE_MASK) == 0;

        queue->completion_head++;

        if (queue->completion_head >= queue->depth)
        {
            queue->completion_head = 0;
            queue->completion_phase ^= 1U;
        }

        __atomic_thread_fence(__ATOMIC_RELEASE);
        *completion_doorbell(queue->queue_id) =
            queue->completion_head;

        return successful;
    }

    return false;
}

static bool identify(
    uint32_t namespace_id,
    uint8_t cns
)
{
    memset(controller.identify_buffer, 0, PAGE_SIZE);

    nvme_submission_t command;
    memset(&command, 0, sizeof(command));

    command.opcode = NVME_ADMIN_IDENTIFY;
    command.namespace_id = namespace_id;
    command.prp1 = controller.identify_physical;
    command.cdw10 = cns;

    return queue_submit(
        &controller.admin_queue,
        &command,
        NULL
    );
}

static bool set_queue_count(void)
{
    nvme_submission_t command;
    memset(&command, 0, sizeof(command));

    command.opcode = NVME_ADMIN_SET_FEATURES;
    command.cdw10 = NVME_FEATURE_NUM_QUEUES;
    command.cdw11 = 0;

    uint32_t result = 0;

    if (!queue_submit(
        &controller.admin_queue,
        &command,
        &result
    ))
    {
        return false;
    }

    uint16_t submission_queues =
        (uint16_t)((result & 0xFFFFU) + 1U);

    uint16_t completion_queues =
        (uint16_t)(((result >> 16) & 0xFFFFU) + 1U);

    return
        submission_queues >= 1U &&
        completion_queues >= 1U;
}

static bool create_io_queues(void)
{
    nvme_submission_t command;
    memset(&command, 0, sizeof(command));

    command.opcode = NVME_ADMIN_CREATE_IO_CQ;
    command.prp1 = controller.io_queue.completion_physical;
    command.cdw10 =
        ((uint32_t)(controller.io_queue.depth - 1U) << 16) |
        controller.io_queue.queue_id;
    command.cdw11 = NVME_QUEUE_PHYS_CONTIGUOUS;

    if (!queue_submit(
        &controller.admin_queue,
        &command,
        NULL
    ))
    {
        return false;
    }

    memset(&command, 0, sizeof(command));
    command.opcode = NVME_ADMIN_CREATE_IO_SQ;
    command.prp1 = controller.io_queue.submission_physical;
    command.cdw10 =
        ((uint32_t)(controller.io_queue.depth - 1U) << 16) |
        controller.io_queue.queue_id;
    command.cdw11 =
        ((uint32_t)controller.io_queue.queue_id << 16) |
        NVME_QUEUE_PHYS_CONTIGUOUS;

    return queue_submit(
        &controller.admin_queue,
        &command,
        NULL
    );
}

static bool issue_io(
    nvme_namespace_t *namespace_state,
    uint8_t opcode,
    uint64_t lba,
    uint32_t sector_count,
    bool write
)
{
    if (
        namespace_state == NULL ||
        sector_count == 0 ||
        sector_count > 0x10000U ||
        lba >= namespace_state->sector_count ||
        sector_count >
            namespace_state->sector_count - lba
    )
    {
        return false;
    }

    uint64_t bytes =
        (uint64_t)sector_count *
        namespace_state->sector_size;

    if (bytes > PAGE_SIZE)
    {
        return false;
    }

    nvme_submission_t command;
    memset(&command, 0, sizeof(command));

    command.opcode = opcode;
    command.namespace_id =
        namespace_state->namespace_id;
    command.prp1 = controller.io_physical;
    command.cdw10 = (uint32_t)lba;
    command.cdw11 = (uint32_t)(lba >> 32);
    command.cdw12 = sector_count - 1U;

    if (!queue_submit(
        &controller.io_queue,
        &command,
        NULL
    ))
    {
        namespace_state->errors++;
        return false;
    }

    if (write)
    {
        namespace_state->write_commands++;
    }
    else
    {
        namespace_state->read_commands++;
    }

    return true;
}

static bool issue_flush(
    nvme_namespace_t *namespace_state
)
{
    nvme_submission_t command;
    memset(&command, 0, sizeof(command));

    command.opcode = NVME_NVM_FLUSH;
    command.namespace_id =
        namespace_state->namespace_id;

    if (!queue_submit(
        &controller.io_queue,
        &command,
        NULL
    ))
    {
        namespace_state->errors++;
        return false;
    }

    namespace_state->flush_commands++;
    return true;
}

static bool namespace_read_callback(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
)
{
    nvme_namespace_t *namespace_state = context;

    if (
        namespace_state == NULL ||
        buffer == NULL ||
        sector_count == 0
    )
    {
        return false;
    }

    uint32_t maximum_sectors =
        PAGE_SIZE / namespace_state->sector_size;

    uint8_t *destination = buffer;

    while (sector_count > 0)
    {
        uint32_t batch = sector_count;

        if (batch > maximum_sectors)
        {
            batch = maximum_sectors;
        }

        if (!issue_io(
            namespace_state,
            NVME_NVM_READ,
            lba,
            batch,
            false
        ))
        {
            return false;
        }

        uint32_t byte_count =
            batch * namespace_state->sector_size;

        memcpy(
            destination,
            controller.io_buffer,
            byte_count
        );

        destination += byte_count;
        lba += batch;
        sector_count -= batch;
    }

    return true;
}

static bool namespace_write_callback(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
)
{
    nvme_namespace_t *namespace_state = context;

    if (
        namespace_state == NULL ||
        buffer == NULL ||
        sector_count == 0
    )
    {
        return false;
    }

    uint32_t maximum_sectors =
        PAGE_SIZE / namespace_state->sector_size;

    const uint8_t *source = buffer;

    while (sector_count > 0)
    {
        uint32_t batch = sector_count;

        if (batch > maximum_sectors)
        {
            batch = maximum_sectors;
        }

        uint32_t byte_count =
            batch * namespace_state->sector_size;

        memcpy(
            controller.io_buffer,
            source,
            byte_count
        );

        __atomic_thread_fence(__ATOMIC_RELEASE);

        if (!issue_io(
            namespace_state,
            NVME_NVM_WRITE,
            lba,
            batch,
            true
        ))
        {
            return false;
        }

        source += byte_count;
        lba += batch;
        sector_count -= batch;
    }

    return issue_flush(namespace_state);
}

static bool register_namespace(uint32_t namespace_id)
{
    if (
        namespace_count >= NVME_MAX_NAMESPACES ||
        !identify(
            namespace_id,
            NVME_IDENTIFY_NAMESPACE
        )
    )
    {
        return false;
    }

    const uint8_t *data = controller.identify_buffer;
    uint64_t sector_count = read_u64(data + 0);
    uint8_t formatted_index = data[26] & 0x0FU;
    uint8_t lba_format_count = (uint8_t)(data[25] + 1U);

    if (
        sector_count == 0 ||
        formatted_index >= lba_format_count ||
        formatted_index >= 16U
    )
    {
        return false;
    }

    uint32_t format_offset =
        128U + (uint32_t)formatted_index * 4U;

    uint8_t lba_shift = data[format_offset + 2U];

    if (lba_shift < 9U || lba_shift > 12U)
    {
        return false;
    }

    uint32_t sector_size = 1U << lba_shift;

    if (
        sector_size > PAGE_SIZE ||
        PAGE_SIZE % sector_size != 0
    )
    {
        return false;
    }

    nvme_namespace_t *namespace_state =
        &namespaces[namespace_count];

    memset(namespace_state, 0, sizeof(*namespace_state));
    namespace_state->namespace_id = namespace_id;
    namespace_state->sector_size = sector_size;
    namespace_state->sector_count = sector_count;
    build_namespace_name(namespace_state);

    namespace_state->block_device.name =
        namespace_state->name;
    namespace_state->block_device.sector_size =
        sector_size;
    namespace_state->block_device.sector_count =
        sector_count;
    namespace_state->block_device.writable = true;
    namespace_state->block_device.context =
        namespace_state;
    namespace_state->block_device.read =
        namespace_read_callback;
    namespace_state->block_device.write =
        namespace_write_callback;

    if (!block_device_register(
        &namespace_state->block_device
    ))
    {
        memset(namespace_state, 0, sizeof(*namespace_state));
        return false;
    }

    namespace_count++;
    return true;
}

static bool controller_disable(void)
{
    controller.registers->cc &= ~NVME_CC_EN;
    return wait_ready(false);
}

static bool controller_enable(void)
{
    controller.registers->cc =
        NVME_CC_CSS_NVM |
        NVME_CC_MPS_4K |
        NVME_CC_AMS_RR |
        NVME_CC_SHN_NONE |
        NVME_CC_IOSQES_64 |
        NVME_CC_IOCQES_16 |
        NVME_CC_EN;

    return wait_ready(true);
}

bool nvme_init(void)
{
    memset(&controller, 0, sizeof(controller));
    memset(namespaces, 0, sizeof(namespaces));
    namespace_count = 0;

    const pci_device_t *device =
        pci_find_class(
            PCI_CLASS_STORAGE,
            PCI_SUBCLASS_NVM,
            0
        );

    if (
        device == NULL ||
        device->programming_interface !=
            PCI_PROGIF_NVME
    )
    {
        return false;
    }

    uint32_t bar0 = device->bars[0];

    if (
        (bar0 & 0x1U) != 0 ||
        (bar0 & 0xFFFFFFF0U) == 0
    )
    {
        return false;
    }

    uint64_t bar_physical =
        (uint64_t)(bar0 & 0xFFFFFFF0U);

    if ((bar0 & 0x6U) == 0x4U)
    {
        bar_physical |=
            (uint64_t)device->bars[1] << 32;
    }

    if (!map_mmio_range(
        bar_physical,
        NVME_MMIO_BYTES
    ))
    {
        return false;
    }

    pci_set_command_bits(
        device,
        PCI_COMMAND_MEMORY |
        PCI_COMMAND_BUS_MASTER
    );

    controller.pci_device = device;
    controller.bar_physical = bar_physical;
    controller.registers =
        physical_to_virtual(bar_physical);
    controller.version = controller.registers->vs;

    uint64_t capabilities = controller.registers->cap;
    controller.maximum_queue_entries =
        (uint16_t)((capabilities & 0xFFFFU) + 1U);
    controller.timeout_units =
        (uint8_t)((capabilities >> 24) & 0xFFU);

    uint8_t doorbell_stride_shift =
        (uint8_t)((capabilities >> 32) & 0x0FU);

    controller.doorbell_stride =
        4U << doorbell_stride_shift;

    uint8_t minimum_page_shift =
        (uint8_t)((capabilities >> 48) & 0x0FU);

    if (minimum_page_shift != 0)
    {
        return false;
    }

    if (!controller_disable())
    {
        return false;
    }

    if (
        !queue_allocate(
            &controller.admin_queue,
            0,
            NVME_ADMIN_QUEUE_DEPTH
        ) ||
        !queue_allocate(
            &controller.io_queue,
            1,
            NVME_IO_QUEUE_DEPTH
        ) ||
        !allocate_dma_page(
            &controller.identify_physical,
            (void **)&controller.identify_buffer
        ) ||
        !allocate_dma_page(
            &controller.io_physical,
            (void **)&controller.io_buffer
        )
    )
    {
        return false;
    }

    controller.registers->aqa =
        ((uint32_t)(controller.admin_queue.depth - 1U) << 16) |
        (controller.admin_queue.depth - 1U);
    controller.registers->asq =
        controller.admin_queue.submission_physical;
    controller.registers->acq =
        controller.admin_queue.completion_physical;

    if (!controller_enable())
    {
        return false;
    }

    queue_reset(
        &controller.admin_queue,
        0,
        controller.admin_queue.depth
    );

    if (!identify(0, NVME_IDENTIFY_CONTROLLER))
    {
        return false;
    }

    const uint8_t *controller_data =
        controller.identify_buffer;

    trim_ascii(
        controller.serial,
        sizeof(controller.serial),
        controller_data + 4,
        20
    );

    trim_ascii(
        controller.model,
        sizeof(controller.model),
        controller_data + 24,
        40
    );

    trim_ascii(
        controller.firmware,
        sizeof(controller.firmware),
        controller_data + 64,
        8
    );

    controller.mdts = controller_data[77];
    controller.controller_namespace_count =
        read_u32(controller_data + 516);

    if (
        !set_queue_count() ||
        !create_io_queues()
    )
    {
        return false;
    }

    uint32_t count =
        controller.controller_namespace_count;

    if (count > NVME_MAX_NAMESPACES)
    {
        count = NVME_MAX_NAMESPACES;
    }

    for (
        uint32_t namespace_id = 1;
        namespace_id <= count;
        namespace_id++
    )
    {
        (void)register_namespace(namespace_id);
    }

    controller.initialized = namespace_count > 0;
    return controller.initialized;
}

bool nvme_available(void)
{
    return controller.initialized;
}

uint32_t nvme_namespace_count(void)
{
    return namespace_count;
}

const block_device_t *nvme_block_device(uint32_t index)
{
    if (index >= namespace_count)
    {
        return NULL;
    }

    return &namespaces[index].block_device;
}

void nvme_print_status(void)
{
    if (!controller.initialized)
    {
        kprintf("NVMe: controller unavailable\n");
        return;
    }

    kprintf(
        "NVMe: PCI %02x:%02x.%x BAR=0x%llX version=%u.%u.%u queues=%u namespaces=%u\n",
        (unsigned int)controller.pci_device->bus,
        (unsigned int)controller.pci_device->device,
        (unsigned int)controller.pci_device->function,
        (unsigned long long)controller.bar_physical,
        (unsigned int)((controller.version >> 16) & 0xFFFFU),
        (unsigned int)((controller.version >> 8) & 0xFFU),
        (unsigned int)(controller.version & 0xFFU),
        (unsigned int)controller.io_queue.depth,
        (unsigned int)namespace_count
    );

    kprintf(
        "Controller: model=%s serial=%s firmware=%s MDTS=%u\n",
        controller.model[0] != '\0' ? controller.model : "unknown",
        controller.serial[0] != '\0' ? controller.serial : "unknown",
        controller.firmware[0] != '\0' ? controller.firmware : "unknown",
        (unsigned int)controller.mdts
    );

    for (uint32_t index = 0; index < namespace_count; index++)
    {
        const nvme_namespace_t *namespace_state =
            &namespaces[index];

        uint64_t capacity_mib =
            namespace_state->sector_count *
            namespace_state->sector_size /
            (1024ULL * 1024ULL);

        kprintf(
            "[%u] NSID=%u sectors=%llu block=%u capacity=%llu MiB writable=yes\n",
            (unsigned int)index,
            (unsigned int)namespace_state->namespace_id,
            (unsigned long long)namespace_state->sector_count,
            (unsigned int)namespace_state->sector_size,
            (unsigned long long)capacity_mib
        );

        kprintf(
            "    reads=%llu writes=%llu flushes=%llu errors=%llu\n",
            (unsigned long long)namespace_state->read_commands,
            (unsigned long long)namespace_state->write_commands,
            (unsigned long long)namespace_state->flush_commands,
            (unsigned long long)namespace_state->errors
        );
    }
}

bool nvme_run_self_test(void)
{
    if (
        !controller.initialized ||
        namespace_count == 0
    )
    {
        return false;
    }

    nvme_namespace_t *namespace_state = &namespaces[0];

    if (
        namespace_state->sector_size > PAGE_SIZE ||
        NVME_TEST_LBA >= namespace_state->sector_count
    )
    {
        return false;
    }

    uint32_t bytes = namespace_state->sector_size;

    if (!namespace_read_callback(
        namespace_state,
        NVME_TEST_LBA,
        1,
        self_test_original
    ))
    {
        return false;
    }

    for (uint32_t index = 0; index < bytes; index++)
    {
        self_test_pattern[index] = (uint8_t)(
            0xA5U ^
            (uint8_t)index ^
            (uint8_t)(index >> 8)
        );
    }

    bool wrote = namespace_write_callback(
        namespace_state,
        NVME_TEST_LBA,
        1,
        self_test_pattern
    );

    bool read_back = false;
    bool matches = false;

    if (wrote)
    {
        read_back = namespace_read_callback(
            namespace_state,
            NVME_TEST_LBA,
            1,
            self_test_verify
        );
    }

    if (read_back)
    {
        matches = true;

        for (uint32_t index = 0; index < bytes; index++)
        {
            if (self_test_verify[index] != self_test_pattern[index])
            {
                matches = false;
                break;
            }
        }
    }

    bool restored = namespace_write_callback(
        namespace_state,
        NVME_TEST_LBA,
        1,
        self_test_original
    );

    return wrote && read_back && matches && restored;
}
