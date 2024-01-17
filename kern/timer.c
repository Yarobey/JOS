#include <inc/assert.h>
#include <inc/memlayout.h>
#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/types.h>
#include <inc/uefi.h>
#include <inc/x86.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/pmap.h>
#include <kern/pmap.h>
#include <kern/timer.h>
#include <kern/trap.h>
#include <kern/tsc.h>

#define kilo      (1000ULL)
#define Mega      (kilo * kilo)
#define Giga      (kilo * Mega)
#define Tera      (kilo * Giga)
#define Peta      (kilo * Tera)
#define ULONG_MAX ~0UL

#if LAB <= 6
/* Early variant of memory mapping that does 1:1 aligned area mapping
 * in 2MB pages. You will need to reimplement this code with proper
 * virtual memory mapping in the future. */
void *
mmio_map_region(physaddr_t pa, size_t size) {
    void map_addr_early_boot(uintptr_t addr, uintptr_t addr_phys, size_t sz);
    const physaddr_t base_2mb = 0x200000;
    uintptr_t org = pa;
    size += pa & (base_2mb - 1);
    size += (base_2mb - 1);
    pa &= ~(base_2mb - 1);
    size &= ~(base_2mb - 1);
    map_addr_early_boot(pa, pa, size);
    return (void *)org;
}
void *
mmio_remap_last_region(physaddr_t pa, void *addr, size_t oldsz, size_t newsz) {
    return mmio_map_region(pa, newsz);
}
#endif

struct Timer timertab[MAX_TIMERS];
struct Timer *timer_for_schedule;

struct Timer timer_hpet0 = {
        .timer_name = "hpet0",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim0,
        .handle_interrupts = hpet_handle_interrupts_tim0,
};

struct Timer timer_hpet1 = {
        .timer_name = "hpet1",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim1,
        .handle_interrupts = hpet_handle_interrupts_tim1,
};

struct Timer timer_acpipm = {
        .timer_name = "pm",
        .timer_init = acpi_enable,
        .get_cpu_freq = pmtimer_cpu_frequency,
};

void
acpi_enable(void) {
    FADT *fadt = get_fadt();
    outb(fadt->SMI_CommandPort, fadt->AcpiEnable);
    while ((inw(fadt->PM1aControlBlock) & 1) == 0) /* nothing */
        ;
}

static bool
verify_table(const void *vp, uint32_t size) {
    uint8_t * p = (uint8_t *)vp;
    // It should be uint8_t, but then UBSAN says that there's int -> unsigned char truncation on line 83. :thinking:
    uint32_t sum = 0;
    for (int i = 0; i < size; i++)
        sum += p[i];
    return (sum & 0xFF) == 0;
}

static void *
acpi_find_table(const char *sign) {
    /*
     * This function performs lookup of ACPI table by its signature
     * and returns valid pointer to the table mapped somewhere.
     *
     * It is a good idea to checksum tables before using them.
     *
     * HINT: Use mmio_map_region/mmio_remap_last_region
     * before accessing table addresses
     * (Why mmio_remap_last_region is requrired?)
     * HINT: RSDP address is stored in uefi_lp->ACPIRoot
     * HINT: You may want to distunguish RSDT/XSDT
     */
    RSDP * rsdp = mmio_map_region(uefi_lp->ACPIRoot, sizeof(RSDP));
    if (!verify_table(rsdp, 20))
        panic("ACPI RSDP is corrupted");
    if (rsdp->Revision) { // ACPI 2.0+; 64-bit addresses
        if (!verify_table(rsdp, rsdp->Length))
            panic("ACPI RSDP is corrupted");
        XSDT * xsdt = mmio_map_region(rsdp->XsdtAddress, sizeof(ACPISDTHeader)); // just header cuz we dont know actual lenght yet
        xsdt = mmio_remap_last_region(rsdp->XsdtAddress, xsdt, sizeof(ACPISDTHeader), xsdt->h.Length);
        if (!verify_table(xsdt, xsdt->h.Length))
            panic("ACPI XSDT is corrupted");
        uint32_t entries = (xsdt->h.Length - sizeof(ACPISDTHeader)) / sizeof(uint64_t);
        for (uint32_t i = 0; i < entries; i++) {
            ACPISDTHeader * h = mmio_map_region(xsdt->PointerToOtherSDT[i], sizeof(ACPISDTHeader));
            h = mmio_remap_last_region(xsdt->PointerToOtherSDT[i], h, sizeof(ACPISDTHeader), h->Length);
            if (verify_table(h, h->Length) && !strncmp(h->Signature, sign, 4))
                return (void *)h;
        }
    } else { // ACPI 1.0; 32-bit addresses
        RSDT * rsdt = mmio_map_region(rsdp->RsdtAddress, sizeof(ACPISDTHeader)); // just header cuz we dont know actual lenght yet
        rsdt = mmio_remap_last_region(rsdp->RsdtAddress, rsdt, sizeof(ACPISDTHeader), rsdt->h.Length);
        if (!verify_table(rsdt, rsdt->h.Length))
            panic("ACPI RSDT is corrupted");
        uint32_t entries = (rsdt->h.Length - sizeof(ACPISDTHeader)) / sizeof(uint32_t);
        for (uint32_t i = 0; i < entries; i++) {
            ACPISDTHeader * h = mmio_map_region(rsdt->PointerToOtherSDT[i], sizeof(ACPISDTHeader));
            h = mmio_remap_last_region(rsdt->PointerToOtherSDT[i], h, sizeof(ACPISDTHeader), h->Length);
            if (verify_table(h, h->Length) && !strncmp(h->Signature, sign, 4))
                return (void *)h;
        }
    }
    
    return NULL;
}

MCFG *
get_mcfg(void) {
    static MCFG *kmcfg;
    if (!kmcfg) {
        struct AddressSpace *as = switch_address_space(&kspace);
        kmcfg = acpi_find_table("MCFG");
        switch_address_space(as);
    }

    return kmcfg;
}

#define MAX_SEGMENTS 16

uintptr_t
make_fs_args(char *ustack_top) {

    MCFG *mcfg = get_mcfg();
    if (!mcfg) {
        cprintf("MCFG table is absent!");
        return (uintptr_t)ustack_top;
    }

    char *argv[MAX_SEGMENTS + 3] = {0};

    /* Store argv strings on stack */

    ustack_top -= 3;
    argv[0] = ustack_top;
    nosan_memcpy(argv[0], "fs", 3);

    int nent = (mcfg->h.Length - sizeof(MCFG)) / sizeof(CSBAA);
    if (nent > MAX_SEGMENTS)
        nent = MAX_SEGMENTS;

    for (int i = 0; i < nent; i++) {
        CSBAA *ent = &mcfg->Data[i];

        char arg[64];
        snprintf(arg, sizeof(arg) - 1, "ecam=%llx:%04x:%02x:%02x",
                 (long long)ent->BaseAddress, ent->SegmentGroup, ent->StartBus, ent->EndBus);

        int len = strlen(arg) + 1;
        ustack_top -= len;
        nosan_memcpy(ustack_top, arg, len);
        argv[i + 1] = ustack_top;
    }

    char arg[64];
    snprintf(arg, sizeof(arg) - 1, "tscfreq=%llx", (long long)tsc_calibrate());
    int len = strlen(arg) + 1;
    ustack_top -= len;
    nosan_memcpy(ustack_top, arg, len);
    argv[nent + 1] = ustack_top;

    /* Realign stack */
    ustack_top = (char *)((uintptr_t)ustack_top & ~(2 * sizeof(void *) - 1));

    /* Copy argv vector */
    ustack_top -= (nent + 3) * sizeof(void *);
    nosan_memcpy(ustack_top, argv, (nent + 3) * sizeof(argv[0]));

    char **argv_arg = (char **)ustack_top;
    long argc_arg = nent + 2;

    /* Store argv and argc arguemnts on stack */
    ustack_top -= sizeof(void *);
    nosan_memcpy(ustack_top, &argv_arg, sizeof(argv_arg));
    ustack_top -= sizeof(void *);
    nosan_memcpy(ustack_top, &argc_arg, sizeof(argc_arg));

    /* and return new stack pointer */
    return (uintptr_t)ustack_top;
}

/* Obtain and map FADT ACPI table address. */
FADT *
get_fadt(void) {
    // HINT: ACPI table signatures are
    //       not always as their names
    // We need to get in from acpi_find_table only once,
    // so thats why we use static here.
    static FADT *fadt;
    if (!fadt)
        fadt = acpi_find_table("FACP");
    return fadt;
}

/* Obtain and map RSDP ACPI table address. */
HPET *
get_hpet(void) {
    // We need to get it from acpi_find_table only once,
    // so thats why we use static here.
    static HPET *hpet;
    if (!hpet)
        hpet = acpi_find_table("HPET");
    return hpet;
}

/* Getting physical HPET timer address from its table. */
HPETRegister *
hpet_register(void) {
    HPET *hpet_timer = get_hpet();
    if (!hpet_timer->address.address) panic("hpet is unavailable\n");

    uintptr_t paddr = hpet_timer->address.address;
    return mmio_map_region(paddr, sizeof(HPETRegister));
}

/* Debug HPET timer state. */
void
hpet_print_struct(void) {
    HPET *hpet = get_hpet();
    assert(hpet != NULL);
    cprintf("signature = %s\n", (hpet->h).Signature);
    cprintf("length = %08x\n", (hpet->h).Length);
    cprintf("revision = %08x\n", (hpet->h).Revision);
    cprintf("checksum = %08x\n", (hpet->h).Checksum);

    cprintf("oem_revision = %08x\n", (hpet->h).OEMRevision);
    cprintf("creator_id = %08x\n", (hpet->h).CreatorID);
    cprintf("creator_revision = %08x\n", (hpet->h).CreatorRevision);

    cprintf("hardware_rev_id = %08x\n", hpet->hardware_rev_id);
    cprintf("comparator_count = %08x\n", hpet->comparator_count);
    cprintf("counter_size = %08x\n", hpet->counter_size);
    cprintf("reserved = %08x\n", hpet->reserved);
    cprintf("legacy_replacement = %08x\n", hpet->legacy_replacement);
    cprintf("pci_vendor_id = %08x\n", hpet->pci_vendor_id);
    cprintf("hpet_number = %08x\n", hpet->hpet_number);
    cprintf("minimum_tick = %08x\n", hpet->minimum_tick);

    cprintf("address_structure:\n");
    cprintf("address_space_id = %08x\n", (hpet->address).address_space_id);
    cprintf("register_bit_width = %08x\n", (hpet->address).register_bit_width);
    cprintf("register_bit_offset = %08x\n", (hpet->address).register_bit_offset);
    cprintf("address = %08lx\n", (unsigned long)(hpet->address).address);
}

static volatile HPETRegister *hpetReg;
/* HPET timer period (in femtoseconds) */
static uint64_t hpetFemto = 0;
/* HPET timer frequency */
static uint64_t hpetFreq = 0;

/* HPET timer initialisation */
void
hpet_init() {
    if (hpetReg == NULL) {
        nmi_disable();
        hpetReg = hpet_register();
        uint64_t cap = hpetReg->GCAP_ID;
        hpetFemto = (uintptr_t)(cap >> 32);
        if (!(cap & HPET_LEG_RT_CAP)) panic("HPET has no LegacyReplacement mode");

        // cprintf("hpetFemto = %llu\n", hpetFemto);
        hpetFreq = (1 * Peta) / hpetFemto;
        // cprintf("HPET: Frequency = %d.%03dMHz\n", (uintptr_t)(hpetFreq / Mega), (uintptr_t)(hpetFreq % Mega));
        /* Enable ENABLE_CNF bit to enable timer */
        hpetReg->GEN_CONF |= HPET_ENABLE_CNF;
        nmi_enable();
    }
}

/* HPET register contents debugging. */
void
hpet_print_reg(void) {
    cprintf("GCAP_ID = %016lx\n", (unsigned long)hpetReg->GCAP_ID);
    cprintf("GEN_CONF = %016lx\n", (unsigned long)hpetReg->GEN_CONF);
    cprintf("GINTR_STA = %016lx\n", (unsigned long)hpetReg->GINTR_STA);
    cprintf("MAIN_CNT = %016lx\n", (unsigned long)hpetReg->MAIN_CNT);
    cprintf("TIM0_CONF = %016lx\n", (unsigned long)hpetReg->TIM0_CONF);
    cprintf("TIM0_COMP = %016lx\n", (unsigned long)hpetReg->TIM0_COMP);
    cprintf("TIM0_FSB = %016lx\n", (unsigned long)hpetReg->TIM0_FSB);
    cprintf("TIM1_CONF = %016lx\n", (unsigned long)hpetReg->TIM1_CONF);
    cprintf("TIM1_COMP = %016lx\n", (unsigned long)hpetReg->TIM1_COMP);
    cprintf("TIM1_FSB = %016lx\n", (unsigned long)hpetReg->TIM1_FSB);
    cprintf("TIM2_CONF = %016lx\n", (unsigned long)hpetReg->TIM2_CONF);
    cprintf("TIM2_COMP = %016lx\n", (unsigned long)hpetReg->TIM2_COMP);
    cprintf("TIM2_FSB = %016lx\n", (unsigned long)hpetReg->TIM2_FSB);
}

/* HPET main timer counter value. */
uint64_t
hpet_get_main_cnt(void) {
    return hpetReg->MAIN_CNT;
}

/* - Configure HPET timer 0 to trigger every 0.5 seconds on IRQ_TIMER line
 * - Configure HPET timer 1 to trigger every 1.5 seconds on IRQ_CLOCK line
 *
 * HINT To be able to use HPET as PIT replacement consult
 *      LegacyReplacement functionality in HPET spec.
 * HINT Don't forget to unmask interrupt in PIC */

/* https://forge.ispras.ru/attachments/download/11759/hpet.pdf page 12
 * Set LEG_RT_CNF bit in Genereal configuration register.
 * TimerX configuration is described on page 16-18
 * We enable interrupts, set value, enable periodic interrupts.
 * Comparator register value (TIMX_COMP) stores value for timer to
 * compare with. (page 19-20) */
void
hpet_enable_interrupts_tim0(void) {
    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;
    /* TimerX configuration is described on page 16-18
     * We enable interrupts, set value, enable periodic interrupts. */
    hpetReg->TIM0_CONF = 0;
    hpetReg->TIM0_CONF |= (IRQ_TIMER << 9);
    hpetReg->TIM0_CONF |= HPET_TN_TYPE_CNF;
    hpetReg->TIM0_CONF |= HPET_TN_INT_ENB_CNF;
    hpetReg->TIM0_CONF |= HPET_TN_VAL_SET_CNF;
    hpetReg->TIM0_COMP = Peta / 2 / hpetFemto;
    pic_irq_unmask(IRQ_TIMER);
}

void
hpet_enable_interrupts_tim1(void) {
    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;
    hpetReg->TIM1_CONF = 0;
    hpetReg->TIM1_CONF |= (IRQ_CLOCK << 9);
    hpetReg->TIM1_CONF |= HPET_TN_TYPE_CNF;
    hpetReg->TIM1_CONF |= HPET_TN_INT_ENB_CNF;
    hpetReg->TIM1_CONF |= HPET_TN_VAL_SET_CNF;
    hpetReg->TIM1_COMP = 3 * Peta / 2 / hpetFemto;
    pic_irq_unmask(IRQ_CLOCK);
}

void
hpet_handle_interrupts_tim0(void) {
    pic_send_eoi(IRQ_TIMER);
}

void
hpet_handle_interrupts_tim1(void) {
    pic_send_eoi(IRQ_CLOCK);
}

/* Calculate CPU frequency in Hz with the help with HPET timer.
 * HINT Use hpet_get_main_cnt function and do not forget about
 * about pause instruction. */
uint64_t
hpet_cpu_frequency(void) {
    static uint64_t cpu_freq;
    if (cpu_freq)
        return cpu_freq;

    const uint64_t wait = 100;
    uint64_t hpet_count = 0;
    uint64_t hpet_first = hpet_get_main_cnt();
    uint64_t tsc_first = read_tsc();
    uint64_t tsc_last = read_tsc();
    while (hpet_count < hpetFreq / wait) {
        asm("pause");
        hpet_count = hpet_get_main_cnt() - hpet_first;
        tsc_last = read_tsc();
    }

    cpu_freq = (tsc_last - tsc_first) * hpetFreq / hpet_count;
    return cpu_freq;
}

uint32_t
pmtimer_get_timeval(void) {
    FADT *fadt = get_fadt();
    return inl(fadt->PMTimerBlock);
}

/* Calculate CPU frequency in Hz with the help with ACPI PowerManagement timer.
 * HINT Use pmtimer_get_timeval function and do not forget that ACPI PM timer
 *      can be 24-bit or 32-bit. */
uint64_t
pmtimer_cpu_frequency(void) {
    static uint64_t cpu_freq;
    if (cpu_freq)
        return cpu_freq;

    const uint64_t wait = 100;
    uint64_t pm_count = 0;
    uint64_t pm_first = pmtimer_get_timeval();
    uint64_t tsc_first = read_tsc();
    uint64_t tsc_last = read_tsc();
    while (pm_count < PM_FREQ / wait) {
        asm("pause");
        uint64_t pm_cur = pmtimer_get_timeval();
        tsc_last = read_tsc();
        if (pm_first <= pm_cur) {
            pm_count = pm_cur - pm_first;
        } else if (pm_first - pm_cur <= 0x00FFFFFF) {
            pm_count = 0x00FFFFFF - pm_first + pm_cur;
        } else {
            pm_count = 0xFFFFFFFF - pm_first + pm_cur;
        }
    }
    cpu_freq = (tsc_last - tsc_first) * PM_FREQ / pm_count;
    return cpu_freq;
}
