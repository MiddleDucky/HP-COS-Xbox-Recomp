/* guest_vad: reserve and commit bookkeeping for the guest's low memory, the same
 * answers NtAllocateVirtualMemory, NtFreeVirtualMemory and NtQueryVirtualMemory give
 * on the console. Bookkeeping only: the memory itself stays with the allocator.
 * RECOMP_VAD=1 turns it on. */
#ifndef GUEST_VAD_H
#define GUEST_VAD_H
#include <stdint.h>

int      guest_vad_enabled(void);
void     guest_vad_init(uint32_t top);
/* Records a new region. Rounds *size up to pages. Commits it too when commit is set. */
void     guest_vad_reserve(uint32_t base, uint32_t *size, uint32_t protect, int commit);
/* MEM_COMMIT alone. New pages go through zero(), as the console hands out zeroed pages. */
uint32_t guest_vad_commit(uint32_t *base, uint32_t *size, uint32_t protect,
                          void (*zero)(uint32_t va, uint32_t bytes));
/* MEM_DECOMMIT or MEM_RELEASE. *released is set when a whole region is gone. */
uint32_t guest_vad_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                        void (*zero)(uint32_t va, uint32_t bytes), int *released);
/* BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type */
uint32_t guest_vad_query(uint32_t address, uint32_t info[7]);
#endif
