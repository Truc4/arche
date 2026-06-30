#ifndef CODEGEN_H
#define CODEGEN_H

#include "../hir/hir.h"
#include "../semantic/semantic.h"
#include <stdio.h>

/* Code generator context */
typedef struct CodegenContext CodegenContext;

/* Create, generate, and free codegen context */
CodegenContext *codegen_create(HirProgram *ast, SemanticContext *sem_ctx);
void codegen_generate(CodegenContext *ctx, FILE *output);
/* True if codegen emitted a hard error (diagnostics already printed to stderr). The caller must
 * fail the build and not run the emitted IR. */
int codegen_had_error(const CodegenContext *ctx);
void codegen_free(CodegenContext *ctx);

/* Per-unit codegen: restrict this context to emit only `unit`'s bodies (cross-unit callees are
 * declared). Implies per-unit mode. Used by the per-unit driver in compile.c. */
void codegen_set_emit_unit(CodegenContext *ctx, int unit);
/* `--unchecked`: strip implicit runtime bounds checks (unannotated fallible ops become `!undefined`).
 * Explicit policies are still honored. Set before codegen runs. */
void codegen_set_unchecked(int on);
/* `--incremental`: force per-unit (device-granular) codegen — each compilation unit (driver = unit 0,
 * each imported device = unit N) is opt/llc/cc'd to its own content-hash-cached object, so an
 * unchanged device is reused verbatim. Equivalent to the ARCHE_PER_UNIT env. Set before codegen runs.
 * `on == 0` resets to auto (the env decides), NOT a hard whole-program. */
void codegen_set_per_unit(int on);
/* `--whole-program`: a HARD override forcing whole-program codegen — beats both `codegen_set_per_unit`
 * and the ARCHE_PER_UNIT env. Use for the explicit opt-out (`arche run --whole-program`). */
void codegen_force_whole_program(void);
/* 1 if per-unit codegen is requested (--incremental flag OR the ARCHE_PER_UNIT env) — the driver
 * consults this to choose the device-granular pipeline. */
int codegen_per_unit_enabled(void);

/* `--shared`: arche-owned defs get external (dlsym-able) linkage so a device's procs are reachable in a
 * loadable `.so`. The CLI also forces whole-program when this is set (one clean export surface). */
void codegen_set_shared(int on);
int codegen_shared_enabled(void);

/* Dev hot-reload (`arche run`): route the driver's cross-unit (device) calls through a reload trampoline
 * (internal indirect call). `arche build` leaves it off → direct calls, no runtime fn-pointers. */
void codegen_set_hot(int on);
int codegen_hot_enabled(void);

/* `--gpu`: a `run map @gpu` tries the embedded compute shader on the GPU, falling back to the direct CPU
 * call on failure. Off (default) → `@gpu` builds are pure CPU (no Vulkan dependency, core suite stays
 * GPU-free). See runtime/gpu_runtime.c and codegen/gpu_embed.c. */
void codegen_set_gpu(int on);
int codegen_gpu_enabled(void);

/* ===== Derived placement (Slice 4): per-machine cost profile =====
 * Placement (CPU vs GPU per eligible map) is DERIVED from the kernel signature + a per-machine cost
 * profile, decided at build time and FROZEN into the schedule (no runtime scheduler). The profile is
 * measured once per machine (calibration) and cached; static pools supply the row counts, so every cost
 * input is known at build time. `@gpu` remains a force-GPU override. */
typedef struct {
	int gpu_present;       /* 0 ⇒ no usable device ⇒ CPU-only placement */
	double gpu_launch_us;  /* fixed per-dispatch overhead (microseconds) */
	double pcie_up_gbps;   /* host→device transfer bandwidth (GB/s) */
	double pcie_down_gbps; /* device→host transfer bandwidth (GB/s) */
	double cpu_gflops;     /* CPU arithmetic throughput (Gflop/s) */
	double gpu_gflops;     /* GPU arithmetic throughput (Gflop/s) */
} MachineProfile;

/* A conservative CPU-only default (gpu_present=0) — used when no profile is cached. */
void codegen_default_machine_profile(MachineProfile *out);
/* Set the active build's profile (copied); the placement decision reads it. Call before codegen. */
void codegen_set_machine_profile(const MachineProfile *p);
/* Load <cache_dir>/machine.profile into `out` (1 on success; 0 ⇒ caller should default/calibrate). */
int codegen_load_machine_profile(const char *cache_dir, MachineProfile *out);
/* Write the profile to <cache_dir>/machine.profile (1 on success). */
int codegen_save_machine_profile(const char *cache_dir, const MachineProfile *p);

/* Test hook (tests/unit/compiler/codegen_tests.c). codegen_create malloc's its context and is reused
 * in-process across many compiles (the `arche test` doctest runner); it MUST zero-init every field it
 * relies on. This poisons an exact-size heap chunk, frees it, then lets codegen_create reuse that block
 * and verifies entity_bind_count came back 0 (not the poison's garbage, which would send a func body's
 * name-expr scan off the end of entity_binds[256] into unmapped memory). Returns 1 on success. */
int codegen_selftest_context_zero_init(void);

#endif /* CODEGEN_H */
