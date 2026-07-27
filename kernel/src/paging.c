#include "paging.h"

#include "hhdm.h"
#include "kstdio.h"
#include "lapic.h"
#include "page_allocator.h"
#include "physical_memory.h"
#include "smp.h"
#include "spinlock.h"

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

#define PAGING_ROOT_TRANSITION UINT64_MAX
#define PAGING_SHOOTDOWN_SPIN_LIMIT 5000000ULL
#define PAGING_SELF_TEST_ADDRESS 0xFFFFFE0000200000ULL

#define PAGING_SHOOTDOWN_PAGE 1U
#define PAGING_SHOOTDOWN_FULL 2U

typedef struct
{
    volatile uint64_t request_generation;
    volatile uint64_t acknowledged_generation;
    uint64_t root_physical;
    uint64_t virtual_address;
    uint32_t operation;
    uint32_t reserved;
} paging_mailbox_t;

static uint64_t kernel_root_physical;
static bool initialized;
static bool nx_enabled;

static spinlock_t paging_lock =
    SPINLOCK_INITIALIZER;

static spinlock_t shootdown_lock =
    SPINLOCK_INITIALIZER;

static volatile uint64_t active_roots[SMP_MAX_CPUS];
static paging_mailbox_t mailboxes[SMP_MAX_CPUS];
static uint64_t next_shootdown_generation;

static volatile uint64_t map_operations;
static volatile uint64_t unmap_operations;
static volatile uint64_t user_spaces_created;
static volatile uint64_t user_spaces_destroyed;
static volatile uint64_t shootdown_requests;
static volatile uint64_t shootdown_ipis;
static volatile uint64_t local_invalidations;
static volatile uint64_t remote_invalidations;
static volatile uint64_t shootdown_failures;
static volatile uint64_t destroy_wait_failures;
static volatile uint32_t self_test_status;

static uint64_t interrupt_state(void)
{
    uint64_t flags;

    __asm__ volatile(
        "pushfq\n"
        "popq %0"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void interrupt_disable(void)
{
    __asm__ volatile(
        "cli"
        :
        :
        : "memory"
    );
}

static void interrupt_restore(uint64_t flags)
{
    if ((flags & (1ULL << 9)) != 0)
    {
        __asm__ volatile(
            "sti"
            :
            :
            : "memory"
        );
    }
}

static void cpu_relax(void)
{
    __asm__ volatile(
        "pause"
        :
        :
        : "memory"
    );
}

static void clear_bytes(
    void *pointer,
    size_t count
)
{
    uint8_t *bytes =
        (uint8_t *)pointer;

    for (
        size_t index = 0;
        index < count;
        index++
    )
    {
        bytes[index] = 0;
    }
}

static void clear_page(void *page)
{
    uint64_t *words =
        (uint64_t *)page;

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

static void invalidate_page(uint64_t virtual_address)
{
    __asm__ volatile(
        "invlpg (%0)"
        :
        : "r"(virtual_address)
        : "memory"
    );
}

static void invalidate_all(void)
{
    write_cr3(read_cr3());
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
    if (__atomic_load_n(
            &initialized,
            __ATOMIC_ACQUIRE
        ))
    {
        return;
    }

    spinlock_init(&paging_lock);
    spinlock_init(&shootdown_lock);

    clear_bytes(
        (void *)active_roots,
        sizeof(active_roots)
    );

    clear_bytes(
        mailboxes,
        sizeof(mailboxes)
    );

    kernel_root_physical = read_cr3();
    active_roots[0] = kernel_root_physical;
    next_shootdown_generation = 1;
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

        if ((features_edx & CPUID_EDX_NX) != 0)
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

    __atomic_store_n(
        &initialized,
        true,
        __ATOMIC_RELEASE
    );
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

static uint64_t *next_table_locked(
    uint64_t *table,
    uint16_t index,
    bool create,
    bool user
)
{
    uint64_t entry = table[index];

    if ((entry & PAGE_PRESENT) != 0)
    {
        if ((entry & PAGE_HUGE) != 0)
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

static bool lookup_leaf_locked(
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
        (pml4_entry & PAGE_PRESENT) == 0 ||
        (require_user &&
         (pml4_entry & PAGE_USER) == 0)
    )
    {
        return false;
    }

    uint64_t *pdpt =
        table_from_entry(pml4_entry);

    uint64_t pdpt_entry =
        pdpt[pdpt_index];

    if (
        (pdpt_entry & PAGE_PRESENT) == 0 ||
        (pdpt_entry & PAGE_HUGE) != 0 ||
        (require_user &&
         (pdpt_entry & PAGE_USER) == 0)
    )
    {
        return false;
    }

    uint64_t *pd =
        table_from_entry(pdpt_entry);

    uint64_t pd_entry =
        pd[pd_index];

    if (
        (pd_entry & PAGE_PRESENT) == 0 ||
        (pd_entry & PAGE_HUGE) != 0 ||
        (require_user &&
         (pd_entry & PAGE_USER) == 0)
    )
    {
        return false;
    }

    uint64_t *pt =
        table_from_entry(pd_entry);

    uint64_t *entry =
        &pt[pt_index];

    if (
        (*entry & PAGE_PRESENT) == 0 ||
        (require_user &&
         (*entry & PAGE_USER) == 0)
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

static bool cpu_online(uint32_t cpu_index)
{
    const smp_cpu_t *cpu =
        smp_cpu(cpu_index);

    if (cpu == NULL)
    {
        return false;
    }

    smp_cpu_state_t state =
        (smp_cpu_state_t)__atomic_load_n(
            &cpu->state,
            __ATOMIC_ACQUIRE
        );

    return (
        state == SMP_CPU_ONLINE ||
        state == SMP_CPU_PARKED ||
        state == SMP_CPU_WORKER
    );
}

static bool cpu_needs_invalidation(
    uint32_t cpu_index,
    uint64_t root_physical
)
{
    if (!cpu_online(cpu_index))
    {
        return false;
    }

    if (root_physical == kernel_root_physical)
    {
        return true;
    }

    uint64_t active =
        __atomic_load_n(
            &active_roots[cpu_index],
            __ATOMIC_ACQUIRE
        );

    return (
        active == root_physical ||
        active == PAGING_ROOT_TRANSITION
    );
}

static void invalidate_if_current(
    uint64_t root_physical,
    uint64_t virtual_address,
    uint32_t operation
)
{
    uint64_t current_root = read_cr3();

    if (
        root_physical != kernel_root_physical &&
        current_root != root_physical
    )
    {
        return;
    }

    if (operation == PAGING_SHOOTDOWN_FULL)
    {
        invalidate_all();
    }
    else
    {
        invalidate_page(virtual_address);
    }
}

static bool perform_shootdown(
    uint64_t root_physical,
    uint64_t virtual_address,
    uint32_t operation
)
{
    initialize();

    root_physical &= PAGE_ADDRESS_MASK;

    if (
        root_physical == 0 ||
        (operation != PAGING_SHOOTDOWN_PAGE &&
         operation != PAGING_SHOOTDOWN_FULL)
    )
    {
        return false;
    }

    while (!spinlock_try_lock(&shootdown_lock))
    {
        /*
         * A caller may already be inside an interrupt gate with IF=0.
         * Poll its mailbox while waiting so another CPU can complete a
         * shootdown without depending on nested interrupt delivery.
         */
        paging_handle_tlb_ipi();
        cpu_relax();
    }

    uint64_t interrupt_flags =
        interrupt_state();

    interrupt_disable();

    uint32_t current_cpu =
        smp_current_cpu_index();

    uint64_t generation =
        next_shootdown_generation++;

    if (generation == 0)
    {
        generation =
            next_shootdown_generation++;
    }

    __atomic_add_fetch(
        &shootdown_requests,
        1,
        __ATOMIC_RELAXED
    );

    if (
        root_physical == kernel_root_physical ||
        read_cr3() == root_physical
    )
    {
        invalidate_if_current(
            root_physical,
            virtual_address,
            operation
        );

        __atomic_add_fetch(
            &local_invalidations,
            1,
            __ATOMIC_RELAXED
        );
    }

    bool success = true;
    uint64_t target_mask = 0;
    uint32_t cpu_count = smp_cpu_count();

    if (cpu_count > SMP_MAX_CPUS)
    {
        cpu_count = SMP_MAX_CPUS;
    }

    for (
        uint32_t cpu_index = 0;
        cpu_index < cpu_count;
        cpu_index++
    )
    {
        if (
            cpu_index == current_cpu ||
            !cpu_needs_invalidation(
                cpu_index,
                root_physical
            )
        )
        {
            continue;
        }

        const smp_cpu_t *cpu =
            smp_cpu(cpu_index);

        if (cpu == NULL)
        {
            continue;
        }

        paging_mailbox_t *mailbox =
            &mailboxes[cpu_index];

        mailbox->root_physical =
            root_physical;

        mailbox->virtual_address =
            virtual_address;

        mailbox->operation = operation;

        __atomic_store_n(
            &mailbox->request_generation,
            generation,
            __ATOMIC_RELEASE
        );

        if (!lapic_send_ipi(
                cpu->apic_id,
                PAGING_TLB_IPI_VECTOR
            ))
        {
            success = false;

            __atomic_add_fetch(
                &shootdown_failures,
                1,
                __ATOMIC_RELAXED
            );

            continue;
        }

        if (cpu_index < 64)
        {
            target_mask |=
                1ULL << cpu_index;
        }

        __atomic_add_fetch(
            &shootdown_ipis,
            1,
            __ATOMIC_RELAXED
        );
    }

    for (
        uint32_t cpu_index = 0;
        cpu_index < cpu_count;
        cpu_index++
    )
    {
        if (
            cpu_index >= 64 ||
            (target_mask &
                (1ULL << cpu_index)) == 0
        )
        {
            continue;
        }

        paging_mailbox_t *mailbox =
            &mailboxes[cpu_index];

        uint64_t spin = 0;

        while (
            __atomic_load_n(
                &mailbox->acknowledged_generation,
                __ATOMIC_ACQUIRE
            ) != generation &&
            spin < PAGING_SHOOTDOWN_SPIN_LIMIT
        )
        {
            cpu_relax();
            spin++;
        }

        if (
            __atomic_load_n(
                &mailbox->acknowledged_generation,
                __ATOMIC_ACQUIRE
            ) != generation
        )
        {
            success = false;

            __atomic_add_fetch(
                &shootdown_failures,
                1,
                __ATOMIC_RELAXED
            );
        }
    }

    spinlock_unlock(&shootdown_lock);
    interrupt_restore(interrupt_flags);

    return success;
}

static bool map_page_locked(
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable,
    bool user,
    bool executable
)
{
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
        next_table_locked(
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
        next_table_locked(
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
        next_table_locked(
            pd,
            pd_index,
            true,
            user
        );

    if (pt == NULL)
    {
        return false;
    }

    if ((pt[pt_index] & PAGE_PRESENT) != 0)
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

    return true;
}

static void free_table_tree_locked(
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
                (entry & PAGE_PRESENT) == 0 ||
                (entry & PAGE_HUGE) != 0
            )
            {
                continue;
            }

            free_table_tree_locked(
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

static bool root_is_active(
    uint64_t root_physical
)
{
    uint32_t cpu_count = smp_cpu_count();

    if (cpu_count == 0)
    {
        cpu_count = 1;
    }

    if (cpu_count > SMP_MAX_CPUS)
    {
        cpu_count = SMP_MAX_CPUS;
    }

    for (
        uint32_t cpu_index = 0;
        cpu_index < cpu_count;
        cpu_index++
    )
    {
        uint64_t active =
            __atomic_load_n(
                &active_roots[cpu_index],
                __ATOMIC_ACQUIRE
            );

        if (
            active == root_physical ||
            active == PAGING_ROOT_TRANSITION
        )
        {
            return true;
        }
    }

    return false;
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

    uint64_t flags =
        spinlock_lock_irqsave(
            &paging_lock
        );

    void *physical_page = alloc_page();

    if (physical_page == NULL)
    {
        spinlock_unlock_irqrestore(
            &paging_lock,
            flags
        );

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

    spinlock_unlock_irqrestore(
        &paging_lock,
        flags
    );

    __atomic_add_fetch(
        &user_spaces_created,
        1,
        __ATOMIC_RELAXED
    );

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
        root_physical == kernel_root_physical
    )
    {
        return;
    }

    uint64_t spin = 0;

    while (
        root_is_active(root_physical) &&
        spin < PAGING_SHOOTDOWN_SPIN_LIMIT
    )
    {
        paging_handle_tlb_ipi();
        cpu_relax();
        spin++;
    }

    if (root_is_active(root_physical))
    {
        __atomic_add_fetch(
            &destroy_wait_failures,
            1,
            __ATOMIC_RELAXED
        );

        return;
    }

    (void)perform_shootdown(
        root_physical,
        0,
        PAGING_SHOOTDOWN_FULL
    );

    uint64_t flags =
        spinlock_lock_irqsave(
            &paging_lock
        );

    if (root_is_active(root_physical))
    {
        spinlock_unlock_irqrestore(
            &paging_lock,
            flags
        );

        __atomic_add_fetch(
            &destroy_wait_failures,
            1,
            __ATOMIC_RELAXED
        );

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
            (entry & PAGE_PRESENT) == 0 ||
            (entry & PAGE_HUGE) != 0
        )
        {
            continue;
        }

        free_table_tree_locked(
            entry & PAGE_ADDRESS_MASK,
            3
        );

        root[index] = 0;
    }

    free_page((void *)root_physical);

    spinlock_unlock_irqrestore(
        &paging_lock,
        flags
    );

    __atomic_add_fetch(
        &user_spaces_destroyed,
        1,
        __ATOMIC_RELAXED
    );
}

void paging_activate(uint64_t root_physical)
{
    initialize();

    root_physical &= PAGE_ADDRESS_MASK;

    if (root_physical == 0)
    {
        root_physical = kernel_root_physical;
    }

    uint32_t cpu_index =
        smp_current_cpu_index();

    if (cpu_index >= SMP_MAX_CPUS)
    {
        cpu_index = 0;
    }

    uint64_t current_root = read_cr3();

    if (current_root == root_physical)
    {
        __atomic_store_n(
            &active_roots[cpu_index],
            root_physical,
            __ATOMIC_RELEASE
        );

        return;
    }

    __atomic_store_n(
        &active_roots[cpu_index],
        PAGING_ROOT_TRANSITION,
        __ATOMIC_RELEASE
    );

    write_cr3(root_physical);

    __atomic_store_n(
        &active_roots[cpu_index],
        root_physical,
        __ATOMIC_RELEASE
    );
}

bool paging_map_user_page_in(
    uint64_t root_physical,
    uint64_t virtual_address,
    uint64_t physical_address,
    bool writable,
    bool executable
)
{
    initialize();

    root_physical &= PAGE_ADDRESS_MASK;

    uint64_t flags =
        spinlock_lock_irqsave(
            &paging_lock
        );

    bool mapped = map_page_locked(
        root_physical,
        virtual_address,
        physical_address,
        writable,
        true,
        executable
    );

    spinlock_unlock_irqrestore(
        &paging_lock,
        flags
    );

    if (!mapped)
    {
        return false;
    }

    __atomic_add_fetch(
        &map_operations,
        1,
        __ATOMIC_RELAXED
    );

    (void)perform_shootdown(
        root_physical,
        virtual_address,
        PAGING_SHOOTDOWN_PAGE
    );

    return true;
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
    initialize();

    uint64_t flags =
        spinlock_lock_irqsave(
            &paging_lock
        );

    bool mapped = map_page_locked(
        kernel_root_physical,
        virtual_address,
        physical_address,
        writable,
        false,
        false
    );

    spinlock_unlock_irqrestore(
        &paging_lock,
        flags
    );

    if (!mapped)
    {
        return false;
    }

    __atomic_add_fetch(
        &map_operations,
        1,
        __ATOMIC_RELAXED
    );

    (void)perform_shootdown(
        kernel_root_physical,
        virtual_address,
        PAGING_SHOOTDOWN_PAGE
    );

    return true;
}

bool paging_unmap_page_in(
    uint64_t root_physical,
    uint64_t virtual_address
)
{
    initialize();

    root_physical &= PAGE_ADDRESS_MASK;

    if (
        root_physical == 0 ||
        (virtual_address & (PAGE_SIZE - 1)) != 0
    )
    {
        return false;
    }

    uint64_t flags =
        spinlock_lock_irqsave(
            &paging_lock
        );

    uint64_t *entry = NULL;

    bool found = lookup_leaf_locked(
        root_physical,
        virtual_address,
        &entry,
        false
    );

    if (found && entry != NULL)
    {
        *entry = 0;
    }

    spinlock_unlock_irqrestore(
        &paging_lock,
        flags
    );

    if (!found)
    {
        return false;
    }

    __atomic_add_fetch(
        &unmap_operations,
        1,
        __ATOMIC_RELAXED
    );

    (void)perform_shootdown(
        root_physical,
        virtual_address,
        PAGING_SHOOTDOWN_PAGE
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
    initialize();

    uint64_t flags =
        spinlock_lock_irqsave(
            &paging_lock
        );

    bool mapped = lookup_leaf_locked(
        root_physical,
        virtual_address,
        NULL,
        false
    );

    spinlock_unlock_irqrestore(
        &paging_lock,
        flags
    );

    return mapped;
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
    initialize();

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

    uint64_t flags =
        spinlock_lock_irqsave(
            &paging_lock
        );

    bool valid = true;

    for (;;)
    {
        uint64_t *entry = NULL;

        if (
            !lookup_leaf_locked(
                root_physical,
                page,
                &entry,
                true
            ) ||
            (writable &&
             (entry == NULL ||
              (*entry & PAGE_WRITABLE) == 0))
        )
        {
            valid = false;
            break;
        }

        if (page == final_page)
        {
            break;
        }

        if (page > UINT64_MAX - PAGE_SIZE)
        {
            valid = false;
            break;
        }

        page += PAGE_SIZE;
    }

    spinlock_unlock_irqrestore(
        &paging_lock,
        flags
    );

    return valid;
}

bool paging_run_shootdown_self_test(void)
{
    initialize();

    uint64_t failures_before =
        __atomic_load_n(
            &shootdown_failures,
            __ATOMIC_RELAXED
        );

    uint64_t ipis_before =
        __atomic_load_n(
            &shootdown_ipis,
            __ATOMIC_RELAXED
        );

    if (paging_is_mapped_in(
            kernel_root_physical,
            PAGING_SELF_TEST_ADDRESS
        ))
    {
        __atomic_store_n(
            &self_test_status,
            2,
            __ATOMIC_RELEASE
        );

        return false;
    }

    void *page = alloc_page();

    if (page == NULL)
    {
        __atomic_store_n(
            &self_test_status,
            2,
            __ATOMIC_RELEASE
        );

        return false;
    }

    bool mapped = paging_map_kernel_page(
        PAGING_SELF_TEST_ADDRESS,
        (uint64_t)page,
        true
    );

    bool visible =
        mapped &&
        paging_is_mapped_in(
            kernel_root_physical,
            PAGING_SELF_TEST_ADDRESS
        );

    bool unmapped = false;

    if (visible)
    {
        unmapped = paging_unmap_page_in(
            kernel_root_physical,
            PAGING_SELF_TEST_ADDRESS
        );
    }

    bool absent =
        !paging_is_mapped_in(
            kernel_root_physical,
            PAGING_SELF_TEST_ADDRESS
        );

    if (absent)
    {
        free_page(page);
    }

    uint64_t failures_after =
        __atomic_load_n(
            &shootdown_failures,
            __ATOMIC_RELAXED
        );

    uint64_t ipis_after =
        __atomic_load_n(
            &shootdown_ipis,
            __ATOMIC_RELAXED
        );

    bool remote_exercised =
        smp_online_count() <= 1 ||
        ipis_after > ipis_before;

    bool passed = (
        mapped &&
        visible &&
        unmapped &&
        absent &&
        failures_after == failures_before &&
        remote_exercised
    );

    __atomic_store_n(
        &self_test_status,
        passed ? 1U : 2U,
        __ATOMIC_RELEASE
    );

    return passed;
}

void paging_register_current_cpu(void)
{
    initialize();

    uint32_t cpu_index =
        smp_current_cpu_index();

    if (cpu_index >= SMP_MAX_CPUS)
    {
        return;
    }

    __atomic_store_n(
        &active_roots[cpu_index],
        read_cr3(),
        __ATOMIC_RELEASE
    );
}

void paging_handle_tlb_ipi(void)
{
    if (!initialized)
    {
        return;
    }

    uint32_t cpu_index =
        smp_current_cpu_index();

    if (cpu_index >= SMP_MAX_CPUS)
    {
        return;
    }

    paging_mailbox_t *mailbox =
        &mailboxes[cpu_index];

    uint64_t generation =
        __atomic_load_n(
            &mailbox->request_generation,
            __ATOMIC_ACQUIRE
        );

    if (
        generation == 0 ||
        generation == __atomic_load_n(
            &mailbox->acknowledged_generation,
            __ATOMIC_RELAXED
        )
    )
    {
        return;
    }

    uint64_t root_physical =
        mailbox->root_physical;

    uint64_t virtual_address =
        mailbox->virtual_address;

    uint32_t operation =
        mailbox->operation;

    invalidate_if_current(
        root_physical,
        virtual_address,
        operation
    );

    __atomic_add_fetch(
        &remote_invalidations,
        1,
        __ATOMIC_RELAXED
    );

    __atomic_store_n(
        &mailbox->acknowledged_generation,
        generation,
        __ATOMIC_RELEASE
    );
}

void paging_print_status(void)
{
    initialize();

    uint32_t active_count = 0;
    uint32_t cpu_count = smp_cpu_count();

    if (cpu_count == 0)
    {
        cpu_count = 1;
    }

    if (cpu_count > SMP_MAX_CPUS)
    {
        cpu_count = SMP_MAX_CPUS;
    }

    for (
        uint32_t cpu_index = 0;
        cpu_index < cpu_count;
        cpu_index++
    )
    {
        uint64_t active =
            __atomic_load_n(
                &active_roots[cpu_index],
                __ATOMIC_ACQUIRE
            );

        if (
            active != 0 &&
            active != PAGING_ROOT_TRANSITION
        )
        {
            active_count++;
        }
    }

    uint32_t test_status =
        __atomic_load_n(
            &self_test_status,
            __ATOMIC_ACQUIRE
        );

    const char *test_name =
        test_status == 1 ?
            "passed" :
        test_status == 2 ?
            "failed" :
            "not-run";

    kprintf(
        "SMP paging: selftest=%s active=%u maps=%llu unmaps=%llu roots=%llu destroyed=%llu shootdowns=%llu ipis=%llu local=%llu remote=%llu failures=%llu destroy-waits=%llu\n",
        test_name,
        (unsigned int)active_count,
        (unsigned long long)__atomic_load_n(
            &map_operations,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &unmap_operations,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &user_spaces_created,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &user_spaces_destroyed,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &shootdown_requests,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &shootdown_ipis,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &local_invalidations,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &remote_invalidations,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &shootdown_failures,
            __ATOMIC_RELAXED
        ),
        (unsigned long long)__atomic_load_n(
            &destroy_wait_failures,
            __ATOMIC_RELAXED
        )
    );
}
