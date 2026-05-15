# Transform-only C baseline: load once, time N=216,000 column-op iters, write out.
# Same flags as etl/c_baseline so the compiler is given every chance.

CC      ?= cc
CFLAGS  ?= -O3 -march=native -Wall -Wextra -std=c11
OUT_DIR ?= bin

TASKS = task_1_derived_columns task_2_filter_invalid task_3_bucket_timestamps task_4_aggregate_region task_5_pipeline task_2_branchless task_5_branchless

all: $(addprefix $(OUT_DIR)/,$(TASKS))

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/%: %.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OUT_DIR)

.PHONY: all clean
