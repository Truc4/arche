#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Transaction archetype struct definition (matches LLVM layout) */
typedef struct {
	double *price;
	int *quantity;
	long count;
	long capacity;
	long *free_list;
	long free_count;
} Transaction;

/* External global variable for Transaction archetype */
extern Transaction *archetype_Transaction;

/* Load CSV and populate archetype */
void load_transaction_csv(void) {
	FILE *file = fopen("data/transactions.csv", "r");
	if (!file) {
		fprintf(stderr, "Failed to open CSV file\n");
		return;
	}

	char line[256];
	int row = 0;

	/* Skip header */
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return;
	}

	/* Read data rows */
	while (fgets(line, sizeof(line), file) && row < 3) {
		double price;
		int quantity;

		if (sscanf(line, "%lf,%d", &price, &quantity) == 2) {
			archetype_Transaction->price[row] = price;
			archetype_Transaction->quantity[row] = quantity;
			row++;
		}
	}

	archetype_Transaction->count = row;
	fclose(file);
}
