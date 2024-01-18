#ifndef __EVENT_RING_H__
#define __EVENT_RING_H__

#define PACKED __attribute__((packed))

#define ERST_SIZE 1
#define ERS_SIZE 32

struct EventRingSegment {
    // uint8_t rvsdz0: 6;
    uint64_t ring_segment_base_address;
    uint16_t ring_segment_size;
    uint16_t rsvdz1[3];
} PACKED;

#endif