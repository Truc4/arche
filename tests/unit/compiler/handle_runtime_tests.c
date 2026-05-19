#include "../../../runtime/handles.h"
#include <assert.h>
#include <stdio.h>

static __ArcheSlot slots[4];

int main(void) {
	/* alloc, get, free, generation increments */
	int dummy = 7;
	int32_t h1 = __arche_slot_alloc("T", slots, 4, &dummy);
	assert(h1 != 0);
	assert(__arche_slot_get("T", slots, 4, h1) == &dummy);
	__arche_slot_free("T", slots, 4, h1);

	int32_t h2 = __arche_slot_alloc("T", slots, 4, &dummy);
	assert(h2 != h1); /* different generation */

	__arche_slot_free("T", slots, 4, 0); /* no-op */
	printf("handle_runtime_tests: OK\n");
	return 0;
}
