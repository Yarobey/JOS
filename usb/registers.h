/*
В этом файле описаны все регистры xHCI из пункта 5 спецификации
*/

#ifndef __REGISTERS_H__
#define __REGISTERS_H__

#define PACKED     __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))

#define ZERO_MASK_64(digit) (~((uint64_t)(digit)))
#define ZERO_MASK_32(digit) (~((uint32_t)(digit)))

/*
These registers specify the limits and capabilities of the host controller
implementation.
All Capability Registers are Read-Only (RO). The offsets for these registers are
all relative to the beginning of the host controller’s MMIO address space. 
*/
struct CapabilityRegisters {
    volatile uint8_t caplength;
    volatile uint8_t rsvd0;
    volatile uint16_t hciversion;
    volatile uint32_t hcsparams1;
    // volatile union {
    //     uint32_t all;
    //     struct {
    //     uint8_t MaxSlots;
    //     uint16_t MaxIntrs: 11;
    //     uint8_t rsvd: 5;
    //     uint8_t MaxPorts;
    //     } PACKED;
    // } hcsparams1;
    volatile uint32_t hcsparams2;
    volatile uint32_t hcsparams3;
    volatile uint32_t hccparams1;
    volatile uint32_t dboff; // last 2 bits are reserved
    volatile uint32_t rtsoff; // last 5 bits are reserved
    volatile uint32_t hccparams2;
    // and other inf
} PACKED * cap_regs;

#define DBOFF_MASK  (~((uint32_t)0b11))
#define RTSOFF_MASK (~((uint32_t)0b11))

#define PORTSC_PR (1 << 4)
#define PORTSC_PRC (1 << 21)


/*
The base address of this register space is referred to as Operational Base. The
Operational Base shall be Dword aligned and is calculated by adding the value
of the Capability Registers Length (CAPLENGTH) register (refer to Section 5.3.1)
to the Capability Base address. All registers are multiples of 32 bits in length.
Unless otherwise stated, all registers should be accessed as a 32 -bit width on
reads with an appropriate software mask, if needed. A software
read/modify/write mechanism should be invoked for partial writes.
*/
struct OperationalRegisters {
    volatile uint32_t usbcmd;
    volatile uint32_t usbsts;
    volatile uint32_t pagesize;
    volatile uint32_t rsvdz0[2];
    volatile uint32_t dnctrl;
    volatile uint64_t crcr;
    volatile uint32_t rsvdz1[4];
    volatile uint64_t dcbaap;
    volatile uint32_t config;
    volatile uint8_t rsvdz2[964];
    // see table 5-18
    // ...

} * oper_regs;

struct PortRegisterSet {
    uint32_t portsc;
    uint32_t portpmsc;
    uint32_t portli;
    uint32_t porthlpmc;
};

#define USBCMD_RS 0
#define USBCMD_HCRST 1
#define USBCMD_INTE 2
#define USBCMD_HSEE 3
#define USBCMD_LHCRST 7
#define USBCMD_CSS 8
#define USBCMD_CRS 9
#define USBCMD_EWE 10
#define USBCMD_EU3S 11
#define USBCMD_CME 13
#define USBCMD_ETE 14
#define USBCMD_TSC_EN 15
#define USBCMD_VTIOE 16

// TODO: add other constants (5.4.2 - 5.4.11)

/*
The Interrupter logic consists of an Interrupter Management Register, an
Interrupter Moderation Register, and the Event Ring Registers. A one to one
mapping is defined for Interrupter to MSI-X vector. Up to 1024 Interrupters are
supported.
*/
struct InterrupterRegisterSet {
    volatile uint32_t iman; //RW, only 0 and 1 bits
    volatile struct {
        uint16_t interval; //time, default=1msec
        uint16_t counter; //counter to run on deque
    } imod;
    volatile struct{
        uint16_t erstsz; //max deque size, see capability->HCSPARAMS2->ERNST
        uint16_t rsvd;
    } erstsz;
    volatile uint32_t rsvd;
    uint64_t erstba; //58 bits for address, 6 reserved //pointer to EVENT RING SEGMENT TABLE
    uint64_t erdp; //60 bits address, 1 - ehb, 3 - desi
};

#define IMAN_IP 0
#define IMAN_IE 1


/*
The Doorbell Array is organized as an array of up to 256 Doorbell Registers. One
32-bit Doorbell Register is defined in the array for each Device Slot. System
software utilizes the Doorbell Register to notify the xHC that it has Device Slot
related work for the xHC to perform.
The number of Doorbell Registers implemented by a particular instantiation of a
host controller is documented in the Number of Device Slots (MaxSlots) field of
the HCSPARAMS1 register (section 5.3.3).
These registers are pointed to by the Doorbell Offset Register (DBOFF) in the
xHC Capability register space. The Doorbell Array base address shall be Dword
aligned and is calculated by adding the value in the DBOFF register (section
5.3.7) to “Base” (the base address of the xHCI Capability register address space).
*/
struct DoorbellRegister {
    uint8_t target;
    uint8_t rsvd;
    uint16_t stream_id;
};


#define INTERRUPTER_REGISTER_SET_MAXCOUNT 1024


/*
The base address of this
register space is referred to as Runtime Base. The Runtime Base shall be 32-
byte aligned and is calculated by adding the value Runtime Register Space
Offset register (refer to Section 5.3.8) to the Capability Base address. All
Runtime registers are multiples of 32 bits in length.
*/
struct RuntimeRegisters {
    volatile uint32_t mfindex;
    volatile uint32_t rsvdz[7];
    volatile struct InterrupterRegisterSet int_reg_set[INTERRUPTER_REGISTER_SET_MAXCOUNT];
} * run_regs;

struct TRBTemplate {
    volatile uint64_t parameter;
    volatile uint32_t status;
    volatile uint32_t control;
};

#endif