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
#include "resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Best-effort recursive mkdir (like `mkdir -p`); ignores already-exists. */
static void mkdir_p(const char *path) {
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

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

/* Build + run the GPU probe. Returns 1 if a real GPU dispatch happened (a usable DEVICE is present),
 * 0 otherwise (no device / no glslc / CPU fallback). On presence, fills `launch_us` (the non-resident
 * per-dispatch overhead — launch + a round-trip, the transfer-inclusive constant the cost model uses) and
 * `gpu_gflops` (measured on a RESIDENT pool so compute is isolated from transfer; 0 if immeasurable).
 *
 * DEVICE PRESENCE is decided by whether the GPU actually dispatched — NOT by whether the probe kernel beat
 * its overhead. A working GPU that is overhead-bound on a tiny non-resident kernel is still a usable device;
 * throughput is characterized separately, on a resident pool where a heavy kernel's compute is measurable. */
static int measure_gpu(double *launch_us, double *gpu_gflops) {
	/* GENTLE by design: a tiny pool and few dispatches, so the probe can't saturate a GPU that is also
	 * driving the display. Two @gpu arms: `lnr` (light over a NON-resident pool → launch + a round-trip per
	 * dispatch → the overhead constant) and `hr` (heavy over a @resident pool → its effective throughput).
	 * Nanosecond timing (raw clock_mono) makes the small workload measurable without piling on dispatches. */
	const long BIGN = 1 << 18; /* 256K rows — ~1 MB/column, a light PCIe footprint */
	const int K = 32;
	const double HEAVY_FLOPS = 64.0; /* the heavy arm's per-element op count (matches its body) */

	char seq_lnr[2048], seq_hr[2048]; /* K × "name, " each */
	seq_lnr[0] = seq_hr[0] = 0;
	append_reps(seq_lnr, sizeof(seq_lnr), "lnr", K);
	append_reps(seq_hr, sizeof(seq_hr), "hr", K);

	/* The heavy body over the resident column `cr`: ONE assignment with a balanced nested Horner expression of
	 * N = HEAVY_FLOPS/2 levels (each `(…)*k + k` = 2 flops). A single write keeps the emitted GLSL simple. */
	const int N = (int)(HEAVY_FLOPS / 2);
	char heavy_body[2048];
	int n = snprintf(heavy_body, sizeof(heavy_body), "  cr = ");
	for (int i = 0; i < N; i++)
		n += snprintf(heavy_body + n, sizeof(heavy_body) - (size_t)n, "(");
	n += snprintf(heavy_body + n, sizeof(heavy_body) - (size_t)n, "cr");
	for (int i = 0; i < N; i++)
		n += snprintf(heavy_body + n, sizeof(heavy_body) - (size_t)n, ")*1.0001+0.0001");
	snprintf(heavy_body + n, sizeof(heavy_body) - (size_t)n, ";\n");

	/* Timers read the raw monotonic timespec {sec, nsec} and store nanoseconds, so a sub-millisecond arm is
	 * still measurable. */
	char src[1 << 17];
	snprintf(src, sizeof(src),
	         "#import { fmt os }\n"
	         "c :: float;\n"
	         "P :: arche { c }\n"
	         "[%ld]P(%ld);\n"
	         "cr :: float;\n"
	         "PR :: arche { cr }\n"
	         "@resident\n"
	         "[%ld]PR(%ld);\n"
	         "ns :: i64;\n"
	         "T :: arche { ns }\n"
	         "[4]T(4);\n"
	         "@gpu\n"
	         "lnr :: map (query { c }) (c) { c = c + 1.0; }\n"
	         "@gpu\n"
	         "hr :: map (query { cr }) (cr) {\n%s}\n"
	         "seed :: system { P.c = { 1.0 }; PR.cr = { 1.0 }; }\n"
	         "t0 :: system eff { os.clock_mono()(ts:); T.ns[0] = ts[0] * 1000000000 + ts[1]; }\n"
	         "t1 :: system eff { os.clock_mono()(ts:); T.ns[1] = ts[0] * 1000000000 + ts[1]; }\n"
	         "t2 :: system eff { os.clock_mono()(ts:); T.ns[2] = ts[0] * 1000000000 + ts[1]; }\n"
	         "t3 :: system eff { os.clock_mono()(ts:); T.ns[3] = ts[0] * 1000000000 + ts[1]; }\n"
	         "report :: system eff {\n"
	         "  fmt.printf(\"PROBE %%d %%d\\n\", T.ns[1] - T.ns[0], T.ns[3] - T.ns[2]);\n"
	         "}\n"
	         "#run seq({ seed, t0, %s t1, t2, %s t3, report })\n",
	         BIGN, BIGN, BIGN, BIGN, heavy_body, seq_lnr, seq_hr);

	/* Write the probe, build it with --gpu, run it (GPU debug on, wrapped in a hard timeout so a stuck GPU
	 * can never hang calibrate) and parse the dispatch marker (device presence) + "PROBE <nonres_ns>
	 * <res_heavy_ns>" (the timings). */
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

	snprintf(cmd, sizeof(cmd), "timeout 30 env ARCHE_GPU_DEBUG=1 %s 2>&1", exe);
	FILE *p = popen(cmd, "r");
	if (!p)
		return 0;
	int dispatched = 0;
	long nonres_ns = -1, hr_ns = -1;
	char line[256];
	while (fgets(line, sizeof(line), p)) {
		if (strstr(line, "gpu dispatch"))
			dispatched = 1; /* a real device ran the kernel — presence is THIS, not the timing */
		sscanf(line, "PROBE %ld %ld", &nonres_ns, &hr_ns);
	}
	pclose(p);

	if (!dispatched)
		return 0; /* no usable device (or CPU fallback) — honestly CPU-only */

	/* Overhead constant: the non-resident per-dispatch time (launch + a round-trip), microseconds. */
	*launch_us = (nonres_ns > 0) ? (double)nonres_ns / K / 1000.0 : 0.0;

	/* Effective GPU throughput: total heavy flops / heavy-arm wall time (Gflop/s = flops / ns). This folds in
	 * launch + memory (a memory-bound kernel measures lower than peak ALU), which is exactly the "effective"
	 * number the cost model wants. 0 if the arm didn't time — device present, but the estimate won't
	 * auto-place (only `@gpu`/measured/forced use the GPU). */
	*gpu_gflops = (hr_ns > 0) ? (HEAVY_FLOPS * (double)BIGN * (double)K) / (double)hr_ns : 0.0;
	return 1;
}

int calibrate_run(int argc, char **argv, const GlobalOpts *g) {
	(void)argc;
	(void)argv;
	(void)g;
	/* Write target: $ARCHE_CACHE_DIR when set (install points it at the machine-global lib/arche; tests use
	 * their own dir); otherwise the per-user cache (~/.cache/arche), which `arche build` discovers by default.
	 * So a bare `arche calibrate` "just works" — no env juggling. */
	const char *cdir = getenv("ARCHE_CACHE_DIR");
	char udir[1024];
	if (!cdir || !cdir[0]) {
		if (!arche_user_cache_dir(udir, sizeof(udir))) {
			fprintf(stderr, "arche calibrate: no ARCHE_CACHE_DIR set and no HOME/XDG_CACHE_HOME to default to\n");
			return ARCHE_ERR;
		}
		cdir = udir;
	}
	mkdir_p(cdir);

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
		if (gpu_gflops > 0)
			printf("  gpu_present = 1  launch_us(+transfer) = %.1f  gpu_gflops = %.1f\n", launch_us, gpu_gflops);
		else
			printf("  gpu_present = 1  launch_us(+transfer) = %.1f  gpu_gflops = 0 (throughput not measurable; "
			       "`@gpu`/measured maps still use the GPU, but the estimate won't auto-place)\n",
			       launch_us);
	} else {
		mp.gpu_present = 0;
		printf("  gpu_present = 0 — no usable device (no dispatch), or no glslc; placement is CPU-only\n");
	}

	if (!codegen_save_machine_profile(cdir, &mp)) {
		fprintf(stderr, "arche calibrate: could not write %s/machine.profile\n", cdir);
		return ARCHE_ERR;
	}
	printf("Wrote %s/machine.profile\n", cdir);
	return ARCHE_OK;
}
