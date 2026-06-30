/* Embed `@gpu` map shaders into a `--gpu` build.
 *
 * The file emitter (gpu_glsl.c, `--emit-gpu`) writes `.comp` files as a side artifact. This sibling
 * goes the rest of the way for an actual GPU build: it compiles each map's GLSL to SPIR-V with `glslc`
 * and generates a C registry (byte arrays + `arche_gpu_lookup`) that compile.c links beside the Vulkan
 * dispatcher (runtime/gpu_runtime.c). codegen emits `arche_gpu_dispatch(name, ...)`; the dispatcher
 * looks the name up here, so a name with no embedded shader simply falls back to the CPU path.
 *
 * Graceful degradation: with no `glslc` on PATH, an empty registry is written and the build proceeds
 * (everything runs on the CPU). A `glslc` that IS present but fails to compile a shader is a hard error
 * (an emitter bug), not silently skipped. */

#define _POSIX_C_SOURCE 200809L

#include "gpu_embed.h"
#include "gpu_glsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int glslc_available(void) {
	return system("command -v glslc >/dev/null 2>&1") == 0;
}

/* Read a whole file into a malloc'd buffer; *len gets the byte count. NULL on failure. */
static unsigned char *read_all(const char *path, long *len) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	unsigned char *buf = malloc((size_t)n ? (size_t)n : 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	*len = n;
	return buf;
}

/* Emit `static const unsigned char <sym>[] = { ... };` for the SPIR-V bytes. */
static void emit_bytes(FILE *out, const char *sym, const unsigned char *b, long n) {
	fprintf(out, "static const unsigned char %s[] = {", sym);
	for (long i = 0; i < n; i++)
		fprintf(out, "%s%u", (i == 0) ? "\n  " : ((i % 16 == 0) ? ",\n  " : ","), (unsigned)b[i]);
	fprintf(out, "\n};\n");
}

int arche_gpu_embed(HirProgram *prog, const char *out_c_path, int quiet) {
	if (!prog || !out_c_path)
		return -1;
	gpu_glsl_mark_runs(prog);

	FILE *out = fopen(out_c_path, "w");
	if (!out) {
		fprintf(stderr, "arche: could not write GPU registry %s\n", out_c_path);
		return -1;
	}
	/* Self-contained: this struct layout mirrors ArcheGpuShader in runtime/arche_gpu.h. */
	fprintf(out, "/* generated GPU shader registry — do not edit */\n");
	fprintf(out, "#include <stddef.h>\n");
	fprintf(out, "typedef struct { const char *name; const unsigned char *spv; size_t spv_len; unsigned ncol; }"
	             " ArcheGpuShader;\n\n");

	int embedded = 0;
	int have_glslc = glslc_available();
	char workdir[] = "/tmp/arche_gpu_XXXXXX";
	int have_workdir = 0;
	if (have_glslc) {
		if (mkdtemp(workdir))
			have_workdir = 1;
		else
			have_glslc = 0; /* no temp dir → behave as if glslc absent (CPU fallback) */
	}

	/* Collect entries as we go so the table can be written after the byte arrays. */
	struct {
		const char *name;
		int ncol;
	} entries[256];
	int nentries = 0;

	if (have_glslc) {
		for (int i = 0; i < prog->decl_count; i++) {
			HirDecl *d = prog->decls[i];
			if (!d || d->kind != HIR_DECL_KERNEL || !d->data.kernel ||
			    d->data.kernel->kind != HIR_KERNEL_MAP || !d->data.kernel->is_gpu)
				continue;
			HirKernelDecl *map = d->data.kernel;
			HirArchetypeDecl *arch = gpu_glsl_first_float_arch(prog, map);
			if (!arch)
				continue; /* no GPU form (e.g. non-float columns) → CPU-only */
			/* Names ride into fixed buffers (sym/comp/spv) and a C identifier; an absurdly long one would
			 * truncate-and-collide. Guard explicitly and leave the map CPU-only with a note. */
			if (!map->name || strlen(map->name) > 200) {
				fprintf(stderr, "arche: note: `@gpu` map name too long to embed; runs on CPU\n");
				continue;
			}
			if (nentries >= 256) {
				fprintf(stderr, "arche: note: more than 256 `@gpu` maps; the rest run on CPU\n");
				break;
			}
			char *src = gpu_glsl_build_src(map, arch);
			if (!src)
				continue;

			char comp[600], spv[600], cmd[1400];
			snprintf(comp, sizeof(comp), "%s/%s.comp", workdir, map->name);
			snprintf(spv, sizeof(spv), "%s/%s.spv", workdir, map->name);
			FILE *cf = fopen(comp, "w");
			if (!cf) {
				free(src);
				goto fail;
			}
			fputs(src, cf);
			fclose(cf);
			free(src);

			snprintf(cmd, sizeof(cmd), "glslc -fshader-stage=compute '%s' -o '%s' 2>/dev/null", comp, spv);
			if (system(cmd) != 0) {
				fprintf(stderr, "arche: glslc failed compiling GPU shader for map '%s'\n", map->name);
				unlink(comp);
				goto fail;
			}
			long n = 0;
			unsigned char *bytes = read_all(spv, &n);
			unlink(comp);
			unlink(spv);
			if (!bytes || n <= 0) {
				fprintf(stderr, "arche: could not read SPIR-V for map '%s'\n", map->name);
				free(bytes);
				goto fail;
			}
			char sym[300];
			snprintf(sym, sizeof(sym), "spv_%s", map->name);
			emit_bytes(out, sym, bytes, n);
			free(bytes);
			entries[nentries].name = map->name;
			entries[nentries].ncol = map->param_count;
			nentries++;
			embedded++;
		}
	}
	if (have_workdir)
		rmdir(workdir);

	/* Strict C99 has no empty/zero-length array, so size the table to at least 1 (a zero sentinel that the
	 * count keeps out of reach). Lookup is one uniform scan bounded by the count. */
	fprintf(out, "\nstatic const ArcheGpuShader arche_gpu_shaders[%d] = {\n", nentries > 0 ? nentries : 1);
	for (int i = 0; i < nentries; i++)
		fprintf(out, "  { \"%s\", spv_%s, sizeof(spv_%s), %d },\n", entries[i].name, entries[i].name, entries[i].name,
		        entries[i].ncol);
	if (nentries == 0)
		fprintf(out, "  { (void *)0, (void *)0, 0, 0 },\n");
	fprintf(out, "};\n");
	fprintf(out, "static const unsigned arche_gpu_shader_count = %d;\n\n", nentries);

	fprintf(out, "#include <string.h>\n");
	fprintf(out, "const ArcheGpuShader *arche_gpu_lookup(const char *name) {\n");
	fprintf(out, "  for (unsigned i = 0; i < arche_gpu_shader_count; i++)\n");
	fprintf(out, "    if (name && arche_gpu_shaders[i].name && strcmp(name, arche_gpu_shaders[i].name) == 0)\n");
	fprintf(out, "      return &arche_gpu_shaders[i];\n");
	fprintf(out, "  return (void *)0;\n");
	fprintf(out, "}\n");

	fclose(out);
	if (!quiet && embedded > 0)
		printf("Embedded %d GPU compute shader(s) into the executable\n", embedded);
	else if (!quiet && !have_glslc)
		printf("note: glslc not found — `--gpu` build will run @gpu maps on the CPU\n");
	return embedded;

fail:
	/* A hard error mid-embed (temp I/O or glslc failure): the per-map temp files are already unlinked, so
	 * the work dir is empty — drop it and the half-written registry, fail the build. */
	if (have_workdir)
		rmdir(workdir);
	fclose(out);
	return -1;
}
