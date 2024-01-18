#include <inc/x86.h>
#include <inc/string.h>
#include <inc/lib.h>
#include "fs/pci.h"

#include "usb/context.h"
#include "usb/registers.h"
#include "usb/event_ring.h"
#include "usb/trb.h"

/*
нам нужно работать с памятью только по виртуальным адресам,
при этом нужно использовать mmio для регистров xhci,
поэтому мы последовательно будем алоцировать структуры
по адресам, которые мы храним
*/

#define PAGESIZE 4096
#define XHCI_BASE_ADDR         0x7010000000
uint8_t *__xhci_current_Va = (void*)XHCI_BASE_ADDR;
uintptr_t __xhci_current_Pa;

#define XHCI_MAX_MAP_MEM 0x4000
#define XHCI_RING_DEQUE_MEM 4096
#define DEVICE_NUMBRER 8
#define COMMAND_RING_DEQUE_SIZE 4096

//аналогично nvme
struct XhciController {
    struct PciDevice *pcidev;
    volatile uint8_t *mmio_base_addr;
    volatile uint8_t *buffer;
};

struct EventRing {
    struct TRBTemplate trb[ERS_SIZE];
} * event_ring;

struct NoOpCommandTRB {
    uint32_t rsvdz0[3];
    
    bool c: 1;
    uint16_t rsvdz1: 9;
    uint8_t type: 6;
    uint8_t slot_type: 5;
    uint16_t rsvdz2: 11;
} PACKED;

struct CommandCompletionEventTRB {
    uint64_t command_trb_ptr; // without 4 last bits
    uint32_t command_completion_parameter; // 24 + 8
    uint32_t flags;
};

struct EnableSlotCommandTRB {
   uint32_t rsrvd[3];
   uint32_t flags;
};

static struct XhciController xhci;
struct DoorbellRegister * doorbells;
volatile uint8_t * memory_space_ptr;
volatile uint64_t * dcbaap;
volatile uint8_t * device_context[32];
volatile uint8_t * command_ring_deque;
volatile uint8_t * command_ring_enqueue;
volatile bool pcs;
struct EventRingSegment *event_ring_segment_table;

volatile struct InputContext *input_context;
volatile struct TransferTRB *transfer_ring;

struct DeviceContext *dcbaa_table_clone[DEVICE_NUMBRER];


uint32_t get_portsc(int port_id){
    return *(uint32_t*)(((uint8_t*)(oper_regs))+0x400 + port_id*16);
}

// check usbsts.cnr bit
int controller_not_ready() {
    return (oper_regs->usbsts & (1 << 11)) >> 11;
}

/*
check oper_regs.crcr.crr bit
*/
void wait_for_command_ring_not_running() {
    while ((oper_regs->crcr & (1 << 3)) == 0) {}
}

void wait_for_command_ring_running() {
    while (oper_regs->crcr & (1 << 3)) { cprintf("-\n"); }
}

// "va" and "pa" returns values
int memory_map( void **va, uintptr_t *pa, size_t size) {
    // всегда мапим по границе страницы, иначе ERROR
    if (size % PAGESIZE != 0)
        size = size - (size % PAGESIZE) + PAGESIZE;
    if (va != NULL)
        *va = (void*)__xhci_current_Va;
    if (pa != NULL)
        *pa = __xhci_current_Pa;
    int res = sys_map_physical_region(__xhci_current_Pa, CURENVID, (void*)__xhci_current_Va, size, PROT_RW | PROT_CD);
    __xhci_current_Va += size;
    __xhci_current_Pa += size;
    return res;
}

int xhci_map(struct XhciController *ctl) {
    size_t size = get_bar_size(ctl->pcidev, 0);
    size = size > XHCI_MAX_MAP_MEM ? XHCI_MAX_MAP_MEM : size;
    // first time initialization of pa pointer
    __xhci_current_Pa = get_bar_address(ctl->pcidev, 0);
    return memory_map((void**)(&(ctl->mmio_base_addr)),NULL,size);
    //now, ctl->mmio_base_address is initialized
}

/* initialize mmio registers (figure 3-3) */
int xhci_register_init(struct XhciController *ctl) {
    __xhci_current_Pa = 0x1000000;
    uint8_t *memory_space_ptr = (uint8_t*)ctl->mmio_base_addr;
    cap_regs = (struct CapabilityRegisters *)memory_space_ptr;
    cprintf("^%p\n",cap_regs);
    oper_regs = ((void *)memory_space_ptr + cap_regs->caplength);
    cprintf("^%p\n",oper_regs);
    run_regs = ((void *)memory_space_ptr + (cap_regs->rtsoff & RTSOFF_MASK) );
    cprintf("^%p\n",run_regs);
    doorbells = ((void *)memory_space_ptr + (cap_regs->dboff & DBOFF_MASK));
    cprintf("^%p\n",doorbells);
    while(controller_not_ready()) {}

    // small check: read-only registers can't be rewrited
    //    cprintf("!! %d\n",oper_regs->pagesize);
    //    oper_regs->pagesize = 0xFFFFFFFF;
    //    cprintf("!! %d\n",oper_regs->pagesize);

    // этому 2048 с выравниванием по 6
    // но мапится всё равно целое число страниц, поэтому так
    uintptr_t pa;
    int res;
    size_t size = PAGESIZE;
    if ((res = memory_map((void**)(&dcbaap),&pa,size)))
        return res;
    oper_regs->dcbaap = pa | (oper_regs->dcbaap & 0b111111);
    cprintf("^%p %lx\n",dcbaap,oper_regs->dcbaap & ZERO_MASK_64(0b111111));

    // !но каждому из DCBAA нужна своя DeviceContext
    memset((void*)dcbaap,0,PAGESIZE);
    for (int i = 1; i < DEVICE_NUMBRER; i++) {
        if ((res = memory_map((void**)(dcbaa_table_clone+i),&pa,PAGESIZE) ))
            return res;
        dcbaap[i] = pa;
        memset(dcbaa_table_clone[i], 0, PAGESIZE);
        cprintf("  ^%p %lx %lx\n",dcbaa_table_clone[i],pa,dcbaap[i]);
    }
    uint32_t scratchpad = (cap_regs->hcsparams2 >> 21) & 0b11111;
    if ( scratchpad > 0) {
        // impossible in our case
        if (( res = memory_map((void**)dcbaa_table_clone, &pa, PAGESIZE*scratchpad) ))
            return res;
        dcbaap[0] = pa;
    }
    // TODO: zero fields of new pages, но не все, см 4.5.2

    // этому 4 Kib
    // COMMAND RING allocate
    size = COMMAND_RING_DEQUE_SIZE;
    if ((res = memory_map((void**)(&command_ring_deque),&pa,size)))
        return res;
    oper_regs->crcr = pa | (oper_regs->crcr & 0b111111);
    pcs = 1;
    command_ring_enqueue = command_ring_deque;
    cprintf("^%p %lx\n\n",command_ring_deque,oper_regs->crcr & ZERO_MASK_64(0b111111));

    return 0;
}

int xhci_event_ring_init(){
    /*
    allocate memory for ERST and write its address to run_regs.IRS
    */

    // выделим 16 Kib для таблицы, потом переприсвоим erstba
    // НО sizeof(struct EventRingSegment) = 16, а выравнивание erstba - 64
    // надо выделить 64 кб, тк 1024 с шагом 64
    size_t size = 64*1024; //INTERRUPTER_REGISTER_SET_MAXCOUNT * ERST_SIZE * sizeof(struct EventRingSegment);
    uintptr_t pa_event_ring_segment_table;
    int res;
    if ((res = memory_map((void**)(&event_ring_segment_table),&pa_event_ring_segment_table,size)))
        return res;
    cprintf("^%p %lx\n",event_ring_segment_table,pa_event_ring_segment_table);
    
    // выделим 512kib для самих Event Ring
    size = INTERRUPTER_REGISTER_SET_MAXCOUNT * ERST_SIZE * ERS_SIZE * sizeof(struct EventRingSegment);
    uintptr_t pa_event_ring;
    if ((res = memory_map((void**)(&event_ring),&pa_event_ring,size)))
        return res;
    cprintf("^%p %lx\n",event_ring,pa_event_ring);
    memset(event_ring,0,size);

    for (int i=0; i < INTERRUPTER_REGISTER_SET_MAXCOUNT; i++){
        event_ring_segment_table[i].ring_segment_size = ERS_SIZE;
        event_ring_segment_table[i].ring_segment_base_address = pa_event_ring_segment_table + 64*i;
        //cprintf("    %lx",event_ring_segment_table[i].ring_segment_base_address);

        run_regs->int_reg_set[i].erstsz.erstsz = 1;
        run_regs->int_reg_set[i].erdp = pa_event_ring + sizeof(struct TRBTemplate) * ERS_SIZE * i;
        run_regs->int_reg_set[i].erstba = event_ring_segment_table[i].ring_segment_base_address;
        
        //cprintf("    %lx %lx\n",run_regs->int_reg_set[i].erdp,run_regs->int_reg_set[i].erstba);
    }
    return 0;
}

void xhci_settings_init() {
    oper_regs->config |= DEVICE_NUMBRER;

    volatile uint32_t usbcmd = oper_regs->usbcmd;
    usbcmd = usbcmd | 0x1;
    oper_regs->usbcmd = usbcmd;         // setting Run/Stop register to Run state 

    //volatile uint16_t xecp_offset = (cap_regs->hccparams1 >> 16);
    //cprintf("hccparams1 - %x\n", cap_regs->hccparams1);
    //volatile uint32_t * xecp_pointer = (uint32_t *)((uint8_t *)cap_regs + (xecp_offset << 2)); 
    //volatile uint32_t xecp = xecp_pointer[0];
    //xecp = xecp | (1 << 24);
    //cprintf("xecp - %x\n",xecp);

    // здесь печать обнаруженных устройств. Если не видите здесь своего устройства - это проблема
    for (int i = 0; i < 8; i++) {
        uint32_t val = get_portsc(i); // *(uint32_t*)(((uint8_t*)(oper_regs))+0x400 + i*16);
        if ( val != 0x2a0 )
            cprintf("FIND: ");
        cprintf("PORTSC[%d] = %x\n",i,val);
    }
    cprintf("\n");
}

struct AddressDeviceCommandTRB address_device_command(uint64_t input_context_ptr, uint8_t slot_id, bool cycle_bit) {
    struct AddressDeviceCommandTRB adr_trb;
    adr_trb.slot_id = slot_id;
    adr_trb.input_context_ptr = input_context_ptr;
    adr_trb.reserved0 = 0;
    adr_trb.flags = 11 << 10;
    adr_trb.reserved1 = 0;
    if (cycle_bit) {
        adr_trb.flags += 1;
    }
    return adr_trb;
}

void init_dev(volatile struct EnableSlotCommandTRB *trb, int cycle_bit) {
    memset((void*)trb,0,sizeof(struct EnableSlotCommandTRB));
    trb->flags |= 9 << 10; //code of "enable slots"
    if (cycle_bit)
        trb->flags += 1;
}

void ring_hc_doorbell(){
    cprintf("ring_hc_doorbell\n");
    struct DoorbellRegister db;
    db.target = 1;
    db.stream_id = 0;
    doorbells[0] = db;
}

int xhci_slots_init() {
    init_dev((void*)command_ring_deque, 0);
    volatile struct CommandCompletionEventTRB *trb = (volatile void*)command_ring_deque;
    cprintf("  > %lx %x %x\n",trb->command_trb_ptr,trb->command_completion_parameter,trb->flags);
    ring_hc_doorbell();
    wait_for_command_ring_running();
    cprintf("  > %lx %x %x\n",trb->command_trb_ptr,trb->command_completion_parameter,trb->flags);
    return 0;

/*    size_t size = sizeof(struct InputContext);
    uintptr_t pa_inp_cntx_ptr;
    int res;
    if ((res = memory_map((void**)(&input_context),&pa_inp_cntx_ptr,size)))
        return res;

//    volatile uint8_t * input_context_ptr = (void *)input_context;
//    for (int i = 0; i < sizeof(struct InputContext); i++) {
//        input_context_ptr[i] = 0;
//    }

    input_context->input_control_context.add_context_flags = 0x3;
    input_context->slot_context.root_hub_port_number = 0x5;
    input_context->slot_context.slot_state = 0x1 << 4;
    //input_context->slot_context.usb_device_address = 0;
    uint32_t buf = input_context->slot_context.slot_state;
    buf = buf & ZERO_MASK_32(0xFF);
    input_context->slot_context.slot_state = buf;
    volatile uint32_t first_row = (1 << 27) + (1 << 20);
    input_context->slot_context.first_row = first_row;

    //struct DeviceContext * dev_cont_ptr = (void *)device_context[1];

    size = (PAGESIZE)*32;
    uintptr_t pa;
    uint8_t va;
    if ((res = memory_map((void**)(&va),&pa,size)))
        return res;

    for (int i = 1; i < 31; i++) {
        input_context->endpoint_context[i].transfer_ring_deque_ptr = (uint64_t)(va) + ((uint64_t)i)*PAGESIZE;
        //dev_cont_ptr->endpoint_context[i].transfer_ring_deque_ptr = (uint64_t)((void *)&(transfer_ring[i][0]) - 0x8040000000);
    }

    size = sizeof(struct TransferTRB) * 16 * 128;
    if ((res = memory_map((void**)(&transfer_ring),&pa,size)))
        return res;

    input_context->endpoint_context[0].transfer_ring_deque_ptr = (uint64_t)transfer_ring;
    //dev_cont_ptr->endpoint_context[0].transfer_ring_deque_ptr = (uint64_t)((void *)&(transfer_ring[0][0]) - 0x8040000000 + 1);
    volatile uint8_t flags = 0x26;
    input_context->endpoint_context[0].flags_off = flags;
    input_context->endpoint_context[0].max_burst_size = 0;
    input_context->endpoint_context[0].interval = 0;
    input_context->endpoint_context[0].maxPStreams = 0;
    input_context->endpoint_context[0].max_packet_size = 0x8;

    struct AddressDeviceCommandTRB * adr_command_ring_adr = (struct AddressDeviceCommandTRB *)command_ring_deque;
    adr_command_ring_adr[0] = address_device_command(pa_inp_cntx_ptr, 1, 1);
    doorbells[0].target = 0;
    doorbells[0].stream_id = 0;

    //wait_for_command_ring_not_running();
    return 0;*/
}

/* place command trb to command ring */
uint8_t* place_command(struct TRBTemplate trb){
    cprintf("place_command\n");
    trb.control = trb.control | pcs;
    *((struct TRBTemplate*)command_ring_enqueue) = trb;
    command_ring_enqueue = (uint8_t*)command_ring_enqueue + TRB_SIZE;

    return (uint8_t*)command_ring_enqueue - TRB_SIZE;

    // TODO: check if the ring is full (4.9.2.2)
}

void wait_command_completion(uint8_t* cmd_ptr){
    /* The Command TRB Pointer field of the Command Completion Event shall point to
       the Command TRB that initiated the event

       The Primary Event Ring receives all Command Completion Events.
    */

    /*for (int i=0; i<INTERRUPTER_REGISTER_SET_MAXCOUNT; i++){
        struct InterrupterRegisterSet irs = run_regs->int_reg_set[i];
        for (int j=0; j < irs.erstsz.erstsz; j++){
            struct EventRingSegment ers = ((struct EventRingSegment*)irs.erstba)[j];
        }
    }*/

}

void command_ring_test(){
    struct NoOpCommandTRB trb;
    uint8_t* cmd_ptr = place_command(*((struct TRBTemplate*)(&trb)));
    ring_hc_doorbell();
    wait_command_completion(cmd_ptr);
}

void reset_root_hub_port(int port_id){
    // *(uint32_t*)(((uint8_t*)(oper_regs))+0x400 + port_id*16);
    uint32_t portsc = get_portsc(port_id);
    *(uint32_t*)(((uint8_t*)(oper_regs))+0x400 + port_id*16) = portsc | PORTSC_PR;
    while (!(get_portsc(port_id) | PORTSC_PRC)){}
}

/* (4.3) */
void xhci_usb_device_init(){
    // int port_id = 4;
    // // uint32_t portsc = get_portsc(port_id);
    // // if csc == 1 then attached
    // // if usb2: portsc.pr=1
    // //          wait for PortStatusChangeEvent
    // reset_root_hub_port(port_id);
    // int slot_id = enable_slot(port_id);
    volatile uint32_t *portsc = (uint32_t*)(((uint8_t*)(oper_regs))+0x400 + 4*16);
    cprintf("> %x\n",*portsc);
    *portsc |= 1 << 4;
    while ( ((*portsc) & (1<<21)) == 0 ) {}
    cprintf("> %x\n",*portsc);
    command_ring_test();
}

/* initialize all xhci registers and trb rings: (4.2) */
void
xhci_init() {
    struct XhciController *ctl = &xhci;
    struct PciDevice *pcidevice = find_pci_dev(0x0C, 0x03);
    //int err;
    if (pcidevice == NULL)
        panic("NVMe device not found\n");
    ctl->pcidev = pcidevice;
    if (xhci_map(ctl) )
        panic("XHCI device not found\n");
    if (xhci_register_init(ctl))
        panic("Unable to allocate XHCI structures\n");

    xhci_event_ring_init();
    xhci_settings_init();

    xhci_usb_device_init();

    if (xhci_slots_init())
        panic("Unable to allocate XHCI structures\n");
}

extern volatile int pci_initial;

void
umain(int argc, char **argv) {
    cprintf("***********************INIT USB***********************\n");
    // уже написана в fs/pci.c
    // делаем похожей на nvme
    pci_init(argv);
    xhci_init();
    cprintf("***********************END USB***********************\n");
}