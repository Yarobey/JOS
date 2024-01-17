
#include "fs.h"
#include "nvme.h"

/* Return the virtual address of this disk block. */
void *
diskaddr(blockno_t blockno) {
    if (blockno == 0 || (super && blockno >= super->s_nblocks))
        panic("bad block number %08x in diskaddr", blockno);
    void *r = (void *)(uintptr_t)(DISKMAP + blockno * BLKSIZE);
#ifdef SANITIZE_USER_SHADOW_BASE
    platform_asan_unpoison(r, BLKSIZE);
#endif
    return r;
}

/* Fault any disk block that is read in to memory by
 * loading it from disk. */
static bool
bc_pgfault(struct UTrapframe *utf) {
    void *addr = (void *)utf->utf_fault_va;
    blockno_t blockno = ((uintptr_t)addr - (uintptr_t)DISKMAP) / BLKSIZE;

    /* Check that the fault was within the block cache region */
    if (addr < (void *)DISKMAP || addr >= (void *)(DISKMAP + DISKSIZE)) return 0;

    /* Sanity check the block number. */
    if (super && blockno >= super->s_nblocks)
        panic("reading non-existent block %08x out of %08x\n", blockno, super->s_nblocks);

    /* Allocate a page in the disk map region, read the contents
     * of the block from the disk into that page.
     * Hint: first round addr to page boundary. fs/ide.c has code to read
     * the disk. */
    addr = ROUNDDOWN(addr, BLKSIZE);
    int res = sys_alloc_region(CURENVID, addr, BLKSIZE, PROT_RW);
    if (res)
        panic("bc_pgfault: can't alloc memmory! %i", res);
    /* sys_alloc_region() allocates pages lazily, so addr's corresponding physical address is
     * zero_page_raw address (see pmap.c). Below nvme_read() will take this physical address and
     * pass in to NVMe controller as an address to deliver data to. This way zero_page_raw will be
     * corrupted and simultaneously used as reference for all blocks. To avoid this we need to map
     * addr not lazily. The simpliest way to do this is to write something on it.
     * P.S.: Do we have any syscalls to map pages not lazily? Seems that no =( */
    /* TLDR: lazy allocation doesn't work with NVMe because it uses physical address directly */
    *(char *)addr = 0;

    res = nvme_read(blockno * BLKSECTS, addr, BLKSECTS);
    if (res != NVME_OK)
        panic("bc_pgfault on va %p failed: reading\n", addr);

    return 1;
}

/* Flush the contents of the block containing VA out to disk if
 * necessary, then clear the PTE_D bit using sys_map_region().
 * If the block is not in the block cache or is not dirty, does
 * nothing.
 * Hint: Use is_page_present(), is_page_dirty(), and ide_write().
 * Hint: Use the PTE_SYSCALL constant when calling sys_map_region().
 * Hint: Don't forget to round addr down. */
void
flush_block(void *addr) {
    blockno_t blockno = ((uintptr_t)addr - (uintptr_t)DISKMAP) / BLKSIZE;
    int res;

    if (addr < (void *)(uintptr_t)DISKMAP || addr >= (void *)(uintptr_t)(DISKMAP + DISKSIZE))
        panic("flush_block of bad va %p", addr);
    if (blockno && super && blockno >= super->s_nblocks)
        panic("reading non-existent block %08x out of %08x\n", blockno, super->s_nblocks);

    addr = ROUNDDOWN(addr, BLKSIZE);
    if (!is_page_present(addr) || !is_page_dirty(addr))
        return;
    res = nvme_write(blockno * BLKSECTS, addr, BLKSECTS);
    if (res != NVME_OK)
        panic("flush_block of va %p failed: writing\n", addr);
    res = sys_map_region(CURENVID, addr, CURENVID, addr, BLKSIZE, PTE_SYSCALL & get_prot(addr));
    if (res)
        panic("flush_block of va %p failed: clearing PTE_D\n", addr);

    assert(!is_page_dirty(addr));
}

/* Test that the block cache works, by smashing the superblock and
 * reading it back. */
static void
check_bc(void) {
    struct Super backup;

    /* Back up super block */
    memmove(&backup, diskaddr(1), sizeof backup);

    /* Smash it */
    strcpy(diskaddr(1), "OOPS!\n");
    flush_block(diskaddr(1));
    assert(is_page_present(diskaddr(1)));
    assert(!is_page_dirty(diskaddr(1)));

    /* Clear it out */
    sys_unmap_region(0, diskaddr(1), PAGE_SIZE);
    assert(!is_page_present(diskaddr(1)));

    /* Read it back in */
    assert(strcmp(diskaddr(1), "OOPS!\n") == 0);

    /* Fix it */
    memmove(diskaddr(1), &backup, sizeof backup);
    flush_block(diskaddr(1));

    cprintf("block cache is good\n");
}

void
bc_init(void) {
    struct Super super;
    add_pgfault_handler(bc_pgfault);
    check_bc();

    /* Cache the super block by reading it once */
    memmove(&super, diskaddr(1), sizeof super);
}
