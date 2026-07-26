#ifndef SMP_H
#define SMP_H

#include <stdbool.h>
#include <stdint.h>
#include <limine.h>

#define SMP_MAX_CPUS 64

typedef enum
{
    SMP_CPU_OFFLINE,
    SMP_CPU_STARTING,
    SMP_CPU_ONLINE,
    SMP_CPU_PARKED,
    SMP_CPU_WORKER
} smp_cpu_state_t;

typedef struct
{
    uint32_t index;
    uint32_t processor_id;
    uint32_t apic_id;
    bool bsp;
    volatile uint32_t state;
} smp_cpu_t;

bool smp_init(
    struct limine_mp_response *response
);

bool smp_is_available(void);
bool smp_uses_x2apic(void);

uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);
uint32_t smp_bsp_apic_id(void);

const smp_cpu_t *smp_cpu(uint32_t index);
const smp_cpu_t *smp_current_cpu(void);

const char *smp_cpu_state_name(
    smp_cpu_state_t state
);

void smp_print_status(void);

#endif
