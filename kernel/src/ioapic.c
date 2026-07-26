#include "ioapic.h"

#include "acpi.h"
#include "hhdm.h"
#include "paging.h"
#include "physical_memory.h"

#include <stddef.h>
#include <stdint.h>

#define IOAPIC_REGISTER_SELECT 0x00
#define IOAPIC_REGISTER_WINDOW 0x10

#define IOAPIC_REGISTER_ID      0x00
#define IOAPIC_REGISTER_VERSION 0x01
#define IOAPIC_REDIRECTION_BASE 0x10

#define IOAPIC_MAX_CONTROLLERS  ACPI_MAX_IOAPICS
#define IOAPIC_MAX_ISA_IRQS     16

#define IOAPIC_REDIR_ACTIVE_LOW (1ULL << 13)
#define IOAPIC_REDIR_LEVEL      (1ULL << 15)
#define IOAPIC_REDIR_MASKED     (1ULL << 16)

typedef struct
{
    volatile uint32_t *base;
    uint8_t id;
    uint32_t gsi_base;
    uint32_t redirection_count;
} ioapic_controller_t;

static ioapic_controller_t controllers[
    IOAPIC_MAX_CONTROLLERS
];

static uint32_t controller_count;
static uint32_t total_redirections;
static uint32_t destination_id;
static bool ready;

static bool map_mmio_page(uint64_t physical_address)
{
    uint64_t physical_page =
        physical_address &
        ~(uint64_t)(PAGE_SIZE - 1);

    uint64_t virtual_page =
        (uint64_t)physical_to_virtual(
            physical_page
        );

    if (paging_is_mapped(virtual_page))
    {
        return true;
    }

    return paging_map_kernel_page(
        virtual_page,
        physical_page,
        true
    );
}

static uint32_t ioapic_read(
    const ioapic_controller_t *controller,
    uint8_t reg
)
{
    controller->base[
        IOAPIC_REGISTER_SELECT / sizeof(uint32_t)
    ] = reg;

    return controller->base[
        IOAPIC_REGISTER_WINDOW / sizeof(uint32_t)
    ];
}

static void ioapic_write(
    const ioapic_controller_t *controller,
    uint8_t reg,
    uint32_t value
)
{
    controller->base[
        IOAPIC_REGISTER_SELECT / sizeof(uint32_t)
    ] = reg;

    controller->base[
        IOAPIC_REGISTER_WINDOW / sizeof(uint32_t)
    ] = value;
}

static uint64_t ioapic_read_redirection(
    const ioapic_controller_t *controller,
    uint32_t index
)
{
    uint8_t low_register =
        (uint8_t)(
            IOAPIC_REDIRECTION_BASE +
            index * 2U
        );

    uint32_t low =
        ioapic_read(controller, low_register);

    uint32_t high =
        ioapic_read(
            controller,
            (uint8_t)(low_register + 1U)
        );

    return
        ((uint64_t)high << 32) |
        (uint64_t)low;
}

static void ioapic_write_redirection(
    const ioapic_controller_t *controller,
    uint32_t index,
    uint64_t value
)
{
    uint8_t low_register =
        (uint8_t)(
            IOAPIC_REDIRECTION_BASE +
            index * 2U
        );

    /*
     * Program the high dword first so an old destination cannot
     * observe a newly unmasked low dword during the update.
     */
    ioapic_write(
        controller,
        (uint8_t)(low_register + 1U),
        (uint32_t)(value >> 32)
    );

    ioapic_write(
        controller,
        low_register,
        (uint32_t)value
    );
}

static ioapic_controller_t *controller_for_gsi(
    uint32_t gsi,
    uint32_t *entry_index
)
{
    for (
        uint32_t index = 0;
        index < controller_count;
        index++
    )
    {
        ioapic_controller_t *controller =
            &controllers[index];

        uint32_t end =
            controller->gsi_base +
            controller->redirection_count;

        if (
            gsi >= controller->gsi_base &&
            gsi < end
        )
        {
            if (entry_index != NULL)
            {
                *entry_index =
                    gsi - controller->gsi_base;
            }

            return controller;
        }
    }

    return NULL;
}

static void mask_all_entries(void)
{
    for (
        uint32_t controller_index = 0;
        controller_index < controller_count;
        controller_index++
    )
    {
        ioapic_controller_t *controller =
            &controllers[controller_index];

        for (
            uint32_t entry = 0;
            entry < controller->redirection_count;
            entry++
        )
        {
            uint64_t value =
                ioapic_read_redirection(
                    controller,
                    entry
                );

            value |= IOAPIC_REDIR_MASKED;

            ioapic_write_redirection(
                controller,
                entry,
                value
            );
        }
    }
}

bool ioapic_init(uint32_t destination_apic_id)
{
    ready = false;
    controller_count = 0;
    total_redirections = 0;
    destination_id = destination_apic_id;

    if (
        !acpi_is_available() ||
        destination_id > 0xFFU
    )
    {
        return false;
    }

    uint32_t discovered =
        acpi_ioapic_count();

    if (discovered > IOAPIC_MAX_CONTROLLERS)
    {
        discovered = IOAPIC_MAX_CONTROLLERS;
    }

    for (
        uint32_t index = 0;
        index < discovered;
        index++
    )
    {
        const acpi_ioapic_info_t *info =
            acpi_ioapic(index);

        if (
            info == NULL ||
            info->address == 0
        )
        {
            continue;
        }

        if (!map_mmio_page(info->address))
        {
            continue;
        }

        ioapic_controller_t *controller =
            &controllers[controller_count];

        controller->base =
            (volatile uint32_t *)physical_to_virtual(
                info->address
            );
        controller->id = info->id;
        controller->gsi_base = info->gsi_base;

        uint32_t version =
            ioapic_read(
                controller,
                IOAPIC_REGISTER_VERSION
            );

        controller->redirection_count =
            ((version >> 16) & 0xFFU) + 1U;

        total_redirections +=
            controller->redirection_count;

        controller_count++;
    }

    if (controller_count == 0)
    {
        return false;
    }

    mask_all_entries();

    ready = true;
    return true;
}

bool ioapic_is_ready(void)
{
    return ready;
}

uint32_t ioapic_controller_count(void)
{
    return controller_count;
}

uint32_t ioapic_redirection_count(void)
{
    return total_redirections;
}

bool ioapic_route_isa_irq(
    uint8_t irq,
    uint8_t vector,
    bool masked
)
{
    if (
        !ready ||
        irq >= IOAPIC_MAX_ISA_IRQS ||
        vector < 32
    )
    {
        return false;
    }

    uint32_t gsi;
    bool active_low;
    bool level_triggered;

    if (
        !acpi_resolve_isa_irq(
            irq,
            &gsi,
            &active_low,
            &level_triggered
        )
    )
    {
        return false;
    }

    uint32_t entry_index;
    ioapic_controller_t *controller =
        controller_for_gsi(
            gsi,
            &entry_index
        );

    if (controller == NULL)
    {
        return false;
    }

    uint64_t redirection = vector;

    if (active_low)
    {
        redirection |= IOAPIC_REDIR_ACTIVE_LOW;
    }

    if (level_triggered)
    {
        redirection |= IOAPIC_REDIR_LEVEL;
    }

    if (masked)
    {
        redirection |= IOAPIC_REDIR_MASKED;
    }

    redirection |=
        (uint64_t)(destination_id & 0xFFU) << 56;

    ioapic_write_redirection(
        controller,
        entry_index,
        redirection
    );

    return true;
}

bool ioapic_mask_isa_irq(uint8_t irq)
{
    if (
        !ready ||
        irq >= IOAPIC_MAX_ISA_IRQS
    )
    {
        return false;
    }

    uint32_t gsi;
    bool active_low;
    bool level_triggered;

    if (
        !acpi_resolve_isa_irq(
            irq,
            &gsi,
            &active_low,
            &level_triggered
        )
    )
    {
        return false;
    }

    (void)active_low;
    (void)level_triggered;

    uint32_t entry_index;
    ioapic_controller_t *controller =
        controller_for_gsi(
            gsi,
            &entry_index
        );

    if (controller == NULL)
    {
        return false;
    }

    uint64_t redirection =
        ioapic_read_redirection(
            controller,
            entry_index
        );

    redirection |= IOAPIC_REDIR_MASKED;

    ioapic_write_redirection(
        controller,
        entry_index,
        redirection
    );

    return true;
}
