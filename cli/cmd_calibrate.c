#define _POSIX_C_SOURCE 200809L
/* `arche calibrate` — measure this machine's cost constants once and cache them, so `arche build` can
 * DERIVE CPU/GPU placement per eligible map (Slice 4). The decision is made at build time and frozen — no
 * runtime scheduler. CPU throughput is timed in-process; the GPU constants are measured by building+running
 * a tiny probe through the normal `--gpu` pipeline (so the compiler itself stays Vulkan-free). A box with
 * no usable device writes `gpu_present 0` (honest CPU-only placement).
 *
 * This is a v1: the GPU model is a fixed per-dispatch overhead (launch + the non-resident PCIe round-trip,
 * lumped at the probe's pool size) plus measured throughput. Per-column residency (Slice 5) and a finer
 * size-scaled transfer model are follow-ons. */
#include "../codegen/codegen.h"
#include "../compile/compile.h"
#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Time independent fused-multiply-adds → CPU Gflop/s. Eight parallel accumulators keep the FP units busy
 * (a single dependent chain would measure latency, not throughput). `volatile` sink defeats DCE. */
static double measure_cpu_gflops(void) {
	volatile double sink = 0;
	double a = 1.0000001, b = 0.9999999;
	double x[8] = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7};
	long iters = 40000000; /* × 8 lanes × 2 flops = 6.4e8 flops */
	double t0 = now_s();
	for (long i = 0; i < iters; i++)
		for (int j = 0; j < 8; j++)
			x[j] = x[j] * a + b;
	double dt = now_s() - t0;
	for (int j = 0; j < 8; j++)
		sink += x[j];
	(void)sink;
	if (dt <= 0)
		return 0;
	return (2.0 * 8.0 * (double)iters) / (dt * 1e9);
}

/* The probe schedule repeats each `@gpu` map K times between timer reads; build the seq leaves. */
static void append_reps(char *buf, size_t cap, const char *name, int k) {
	size_t n = strlen(buf);
	for (int i = 0; i < k && n + 64 < cap; i++)
		n += (size_t)snprintf(buf + n, cap - n, "%s, ", name);
}

/* Build + run the GPU probe; on success fill `launch_us` (fixed per-dispatch overhead) and `gpu_gflops`.
 * Returns 1 if the probe ran on the GPU, 0 otherwise (no device / no glslc / CPU fallback). */
static int measure_gpu(double *launch_us, double *gpu_gflops) {
	const long BIGN = 1 << 20; /* ~1M rows */
	const int K = 200;
	const double HEAVY_FLOPS = 64.0; /* the heavy arm's per-element op count (matches its body) */

	char seq_light[4096]; /* K × "light, " (~7 chars) */
	char seq_heavy[4096];
	seq_light[0] = seq_heavy[0] = 0;
	append_reps(seq_light, sizeof(seq_light), "light", K);
	append_reps(seq_heavy, sizeof(seq_heavy), "heavy", K);

	/* The heavy body: ONE assignment with a balanced nested Horner expression of N = HEAVY_FLOPS/2 levels
	 * (each level `(…)*k + k` = 2 flops). A single write keeps the emitted GLSL simple (many sequential
	 * writes to one column break glslc); the deep expression supplies the flops. */
	const int N = (int)(HEAVY_FLOPS / 2);
	char heavy_body[2048];
	int n = snprintf(heavy_body, sizeof(heavy_body), "  c = ");
	for (int i = 0; i < N; i++)
		n += snprintf(heavy_body + n, sizeof(heavy_body) - (size_t)n, "(");
	n += snprintf(heavy_body + n, sizeof(heavy_body) - (size_t)n, "c");
	for (int i = 0; i < N; i++)
		n += snprintf(heavy_body + n, sizeof(heavy_body) - (size_t)n, ")*1.0001+0.0001");
	snprintf(heavy_body + n, sizeof(heavy_body) - (size_t)n, ";\n");

	char src[1 << 17];
	snprintf(src, sizeof(src),
	         "#import { fmt os }\n"
	         "c :: float;\n"
	         "P :: arche { c }\n"
	         "[%ld]P(%ld);\n"
	         "ms :: i64;\n"
	         "T :: arche { ms }\n"
	         "[4]T(4);\n"
	         "@gpu\n"
	         "light :: map (query { c }) (c) { c = c + 1.0; }\n"
	         "@gpu\n"
	         "heavy :: map (query { c }) (c) {\n%s}\n"
	         "seed :: system { P.c = { 1.0 }; }\n"
	         "t0l :: system eff { os.now_ms()(now:); T.ms[0] = now; }\n"
	         "t1l :: system eff { os.now_ms()(now:); T.ms[1] = now; }\n"
	         "t0h :: system eff { os.now_ms()(now:); T.ms[2] = now; }\n"
	         "t1h :: system eff { os.now_ms()(now:); T.ms[3] = now; }\n"
	         "report :: system eff {\n"
	         "  fmt.printf(\"PROBE %%d %%d\\n\", T.ms[1] - T.ms[0], T.ms[3] - T.ms[2]);\n"
	         "}\n"
	         "#run seq({ seed, t0l, %s t1l, t0h, %s t1h, report })\n",
	         BIGN, BIGN, heavy_body, seq_light, seq_heavy);

	/* Write the probe, build it with --gpu, run it, parse "PROBE <light_ms> <heavy_ms>". */
	char dir[] = "/tmp/arche_calib_XXXXXX";
	if (!mkdtemp(dir))
		return 0;
	char spath[512], exe[512], cmd[1200];
	snprintf(spath, sizeof(spath), "%s/probe.arche", dir);
	snprintf(exe, sizeof(exe), "%s/probe", dir);
	FILE *sf = fopen(spath, "w");
	if (!sf)
		return 0;
	fputs(src, sf);
	fclose(sf);

	CompileOpts opts = {0};
	opts.gpu = 1;
	opts.quiet = 1;
	int rc = compile_source(src, spath, exe, &opts);
	if (rc != 0)
		return 0;

	snprintf(cmd, sizeof(cmd), "%s 2>/dev/null", exe);
	FILE *p = popen(cmd, "r");
	if (!p)
		return 0;
	long light_ms = -1, heavy_ms = -1;
	char line[256];
	while (fgets(line, sizeof(line), p))
		if (sscanf(line, "PROBE %ld %ld", &light_ms, &heavy_ms) == 2)
			break;
	pclose(p);
	/* heavy ≈ light ⇒ compute is immeasurable above the per-dispatch overhead (an overhead-dominated GPU,
	 * or CPU fallback). We can't characterize a throughput benefit, so we decline to auto-place on this
	 * device (CPU is the always-legal home). A box where the GPU clearly pays shows heavy ≫ light. */
	if (light_ms < 0 || heavy_ms < 0 || heavy_ms <= light_ms)
		return 0;

	double light_per = (double)light_ms / K;             /* ms per dispatch: launch + transfer (≈ overhead) */
	double heavy_per = (double)heavy_ms / K;             /* ms per dispatch: overhead + compute             */
	double compute_ms = heavy_per - light_per;           /* ms of compute per dispatch                      */
	if (compute_ms <= 0)
		return 0;
	*launch_us = light_per * 1000.0;                                          /* fixed overhead, microseconds */
	*gpu_gflops = (HEAVY_FLOPS * (double)BIGN) / (compute_ms * 1e-3 * 1e9);   /* Gflop/s                      */
	return 1;
}

int calibrate_run(int argc, char **argv, const GlobalOpts *g) {
	(void)argc;
	(void)argv;
	(void)g;
	const char *cdir = getenv("ARCHE_CACHE_DIR");
	if (!cdir || !cdir[0]) {
		fprintf(stderr, "arche calibrate: set ARCHE_CACHE_DIR to the directory the profile should be written "
		                "to (the same one `arche build` reads)\n");
		return ARCHE_ERR;
	}

	MachineProfile mp;
	codegen_default_machine_profile(&mp);
	printf("Measuring CPU throughput...\n");
	mp.cpu_gflops = measure_cpu_gflops();
	printf("  cpu_gflops = %.1f\n", mp.cpu_gflops);

	printf("Probing GPU (build + run a tiny @gpu kernel)...\n");
	double launch_us = 0, gpu_gflops = 0;
	if (measure_gpu(&launch_us, &gpu_gflops)) {
		mp.gpu_present = 1;
		mp.gpu_launch_us = launch_us;
		mp.pcie_up_gbps = 1e6;   /* transfer folded into the fixed per-dispatch overhead (v1) */
		mp.pcie_down_gbps = 1e6;
		mp.gpu_gflops = gpu_gflops;
		printf("  gpu_present = 1  launch_us(+transfer) = %.1f  gpu_gflops = %.1f\n", launch_us, gpu_gflops);
	} else {
		mp.gpu_present = 0;
		printf("  gpu_present = 0 — no usable device, no glslc, or the GPU is overhead-dominated (the probe's "
		       "compute didn't beat its per-dispatch cost); placement is CPU-only\n");
	}

	if (!codegen_save_machine_profile(cdir, &mp)) {
		fprintf(stderr, "arche calibrate: could not write %s/machine.profile\n", cdir);
		return ARCHE_ERR;
	}
	printf("Wrote %s/machine.profile\n", cdir);
	return ARCHE_OK;
}
