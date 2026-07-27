#include "spinlock.h"

#include <stddef.h>
#include <stdint.h>

#define RFLAGS_INTERRUPT_ENABLE (1ULL << 9)

static uint64_t interrupt_save_and_disable(void)
{
    uint64_t flags;

    __asm__ volatile(
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static void interrupt_restore(uint64_t flags)
{
    if ((flags & RFLAGS_INTERRUPT_ENABLE) != 0)
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

void spinlock_init(spinlock_t *lock)
{
    if (lock == NULL)
    {
        return;
    }

    __atomic_store_n(
        &lock->next_ticket,
        0,
        __ATOMIC_RELAXED
    );

    __atomic_store_n(
        &lock->owner_ticket,
        0,
        __ATOMIC_RELAXED
    );
}

void spinlock_lock(spinlock_t *lock)
{
    if (lock == NULL)
    {
        return;
    }

    uint32_t ticket =
        __atomic_fetch_add(
            &lock->next_ticket,
            1,
            __ATOMIC_RELAXED
        );

    while (
        __atomic_load_n(
            &lock->owner_ticket,
            __ATOMIC_ACQUIRE
        ) != ticket
    )
    {
        cpu_relax();
    }
}

bool spinlock_try_lock(spinlock_t *lock)
{
    if (lock == NULL)
    {
        return false;
    }

    uint32_t owner =
        __atomic_load_n(
            &lock->owner_ticket,
            __ATOMIC_ACQUIRE
        );

    uint32_t next =
        __atomic_load_n(
            &lock->next_ticket,
            __ATOMIC_RELAXED
        );

    if (owner != next)
    {
        return false;
    }

    uint32_t desired = next + 1;

    return __atomic_compare_exchange_n(
        &lock->next_ticket,
        &next,
        desired,
        false,
        __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED
    );
}

void spinlock_unlock(spinlock_t *lock)
{
    if (lock == NULL)
    {
        return;
    }

    __atomic_fetch_add(
        &lock->owner_ticket,
        1,
        __ATOMIC_RELEASE
    );
}

uint64_t spinlock_lock_irqsave(spinlock_t *lock)
{
    uint64_t flags =
        interrupt_save_and_disable();

    spinlock_lock(lock);

    return flags;
}

void spinlock_unlock_irqrestore(
    spinlock_t *lock,
    uint64_t interrupt_flags
)
{
    spinlock_unlock(lock);
    interrupt_restore(interrupt_flags);
}

bool spinlock_is_locked(const spinlock_t *lock)
{
    if (lock == NULL)
    {
        return false;
    }

    uint32_t owner =
        __atomic_load_n(
            &lock->owner_ticket,
            __ATOMIC_ACQUIRE
        );

    uint32_t next =
        __atomic_load_n(
            &lock->next_ticket,
            __ATOMIC_ACQUIRE
        );

    return owner != next;
}
