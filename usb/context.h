/*
В этом файле реализованы все структуры контекстов из пункта 6.2
*/

#ifndef __CONTEXT_H__
#define __CONTEXT_H__

#define PACKED     __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))

// The Slot Context data structure defines information 
// that applies to a device as a whole.
struct SlotContext{
    /*uint32_t route_string: 20;
    uint8_t speed: 4;
    uint8_t rzvdz0: 1;
    bool mtt: 1;
    bool hub: 1;
    uint8_t context_entries: 5;*/
    //Contains route_string(0-19 bytes), speed(20-23), MTT(25), HUB(26), context_entries(27-31)
    uint32_t first_row;

    uint16_t max_exit_latency;
    uint8_t root_hub_port_number;
    uint8_t number_of_ports;

    uint8_t parent_hub_slot_id;
    uint8_t parent_port_number;
/*    uint8_t ttt: 2;
    uint8_t rzvdz1: 4;
    uint16_t interrupter_target: 10;*/
    uint16_t interrupter_target;

    /*uint8_t usb_device_address;
    uint32_t rsvdz2: 19;
    uint8_t slot_state: 5;*/
    uint32_t slot_state;
    //6.2.2

    uint32_t rsvdo[4];
} PACKED;

/*struct EndpointContext {
    uint8_t ep_state: 3;
    uint8_t rsvdz0: 5;
    uint8_t mult: 2;
    uint8_t maxPStreams: 5;
    bool lsa:1;
    uint8_t interval;
    uint8_t max_esit_payload_hi;

    uint8_t rsvdz1: 1;
    uint8_t CErr: 2;
    uint8_t EP_type: 3;
    uint8_t rsvdz2: 1;
    bool hid: 1;
    uint8_t max_burst_size;
    uint16_t max_packet_size;
    
    bool dcs: 1;
    uint8_t rsvdz3: 3;
    uint64_t tr_dequeue_pointer:60;

    uint16_t avg_trb_length;
    uint16_t max_esit_payload_lo;

    uint32_t rsvdo[3];
} PACKED;*/


/*
  The Endpoint Context data structure defines information 
  that applies to a specific endpoint.
*/
struct EndpointContext {
    uint8_t ep_state;
    uint8_t maxPStreams;
    uint8_t interval;
    uint8_t max_esit_payload_hi;

    uint8_t flags_off;
    uint8_t max_burst_size;
    uint16_t max_packet_size;

    uint64_t transfer_ring_deque_ptr;

    uint16_t avg_trb_length;
    uint16_t max_esit_payload_lo;

    uint32_t rsvdo[3];
};

/*
The Input Control Context data structure defines which Device Context data
structures are affected by a command and the operations to be performed on
those contexts.
*/
struct InputControlContext {
    // 1:0 - rsvdz0
    uint32_t drop_context_flags;
    uint32_t add_context_flags;
    uint32_t reserved[5];
    uint8_t config_value;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t rsvdz0;
};


/*
The Stream Context data structure defines information that applies to a specific
Stream associated with an endpoint.
*/
struct StreamContext {
    /*bool dcs: 1;
    uint8_t sct: 3;
    uint64_t tr_dequeue_pointer: 60;*/
    uint64_t tr_dequeue_pointer;
    /*uint32_t stopped_edtla: 24;
    uint8_t rsvdo[5];*/
    uint32_t stopped_edtla;
    uint32_t rsvdo;
} PACKED;

// 6.2.5
// 0's element is InputControlContext
// remaining same as in DeviceContext
// struct EndpointContext InputContext[33];

/*
The Device Context data structure consists of up to 32 entries. The first entry
(entry_0) is the Slot Context data structure and the remaining entries are
Endpoint Context data structures. The Context Entries field in the Slot Context
identifies the number of entries in the Device Context.
*/
struct DeviceContext {
    struct SlotContext slot_context;
    struct EndpointContext endpoint_context[31];
    // !this struct must be PAGESIZE
};

//not checked yet


/*
The Input Context data structure specifies the endpoints and the operations to
be performed on those endpoints by the Address Device, Configure Endpoint,
and Evaluate Context Commands.
*/
struct InputContext {
    struct InputControlContext input_control_context;
    struct SlotContext slot_context;
    struct EndpointContext endpoint_context[31];
};

struct TransferTRB {
    volatile uint64_t trb_pointer;

    volatile uint16_t trb_transfer_len_lo;
    volatile uint8_t trb_transfer_len_hi;
    volatile uint8_t completion_code;

    volatile uint16_t flags;
    volatile uint8_t endpoint_id;
    volatile uint8_t slot_id;
};

struct AddressDeviceCommandTRB {
    volatile uint64_t input_context_ptr;

    volatile uint32_t reserved0;

    volatile uint16_t flags;
    volatile uint8_t reserved1;
    volatile uint8_t slot_id;
};

#endif