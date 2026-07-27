#include "ahci.h"

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
#define PCI_SUBCLASS_SATA          0x06U
#define PCI_PROGIF_AHCI            0x01U
#define PCI_COMMAND_MEMORY         0x0002U
#define PCI_COMMAND_BUS_MASTER     0x0004U

#define AHCI_MAX_PORTS             32U
#define AHCI_MAX_DEVICES           8U
#define AHCI_MMIO_BYTES            0x2000U
#define AHCI_COMMAND_SLOTS_USED    1U
#define AHCI_SECTOR_SIZE           512U
#define AHCI_DMA_SECTORS           8U
#define AHCI_TIMEOUT               10000000U

#define AHCI_GHC_HR                (1U << 0)
#define AHCI_GHC_IE                (1U << 1)
#define AHCI_GHC_AE                (1U << 31)

#define AHCI_CAP_S64A              (1U << 31)
#define AHCI_CAP2_BOH              (1U << 0)
#define AHCI_BOHC_BOS              (1U << 0)
#define AHCI_BOHC_OOS              (1U << 1)
#define AHCI_BOHC_BB               (1U << 4)

#define AHCI_PORT_CMD_ST           (1U << 0)
#define AHCI_PORT_CMD_FRE          (1U << 4)
#define AHCI_PORT_CMD_FR           (1U << 14)
#define AHCI_PORT_CMD_CR           (1U << 15)

#define AHCI_PORT_TFD_ERR          (1U << 0)
#define AHCI_PORT_TFD_DRQ          (1U << 3)
#define AHCI_PORT_TFD_BSY          (1U << 7)
#define AHCI_PORT_IS_TFES          (1U << 30)

#define AHCI_SSTS_DET_PRESENT      3U
#define AHCI_SSTS_IPM_ACTIVE       1U

#define AHCI_SIG_ATA               0x00000101U
#define AHCI_SIG_ATAPI             0xEB140101U

#define FIS_TYPE_REG_H2D           0x27U

#define ATA_COMMAND_IDENTIFY       0xECU
#define ATA_COMMAND_READ_DMA_EXT   0x25U
#define ATA_COMMAND_WRITE_DMA_EXT  0x35U
#define ATA_COMMAND_FLUSH_EXT      0xEAU

#define AHCI_HEADER_CFL_MASK       0x001FU
#define AHCI_HEADER_WRITE          (1U << 6)
#define AHCI_PRDT_INTERRUPT        (1U << 31)
#define AHCI_PRDT_BYTE_COUNT_MASK  0x003FFFFFU

#define AHCI_TEST_LBA              128U


typedef volatile struct
{
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t devslp;
    uint32_t reserved1[10];
    uint32_t vendor[4];
} ahci_hba_port_t;

typedef volatile struct
{
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint32_t reserved[29];
    uint32_t vendor[24];
    ahci_hba_port_t ports[AHCI_MAX_PORTS];
} ahci_hba_memory_t;

typedef struct __attribute__((packed))
{
    uint16_t flags;
    uint16_t prdt_length;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} ahci_command_header_t;

typedef struct __attribute__((packed))
{
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;
} ahci_prdt_entry_t;

typedef struct __attribute__((packed))
{
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    ahci_prdt_entry_t prdt[1];
} ahci_command_table_t;

typedef struct __attribute__((packed))
{
    uint8_t fis_type;
    uint8_t pmport_c;
    uint8_t command;
    uint8_t feature_low;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t feature_high;
    uint8_t count_low;
    uint8_t count_high;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} ahci_fis_reg_h2d_t;

typedef struct
{
    ahci_hba_port_t *registers;
    uint8_t port_number;
    bool online;
    bool lba48;
    uint64_t sector_count;
    char model[41];
    char name[64];

    uint64_t command_list_physical;
    uint64_t received_fis_physical;
    uint64_t command_table_physical;
    uint64_t dma_physical;

    ahci_command_header_t *command_list;
    void *received_fis;
    ahci_command_table_t *command_table;
    uint8_t *dma_buffer;

    block_device_t block_device;
    uint64_t read_commands;
    uint64_t write_commands;
    uint64_t errors;
} ahci_port_state_t;

typedef struct
{
    const pci_device_t *pci_device;
    ahci_hba_memory_t *hba;
    uint64_t abar_physical;
    uint32_t version;
    uint32_t capabilities;
    bool supports_64_bit;
    bool initialized;
} ahci_controller_t;

static ahci_controller_t controller;
static ahci_port_state_t port_states[AHCI_MAX_DEVICES];
static uint32_t device_count;

static void write_u64_pair(
    volatile uint32_t *low,
    volatile uint32_t *high,
    uint64_t value
)
{
    *low = (uint32_t)value;
    *high = (uint32_t)(value >> 32);
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

static bool wait_clear(
    volatile uint32_t *value,
    uint32_t mask
)
{
    for (
        uint32_t timeout = 0;
        timeout < AHCI_TIMEOUT;
        timeout++
    )
    {
        if ((*value & mask) == 0)
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool port_stop(ahci_hba_port_t *port)
{
    port->cmd &= ~AHCI_PORT_CMD_ST;

    if (!wait_clear(&port->cmd, AHCI_PORT_CMD_CR))
    {
        return false;
    }

    port->cmd &= ~AHCI_PORT_CMD_FRE;

    return wait_clear(&port->cmd, AHCI_PORT_CMD_FR);
}

static bool port_start(ahci_hba_port_t *port)
{
    if (
        !wait_clear(
            &port->cmd,
            AHCI_PORT_CMD_CR |
            AHCI_PORT_CMD_FR
        )
    )
    {
        return false;
    }

    port->cmd |= AHCI_PORT_CMD_FRE;
    port->cmd |= AHCI_PORT_CMD_ST;
    return true;
}

static bool port_present(const ahci_hba_port_t *port)
{
    uint32_t status = port->ssts;
    uint32_t det = status & 0x0FU;
    uint32_t ipm = (status >> 8) & 0x0FU;

    return
        det == AHCI_SSTS_DET_PRESENT &&
        ipm == AHCI_SSTS_IPM_ACTIVE;
}

static bool command_ready(ahci_hba_port_t *port)
{
    for (
        uint32_t timeout = 0;
        timeout < AHCI_TIMEOUT;
        timeout++
    )
    {
        if (
            (port->tfd &
             (AHCI_PORT_TFD_BSY |
              AHCI_PORT_TFD_DRQ)) == 0
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

static bool issue_command(
    ahci_port_state_t *state,
    uint8_t command,
    uint64_t lba,
    uint16_t sector_count,
    bool write,
    uint32_t byte_count
)
{
    if (
        state == NULL ||
        !state->online ||
        byte_count > PAGE_SIZE
    )
    {
        return false;
    }

    ahci_hba_port_t *port = state->registers;

    if (!command_ready(port))
    {
        state->errors++;
        return false;
    }

    ahci_command_header_t *header =
        &state->command_list[0];

    memset(header, 0, sizeof(*header));
    memset(
        state->command_table,
        0,
        sizeof(*state->command_table)
    );

    header->flags =
        (uint16_t)(
            (sizeof(ahci_fis_reg_h2d_t) / 4U) &
            AHCI_HEADER_CFL_MASK
        );

    if (write)
    {
        header->flags |= AHCI_HEADER_WRITE;
    }

    header->prdt_length =
        byte_count > 0 ? 1U : 0U;
    header->ctba =
        (uint32_t)state->command_table_physical;
    header->ctbau =
        (uint32_t)(state->command_table_physical >> 32);

    if (byte_count > 0)
    {
        ahci_prdt_entry_t *prdt =
            &state->command_table->prdt[0];

        prdt->dba = (uint32_t)state->dma_physical;
        prdt->dbau = (uint32_t)(state->dma_physical >> 32);
        prdt->dbc_i =
            ((byte_count - 1U) &
             AHCI_PRDT_BYTE_COUNT_MASK) |
            AHCI_PRDT_INTERRUPT;
    }

    ahci_fis_reg_h2d_t *fis =
        (ahci_fis_reg_h2d_t *)
            state->command_table->cfis;

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80U;
    fis->command = command;

    if (
        command == ATA_COMMAND_READ_DMA_EXT ||
        command == ATA_COMMAND_WRITE_DMA_EXT
    )
    {
        fis->lba0 = (uint8_t)lba;
        fis->lba1 = (uint8_t)(lba >> 8);
        fis->lba2 = (uint8_t)(lba >> 16);
        fis->device = 1U << 6;
        fis->lba3 = (uint8_t)(lba >> 24);
        fis->lba4 = (uint8_t)(lba >> 32);
        fis->lba5 = (uint8_t)(lba >> 40);
        fis->count_low = (uint8_t)sector_count;
        fis->count_high = (uint8_t)(sector_count >> 8);
    }

    port->is = 0xFFFFFFFFU;
    __sync_synchronize();
    port->ci |= 1U;
    __sync_synchronize();

    for (
        uint32_t timeout = 0;
        timeout < AHCI_TIMEOUT;
        timeout++
    )
    {
        if ((port->ci & 1U) == 0)
        {
            if (
                (port->is & AHCI_PORT_IS_TFES) != 0 ||
                (port->tfd & AHCI_PORT_TFD_ERR) != 0
            )
            {
                state->errors++;
                return false;
            }

            return true;
        }

        if ((port->is & AHCI_PORT_IS_TFES) != 0)
        {
            state->errors++;
            return false;
        }

        __asm__ volatile("pause");
    }

    state->errors++;
    return false;
}

static bool issue_flush(ahci_port_state_t *state)
{
    return issue_command(
        state,
        ATA_COMMAND_FLUSH_EXT,
        0,
        0,
        false,
        0
    );
}

static void parse_model(
    ahci_port_state_t *state,
    const uint16_t identify[256]
)
{
    uint32_t output = 0;

    for (uint32_t word = 27; word <= 46; word++)
    {
        state->model[output++] =
            (char)(identify[word] >> 8);
        state->model[output++] =
            (char)(identify[word] & 0xFFU);
    }

    while (
        output > 0 &&
        state->model[output - 1U] == ' '
    )
    {
        output--;
    }

    state->model[output] = '\0';

    if (output == 0)
    {
        const char fallback[] = "AHCI SATA disk";
        uint32_t index = 0;

        while (fallback[index] != '\0')
        {
            state->model[index] = fallback[index];
            index++;
        }

        state->model[index] = '\0';
    }
}

static void build_device_name(ahci_port_state_t *state)
{
    const char prefix[] = "AHCI SATA port";
    uint32_t output = 0;

    while (prefix[output] != '\0')
    {
        state->name[output] = prefix[output];
        output++;
    }

    if (state->port_number >= 10U)
    {
        state->name[output++] =
            (char)('0' + state->port_number / 10U);
    }

    state->name[output++] =
        (char)('0' + state->port_number % 10U);
    state->name[output++] = ':';
    state->name[output++] = ' ';

    uint32_t model_index = 0;

    while (
        state->model[model_index] != '\0' &&
        output + 1U < sizeof(state->name)
    )
    {
        state->name[output++] =
            state->model[model_index++];
    }

    state->name[output] = '\0';
}

static bool identify_port(ahci_port_state_t *state)
{
    if (
        !issue_command(
            state,
            ATA_COMMAND_IDENTIFY,
            0,
            0,
            false,
            AHCI_SECTOR_SIZE
        )
    )
    {
        return false;
    }

    const uint16_t *identify =
        (const uint16_t *)state->dma_buffer;

    state->lba48 =
        (identify[83] & (1U << 10)) != 0;

    if (!state->lba48)
    {
        return false;
    }

    if (state->lba48)
    {
        state->sector_count =
            (uint64_t)identify[100] |
            ((uint64_t)identify[101] << 16) |
            ((uint64_t)identify[102] << 32) |
            ((uint64_t)identify[103] << 48);
    }
    else
    {
        state->sector_count =
            (uint64_t)identify[60] |
            ((uint64_t)identify[61] << 16);
    }

    if (state->sector_count == 0)
    {
        return false;
    }

    parse_model(state, identify);
    build_device_name(state);
    return true;
}

static bool ahci_read_callback(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    void *buffer
)
{
    ahci_port_state_t *state = context;

    if (
        state == NULL ||
        buffer == NULL ||
        sector_count == 0 ||
        lba >= state->sector_count ||
        sector_count > state->sector_count - lba ||
        (!state->lba48 && lba > 0x0FFFFFFFULL)
    )
    {
        return false;
    }

    uint8_t *output = buffer;
    uint32_t remaining = sector_count;

    while (remaining > 0)
    {
        uint16_t chunk =
            remaining > AHCI_DMA_SECTORS ?
                AHCI_DMA_SECTORS :
                (uint16_t)remaining;

        uint32_t bytes =
            (uint32_t)chunk * AHCI_SECTOR_SIZE;

        if (
            !issue_command(
                state,
                ATA_COMMAND_READ_DMA_EXT,
                lba,
                chunk,
                false,
                bytes
            )
        )
        {
            return false;
        }

        memcpy(output, state->dma_buffer, bytes);
        output += bytes;
        lba += chunk;
        remaining -= chunk;
        state->read_commands++;
    }

    return true;
}

static bool ahci_write_callback(
    void *context,
    uint64_t lba,
    uint32_t sector_count,
    const void *buffer
)
{
    ahci_port_state_t *state = context;

    if (
        state == NULL ||
        buffer == NULL ||
        sector_count == 0 ||
        lba >= state->sector_count ||
        sector_count > state->sector_count - lba ||
        (!state->lba48 && lba > 0x0FFFFFFFULL)
    )
    {
        return false;
    }

    const uint8_t *input = buffer;
    uint32_t remaining = sector_count;

    while (remaining > 0)
    {
        uint16_t chunk =
            remaining > AHCI_DMA_SECTORS ?
                AHCI_DMA_SECTORS :
                (uint16_t)remaining;

        uint32_t bytes =
            (uint32_t)chunk * AHCI_SECTOR_SIZE;

        memcpy(state->dma_buffer, input, bytes);

        if (
            !issue_command(
                state,
                ATA_COMMAND_WRITE_DMA_EXT,
                lba,
                chunk,
                true,
                bytes
            )
        )
        {
            return false;
        }

        input += bytes;
        lba += chunk;
        remaining -= chunk;
        state->write_commands++;
    }

    return issue_flush(state);
}

static bool configure_port(
    ahci_port_state_t *state,
    ahci_hba_port_t *port,
    uint8_t port_number
)
{
    memset(state, 0, sizeof(*state));
    state->registers = port;
    state->port_number = port_number;

    if (!port_stop(port))
    {
        return false;
    }

    void *command_list_virtual = NULL;
    void *command_table_virtual = NULL;
    void *dma_virtual = NULL;

    if (
        !allocate_dma_page(
            &state->command_list_physical,
            &command_list_virtual
        )
    )
    {
        return false;
    }

    state->command_list = command_list_virtual;

    if (
        !allocate_dma_page(
            &state->received_fis_physical,
            &state->received_fis
        ) ||
        !allocate_dma_page(
            &state->command_table_physical,
            &command_table_virtual
        ) ||
        !allocate_dma_page(
            &state->dma_physical,
            &dma_virtual
        )
    )
    {
        return false;
    }

    state->command_table = command_table_virtual;
    state->dma_buffer = dma_virtual;

    write_u64_pair(
        &port->clb,
        &port->clbu,
        state->command_list_physical
    );

    write_u64_pair(
        &port->fb,
        &port->fbu,
        state->received_fis_physical
    );

    ahci_command_header_t *header =
        &state->command_list[0];

    header->ctba =
        (uint32_t)state->command_table_physical;
    header->ctbau =
        (uint32_t)(state->command_table_physical >> 32);

    port->serr = 0xFFFFFFFFU;
    port->is = 0xFFFFFFFFU;
    port->ie = 0;

    state->online = true;

    if (!port_start(port) || !identify_port(state))
    {
        state->online = false;
        (void)port_stop(port);
        return false;
    }

    state->block_device.name = state->name;
    state->block_device.sector_size = AHCI_SECTOR_SIZE;
    state->block_device.sector_count = state->sector_count;
    state->block_device.writable = true;
    state->block_device.context = state;
    state->block_device.read = ahci_read_callback;
    state->block_device.write = ahci_write_callback;

    return true;
}

static bool bios_handoff(void)
{
    if ((controller.hba->cap2 & AHCI_CAP2_BOH) == 0)
    {
        return true;
    }

    controller.hba->bohc |= AHCI_BOHC_OOS;

    for (
        uint32_t timeout = 0;
        timeout < AHCI_TIMEOUT;
        timeout++
    )
    {
        uint32_t bohc = controller.hba->bohc;

        if (
            (bohc & AHCI_BOHC_BOS) == 0 &&
            (bohc & AHCI_BOHC_BB) == 0
        )
        {
            return true;
        }

        __asm__ volatile("pause");
    }

    return false;
}

bool ahci_init(void)
{
    memset(&controller, 0, sizeof(controller));
    memset(port_states, 0, sizeof(port_states));
    device_count = 0;

    const pci_device_t *device = NULL;

    for (uint32_t occurrence = 0; ; occurrence++)
    {
        const pci_device_t *candidate =
            pci_find_class(
                PCI_CLASS_STORAGE,
                PCI_SUBCLASS_SATA,
                occurrence
            );

        if (candidate == NULL)
        {
            break;
        }

        if (
            candidate->programming_interface ==
            PCI_PROGIF_AHCI
        )
        {
            device = candidate;
            break;
        }
    }

    if (device == NULL)
    {
        return false;
    }

    uint32_t bar = device->bars[5];

    if ((bar & 1U) != 0)
    {
        return false;
    }

    uint64_t abar_physical =
        (uint64_t)(bar & 0xFFFFFFF0U);

    if (
        abar_physical == 0 ||
        !map_mmio_range(
            abar_physical,
            AHCI_MMIO_BYTES
        )
    )
    {
        return false;
    }

    pci_set_command_bits(
        device,
        PCI_COMMAND_MEMORY |
        PCI_COMMAND_BUS_MASTER
    );

    controller.pci_device = device;
    controller.abar_physical = abar_physical;
    controller.hba =
        (ahci_hba_memory_t *)
            physical_to_virtual(abar_physical);

    if (!bios_handoff())
    {
        return false;
    }

    controller.hba->ghc |= AHCI_GHC_AE;
    controller.hba->ghc &= ~AHCI_GHC_IE;

    controller.version = controller.hba->vs;
    controller.capabilities = controller.hba->cap;
    controller.supports_64_bit =
        (controller.capabilities & AHCI_CAP_S64A) != 0;

    uint32_t implemented = controller.hba->pi;

    for (
        uint8_t port_number = 0;
        port_number < AHCI_MAX_PORTS &&
        device_count < AHCI_MAX_DEVICES;
        port_number++
    )
    {
        if ((implemented & (1U << port_number)) == 0)
        {
            continue;
        }

        ahci_hba_port_t *port =
            &controller.hba->ports[port_number];

        if (
            !port_present(port) ||
            port->sig != AHCI_SIG_ATA
        )
        {
            continue;
        }

        ahci_port_state_t *state =
            &port_states[device_count];

        if (!configure_port(state, port, port_number))
        {
            continue;
        }

        if (!block_device_register(&state->block_device))
        {
            state->online = false;
            break;
        }

        device_count++;
    }

    controller.initialized = device_count > 0;
    return controller.initialized;
}

bool ahci_available(void)
{
    return controller.initialized;
}

uint32_t ahci_device_count(void)
{
    return device_count;
}

const block_device_t *ahci_block_device(uint32_t index)
{
    if (index >= device_count)
    {
        return NULL;
    }

    return &port_states[index].block_device;
}

void ahci_print_status(void)
{
    if (!controller.initialized)
    {
        kprintf("AHCI: not initialized\n");
        return;
    }

    kprintf(
        "AHCI: PCI %02x:%02x.%u ABAR=%llx version=%x.%02x ports=%u 64-bit=%s\n",
        (unsigned int)controller.pci_device->bus,
        (unsigned int)controller.pci_device->device,
        (unsigned int)controller.pci_device->function,
        (unsigned long long)controller.abar_physical,
        (unsigned int)(controller.version >> 16),
        (unsigned int)(controller.version & 0xFFFFU),
        (unsigned int)device_count,
        controller.supports_64_bit ? "yes" : "no"
    );

    for (uint32_t index = 0; index < device_count; index++)
    {
        const ahci_port_state_t *state = &port_states[index];

        kprintf(
            "[%u] port=%u model=%s sectors=%llu capacity=%llu MiB LBA48=%s\n",
            (unsigned int)index,
            (unsigned int)state->port_number,
            state->model,
            (unsigned long long)state->sector_count,
            (unsigned long long)(
                state->sector_count * AHCI_SECTOR_SIZE /
                (1024ULL * 1024ULL)
            ),
            state->lba48 ? "yes" : "no"
        );

        kprintf(
            "    reads=%llu writes=%llu errors=%llu\n",
            (unsigned long long)state->read_commands,
            (unsigned long long)state->write_commands,
            (unsigned long long)state->errors
        );
    }
}

bool ahci_run_self_test(void)
{
    if (device_count == 0)
    {
        return false;
    }

    ahci_port_state_t *state = &port_states[0];

    if (state->sector_count <= AHCI_TEST_LBA)
    {
        return false;
    }

    uint8_t original[AHCI_SECTOR_SIZE];
    uint8_t pattern[AHCI_SECTOR_SIZE];
    uint8_t verify[AHCI_SECTOR_SIZE];

    if (
        !ahci_read_callback(
            state,
            AHCI_TEST_LBA,
            1,
            original
        )
    )
    {
        return false;
    }

    for (uint32_t index = 0; index < AHCI_SECTOR_SIZE; index++)
    {
        pattern[index] =
            (uint8_t)(index ^ 0x5AU);
        verify[index] = 0;
    }

    static const char signature[] =
        "LatterOS AHCI DMA read/write self-test";

    for (
        uint32_t index = 0;
        signature[index] != '\0';
        index++
    )
    {
        pattern[index] = (uint8_t)signature[index];
    }

    bool passed =
        ahci_write_callback(
            state,
            AHCI_TEST_LBA,
            1,
            pattern
        ) &&
        ahci_read_callback(
            state,
            AHCI_TEST_LBA,
            1,
            verify
        ) &&
        memcmp(pattern, verify, AHCI_SECTOR_SIZE) == 0;

    bool restored =
        ahci_write_callback(
            state,
            AHCI_TEST_LBA,
            1,
            original
        );

    return passed && restored;
}
