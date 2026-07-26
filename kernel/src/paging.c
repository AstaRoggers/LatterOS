#include "paging.h"

#include "hhdm.h"
#include "page_allocator.h"
#include "physical_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_NX       (1ULL << 63)

#define PAGE_ADDRESS_MASK \
    0x000FFFFFFFFFF000ULL

#define USER_CANONICAL_LIMIT \
    0x0000800000000000ULL

#define IA32_EFER_MSR 0xC0000080U
#define IA32_EFER_NXE (1ULL << 11)
#define CPUID_EXTENDED_MAX 0x80000000U
#define CPUID_EXTENDED_FEATURES 0x80000001U
#define CPUID_EDX_NX (1U << 20)

static uint64_t kernel_root_physical;
static bool initialized;
static bool nx_enabled;

static void clear_page(void *page)
{
    uint64_t *words = page;

    for (
        uint32_t index = 0;
        index < PAGE_SIZE / sizeof(uint64_t);
        index++
    )
    {
        words[index] = 0;
    }
}

static uint64_t read_cr3(void)
{
    uint64_t value;

    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(value)
    );

    return value & PAGE_ADDRESS_MASK;
}

static void write_cr3(uint64_t value)
{
    __asm__ volatile(
        "mov %0, %%cr3"
        :
        : "r"(value & PAGE_ADDRESS_MASK)
        : "memory"
    );
}

static void cpuid(
    uint32_t leaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx
)
{
    uint32_t a = leaf;
    uint32_t b;
    uint32_t c = 0;
    uint32_t d;

    __asm__ volatile(
        "cpuid"
        : "+a"(a),
          "=b"(b),
          "+c"(c),
          "=d"(d)
    );

    if (eax != NULL)
    {
        *eax = a;
    }

    if (ebx != NULL)
    {
        *ebx = b;
    }

    if (ecx != NULL)
    {
        *ecx = c;
    }

    if (edx != NULL)
    {
        *edx = d;
    }
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile(
        "rdmsr"
        : "=a"(low),
          "=d"(high)
        : "c"(msr)
    );

    return
        ((uint64_t)high << 32) |
        (uint64_t)low;
}

static void write_msr(
    uint32_t msr,
    uint64_t value
)
{
    __asm__ volatile(
        "wrmsr"
        :
        : "c"(msr),
          "a"((uint32_t)value),
          "d"((uint32_t)(value >> 32))
        : "memory"
    );
}

static void initialize(void)
{
    if (initialized)
    {
        return;
    }

    kernel_root_physical = read_cr3();
    nx_enabled = false;

    uint32_t maximum_extended;
    cpuid(
        CPUID_EXTENDED_MAX,
        &maximum_extended,
        NULL,
        NULL,
        NULL
    );

    if (
        maximum_extended >=
        CPUID_EXTENDED_FEATURES
    )
    {
        uint32_t features_edx;

        cpuid(
            CPUID_EXTENDED_FEATURES,
            NULL,
            NULL,
            NULL,
            &features_edx
        );

        if (features_edx & CPUID_EDX_NX)
        {
            uint64_t efer =
                read_msr(IA32_EFER_MSR);

            efer |= IA32_EFER_NXE;

            write_msr(
                IA32_EFER_MSR,
                efer
            );

            nx_enabled = true;
        }
    }

    initialized = true;
}

static uint64_t *root_virtual(
    uint64_t root_physical
)
{
    return physical_to_virtual(
        root_physical & PAGE_ADDRESS_MASK
    );
}

static uint64_t *table_from_entry(
    uint64_t entry
)
{
    return physical_to_virtual(
        entry & PAGE_ADDRESS_MASK
    );
}

static uint64_t *next_table(
    uint64_t *table,
    uint16_t index,
    bool create,
    bool user
)
{
    uint64_t entry = table[index];

    if (entry & PAGE_PRESENT)
    {
        if (entry & PAGE_HUGE)
        {
            return NULL;
        }

        if (create)
        {
            table[index] |= PAGE_WRITABLE;

            if (user)
            {
                table[index] |= PAGE_USER;
            }
        }

        return table_from_entry(
            table[index]
        );
    }

    if (!create)
    {
        return NULL;
    }

    void *physical_page = alloc_page();

    if (physical_page == NULL)
    {
        return NULL;
    }

    uint64_t *new_table =
        physical_to_virtual(
            (uint64_t)physical_page
        );

    clear_page(new_table);

    uint64_t flags =
        PAGE_PRESENT |
        PAGE_WRITABLE;

    if (user)
    {
        flags |= PAGE_USER;
    }

    table[index] =
        ((uint64_t)physical_page &
            PAGE_ADDRESS_MASK) |
        flags;

    return new_table;
}

static bool map_page_in(
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable,
    bool user,
    bool executable
)
{
    initialize();

    if (
        root_physical == 0 ||
        (virtual_address & (PAGE_SIZE - 1)) != 0 ||
        (physical_address & (PAGE_SIZE - 1)) != 0
    )
    {
        return false;
    }

    uint16_t pml4_index =
        (uint16_t)((virtual_address >> 39) & 0x1FF);

    uint16_t pdpt_index =
        (uint16_t)((virtual_address >> 30) & 0x1FF);

    uint16_t pd_index =
        (uint16_t)((virtual_address >> 21) & 0x1FF);

    uint16_t pt_index =
        (uint16_t)((virtual_address >> 12) & 0x1FF);

    uint64_t *pml4 =
        root_virtual(root_physical);

    uint64_t *pdpt =
        next_table(
            pml4,
            pml4_index,
            true,
            user
        );

    if (pdpt == NULL)
    {
        return false;
    }

    uint64_t *pd =
        next_table(
            pdpt,
            pdpt_index,
            true,
            user
        );

    if (pd == NULL)
    {
        return false;
    }

    uint64_t *pt =
        next_table(
            pd,
            pd_index,
            true,
            user
        );

    if (pt == NULL)
    {
        return false;
    }

    if (pt[pt_index] & PAGE_PRESENT)
    {
        return false;
    }

    uint64_t flags = PAGE_PRESENT;

    if (writable)
    {
        flags |= PAGE_WRITABLE;
    }

    if (user)
    {
        flags |= PAGE_USER;
    }

    if (
        nx_enabled &&
        !executable
    )
    {
        flags |= PAGE_NX;
    }

    pt[pt_index] =
        (physical_address & PAGE_ADDRESS_MASK) |
        flags;

    __asm__ volatile(
        "invlpg (%0)"
        :
        : "r"(virtual_address)
        : "memory"
    );

    return true;
}

static bool lookup_leaf(
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t **leaf,
    bool require_user
)
{
    if (root_physical == 0)
    {
        return false;
    }

    uint16_t pml4_index =
        (uint16_t)((virtual_address >> 39) & 0x1FF);

    uint16_t pdpt_index =
        (uint16_t)((virtual_address >> 30) & 0x1FF);

    uint16_t pd_index =
        (uint16_t)((virtual_address >> 21) & 0x1FF);

    uint16_t pt_index =
        (uint16_t)((virtual_address >> 12) & 0x1FF);

    uint64_t *pml4 =
        root_virtual(root_physical);

    uint64_t pml4_entry =
        pml4[pml4_index];

    if (
        !(pml4_entry & PAGE_PRESENT) ||
        (require_user &&
         !(pml4_entry & PAGE_USER))
    )
    {
        return false;
    }

    uint64_t *pdpt =
        table_from_entry(pml4_entry);

    uint64_t pdpt_entry =
        pdpt[pdpt_index];

    if (
        !(pdpt_entry & PAGE_PRESENT) ||
        (pdpt_entry & PAGE_HUGE) ||
        (require_user &&
         !(pdpt_entry & PAGE_USER))
    )
    {
        return false;
    }

    uint64_t *pd =
        table_from_entry(pdpt_entry);

    uint64_t pd_entry =
        pd[pd_index];

    if (
        !(pd_entry & PAGE_PRESENT) ||
        (pd_entry & PAGE_HUGE) ||
        (require_user &&
         !(pd_entry & PAGE_USER))
    )
    {
        return false;
    }

    uint64_t *pt =
        table_from_entry(pd_entry);

    uint64_t *entry =
        &pt[pt_index];

    if (
        !(*entry & PAGE_PRESENT) ||
        (require_user &&
         !(*entry & PAGE_USER))
    )
    {
        return false;
    }

    if (leaf != NULL)
    {
        *leaf = entry;
    }

    return true;
}

static void free_table_tree(
    uint64_t table_physical,
    uint32_t level
)
{
    uint64_t *table =
        root_virtual(table_physical);

    if (level > 1)
    {
        for (
            uint32_t index = 0;
            index < 512;
            index++
        )
        {
            uint64_t entry = table[index];

            if (
                !(entry & PAGE_PRESENT) ||
                (entry & PAGE_HUGE)
            )
            {
                continue;
            }

            free_table_tree(
                entry & PAGE_ADDRESS_MASK,
                level - 1
            );
        }
    }

    free_page(
        (void *)(table_physical &
            PAGE_ADDRESS_MASK)
    );
}

uint64_t paging_kernel_root(void)
{
    initialize();
    return kernel_root_physical;
}

uint64_t paging_current_root(void)
{
    initialize();
    return read_cr3();
}

bool paging_nx_enabled(void)
{
    initialize();
    return nx_enabled;
}

uint64_t paging_create_user_space(void)
{
    initialize();

    void *physical_page = alloc_page();

    if (physical_page == NULL)
    {
        return 0;
    }

    uint64_t root_physical =
        (uint64_t)physical_page;

    uint64_t *new_root =
        root_virtual(root_physical);

    uint64_t *kernel_root =
        root_virtual(kernel_root_physical);

    clear_page(new_root);

    for (
        uint32_t index = 256;
        index < 512;
        index++
    )
    {
        new_root[index] =
            kernel_root[index];
    }

    return root_physical;
}

void paging_destroy_user_space(
    uint64_t root_physical
)
{
    initialize();

    root_physical &= PAGE_ADDRESS_MASK;

    if (
        root_physical == 0 ||
        root_physical == kernel_root_physical ||
        root_physical == read_cr3()
    )
    {
        return;
    }

    uint64_t *root =
        root_virtual(root_physical);

    for (
        uint32_t index = 0;
        index < 256;
        index++
    )
    {
        uint64_t entry = root[index];

        if (
            !(entry & PAGE_PRESENT) ||
            (entry & PAGE_HUGE)
        )
        {
            continue;
        }

        free_table_tree(
            entry & PAGE_ADDRESS_MASK,
            3
        );

        root[index] = 0;
    }

    free_page((void *)root_physical);
}

void paging_activate(uint64_t root_physical)
{
    initialize();

    root_physical &= PAGE_ADDRESS_MASK;

    if (
        root_physical != 0 &&
        root_physical != read_cr3()
    )
    {
        write_cr3(root_physical);
    }
}

bool paging_map_user_page_in(
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable,
    bool executable
)
{
    return map_page_in(
        root_physical,
        virtual_address,
        physical_address,
        writable,
        true,
        executable
    );
}

bool paging_map_user_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable
)
{
    return paging_map_user_page_in(
        paging_current_root(),
        virtual_address,
        physical_address,
        writable,
        true
    );
}

bool paging_map_kernel_page(
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable
)
{
    return map_page_in(
        paging_kernel_root(),
        virtual_address,
        physical_address,
        writable,
        false,
        false
    );
}

bool paging_unmap_page_in(
    uint64_t root_physical,
    uint64_t virtual_address
)
{
    if (
        (virtual_address & (PAGE_SIZE - 1)) != 0
    )
    {
        return false;
    }

    uint64_t *entry;

    if (
        !lookup_leaf(
            root_physical,
            virtual_address,
            &entry,
            false
        )
    )
    {
        return false;
    }

    *entry = 0;

    __asm__ volatile(
        "invlpg (%0)"
        :
        : "r"(virtual_address)
        : "memory"
    );

    return true;
}

bool paging_unmap_page(
    uint64_t virtual_address
)
{
    return paging_unmap_page_in(
        paging_current_root(),
        virtual_address
    );
}

bool paging_is_mapped_in(
    uint64_t root_physical,
    uint64_t virtual_address
)
{
    return lookup_leaf(
        root_physical,
        virtual_address,
        NULL,
        false
    );
}

bool paging_is_mapped(
    uint64_t virtual_address
)
{
    return paging_is_mapped_in(
        paging_current_root(),
        virtual_address
    );
}

bool paging_user_range_valid(
    uint64_t root_physical,
    uint64_t address,
    size_t size,
    bool writable
)
{
    if (
        root_physical == 0 ||
        address == 0 ||
        size == 0 ||
        address >= USER_CANONICAL_LIMIT ||
        size - 1 > UINT64_MAX - address
    )
    {
        return false;
    }

    uint64_t final_address =
        address + size - 1;

    if (final_address >= USER_CANONICAL_LIMIT)
    {
        return false;
    }

    uint64_t page =
        address &
        ~(uint64_t)(PAGE_SIZE - 1);

    uint64_t final_page =
        final_address &
        ~(uint64_t)(PAGE_SIZE - 1);

    for (;;)
    {
        uint64_t *entry;

        if (
            !lookup_leaf(
                root_physical,
                page,
                &entry,
                true
            ) ||
            (writable &&
             !(*entry & PAGE_WRITABLE))
        )
        {
            return false;
        }

        if (page == final_page)
        {
            break;
        }

        if (page > UINT64_MAX - PAGE_SIZE)
        {
            return false;
        }

        page += PAGE_SIZE;
    }

    return true;
}
