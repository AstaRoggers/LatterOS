#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    volatile uint32_t next_ticket;
    volatile uint32_t owner_ticket;
} spinlock_t;

#define SPINLOCK_INITIALIZER \
    { \
        .next_ticket = 0, \
        .owner_ticket = 0 \
    }

void spinlock_init(spinlock_t *lock);
void spinlock_lock(spinlock_t *lock);
bool spinlock_try_lock(spinlock_t *lock);
void spinlock_unlock(spinlock_t *lock);

uint64_t spinlock_lock_irqsave(spinlock_t *lock);
void spinlock_unlock_irqrestore(
    spinlock_t *lock,
    uint64_t interrupt_flags
);

bool spinlock_is_locked(const spinlock_t *lock);

#endif
