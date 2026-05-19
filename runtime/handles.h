/* Arche extern-type handle tables — runtime helper declarations.
 *
 * Each `extern type T(N)` produces (via codegen) a static slot array:
 *   @__arche_T_slots = internal global [N x %__ArcheSlot] zeroinitializer
 *
 * The three helpers below operate on those arrays. Handles are int32_t with
 * the layout:
 *   bits 31..16 : generation counter (uint16)
 *   bits 15..0  : slot index + 1 (uint16; 0 = null handle)
 *
 * Definitions live in runtime/handles.c; include this header in any C
 * translation unit that needs to call the helpers.
 *
 * All failures abort() after printing a single line to stderr.
 */

#ifndef ARCHE_HANDLES_H
#define ARCHE_HANDLES_H

#include <stdint.h>

typedef struct {
	void *ptr;
	uint16_t gen;
	uint16_t in_use;
} __ArcheSlot;

/* Allocate a free slot for ptr; return packed handle. Aborts if full. */
int32_t __arche_slot_alloc(const char *type_name, __ArcheSlot *slots, int capacity, void *ptr);

/* Decode and validate handle; return ptr or abort on bad/stale handle. */
void *__arche_slot_get(const char *type_name, __ArcheSlot *slots, int capacity, int32_t handle);

/* Free slot; bump generation. No-op on null handle. */
void __arche_slot_free(const char *type_name, __ArcheSlot *slots, int capacity, int32_t handle);

#endif /* ARCHE_HANDLES_H */
