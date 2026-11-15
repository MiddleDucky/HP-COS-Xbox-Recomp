#include "guest_vad.h"
#include <stdlib.h>
#include <string.h>

#define PAGE      4096u
#define S_COMMIT  0x1000u
#define S_RESERVE 0x2000u
#define S_FREE    0x10000u
#define T_PRIVATE 0x20000u
#define USER_TOP  0x7FFE0000u

typedef struct {
    uint32_t owner;           /* region base, 0 free */
    uint32_t alloc_protect;
    uint16_t state;           /* 0 free, or reserved, or committed */
    uint16_t protect;
} page_t;

static page_t  *s_pages;
static uint32_t s_top;

int guest_vad_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_VAD");
        on = e && *e == '1';
    }
    return on;
}

void guest_vad_init(uint32_t top)
{
    if (s_pages) return;
    s_top = top & ~(PAGE - 1);
    s_pages = (page_t *)calloc(s_top / PAGE, sizeof(page_t));
}

void guest_vad_reserve(uint32_t base, uint32_t *size, uint32_t protect, int commit)
{
    uint32_t start = base & ~(PAGE - 1), end, p;
    uint64_t e = (uint64_t)base + *size + PAGE - 1;
    e &= ~(uint64_t)(PAGE - 1);
    if (!s_pages || e > s_top) return;
    end = (uint32_t)e;
    *size = end - base;
    for (p = start / PAGE; p < end / PAGE; p++) {
        s_pages[p].owner = start;
        s_pages[p].alloc_protect = protect;
        s_pages[p].state = (uint16_t)(commit ? S_COMMIT : S_RESERVE);
        s_pages[p].protect = (uint16_t)(commit ? protect : 0);
    }
}

uint32_t guest_vad_commit(uint32_t *base, uint32_t *size, uint32_t protect,
                          void (*zero)(uint32_t, uint32_t))
{
    uint32_t start = *base & ~(PAGE - 1), end, p, owner;
    uint64_t e = (uint64_t)*base + *size + PAGE - 1;
    e &= ~(uint64_t)(PAGE - 1);
    if (!s_pages || e > s_top || !*size) return 0xC0000018u;
    end = (uint32_t)e;
    owner = s_pages[start / PAGE].owner;
    if (!owner) return 0xC0000018u;      /* STATUS_CONFLICTING_ADDRESSES */
    for (p = start / PAGE; p < end / PAGE; p++)
        if (s_pages[p].owner != owner) return 0xC0000018u;
    for (p = start / PAGE; p < end / PAGE; p++) {
        if (s_pages[p].state != S_COMMIT && zero) zero(p * PAGE, PAGE);
        s_pages[p].state = S_COMMIT;
        s_pages[p].protect = (uint16_t)protect;
    }
    *base = start;
    *size = end - start;
    return 0;
}

uint32_t guest_vad_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                        void (*zero)(uint32_t, uint32_t), int *released)
{
    uint32_t start = *base & ~(PAGE - 1), end, p, owner;
    uint64_t e;
    *released = 0;
    if (free_type != 0x4000u && free_type != 0x8000u) return 0xC000000Du;
    if (!s_pages || *base >= s_top) return 0xC00000A0u;   /* STATUS_MEMORY_NOT_ALLOCATED */
    owner = s_pages[start / PAGE].owner;
    if (!owner) return 0xC00000A0u;
    if (*size == 0) {
        if (free_type == 0x8000u && start != owner) return 0xC000009Fu;   /* STATUS_FREE_VM_NOT_AT_BASE */
        end = start;
        while (end < s_top && s_pages[end / PAGE].owner == owner) end += PAGE;
    } else {
        e = ((uint64_t)*base + *size + PAGE - 1) & ~(uint64_t)(PAGE - 1);
        if (e > s_top) return 0xC000001Au;   /* STATUS_UNABLE_TO_FREE_VM */
        end = (uint32_t)e;
        for (p = start / PAGE; p < end / PAGE; p++)
            if (s_pages[p].owner != owner) return 0xC000001Au;
    }
    for (p = start / PAGE; p < end / PAGE; p++) {
        if (s_pages[p].state == S_COMMIT && zero) zero(p * PAGE, PAGE);
        if (free_type == 0x8000u) {
            memset(&s_pages[p], 0, sizeof s_pages[p]);
        } else {
            s_pages[p].state = S_RESERVE;
            s_pages[p].protect = 0;
        }
    }
    if (free_type == 0x8000u) {
        /* The allocator's block goes once none of the region is left. A partial
         * release can leave holes, so look past them, up to the next region. */
        uint32_t q, left = 0;
        for (q = owner; q < s_top; q += PAGE) {
            uint32_t o = s_pages[q / PAGE].owner;
            if (o == owner) { left = 1; break; }
            if (o) break;
        }
        *released = !left;
    }
    *base = start;
    *size = end - start;
    return 0;
}

uint32_t guest_vad_query(uint32_t address, uint32_t info[7])
{
    uint32_t page = address & ~(PAGE - 1), end;
    if (address >= USER_TOP) return 0xC000000Du;
    memset(info, 0, 7 * sizeof info[0]);
    info[0] = page;
    if (!s_pages || page >= s_top) {
        info[3] = USER_TOP - page;
        info[4] = S_FREE;
        info[5] = 1;                     /* PAGE_NOACCESS */
        return 0;
    }
    end = page;
    if (!s_pages[page / PAGE].owner) {
        while (end < s_top && !s_pages[end / PAGE].owner) end += PAGE;
        if (end >= s_top) end = USER_TOP;
        info[3] = end - page;
        info[4] = S_FREE;
        info[5] = 1;
        return 0;
    }
    {
        page_t f = s_pages[page / PAGE];
        while (end < s_top && s_pages[end / PAGE].owner == f.owner &&
               s_pages[end / PAGE].state == f.state && s_pages[end / PAGE].protect == f.protect)
            end += PAGE;
        info[1] = f.owner;
        info[2] = f.alloc_protect;
        info[3] = end - page;
        info[4] = f.state;
        info[5] = f.protect;
        info[6] = T_PRIVATE;
    }
    return 0;
}
