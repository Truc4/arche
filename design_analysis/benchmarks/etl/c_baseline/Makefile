# Hand-written C baseline for the ETL benchmarks.
# Single-pass mmap + manual parse + accumulate.
# Compile: -O3 -march=native to give the compiler every chance.

CC      ?= cc
CFLAGS  ?= -O3 -march=native -Wall -Wextra -std=c11
OUT_DIR ?= bin

TASKS = task_1_derived_columns task_2_filter_invalid task_3_bucket_timestamps task_4_aggregate_region task_5_pipeline

all: $(addprefix $(OUT_DIR)/,$(TASKS))

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/%: %.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OUT_DIR)

.PHONY: all clean
