#include "compile.h"
#include "../cli/resource.h"
#include "../codegen/codegen.h"
#include "../codegen/gpu_embed.h"
#include "../codegen/gpu_glsl.h"
#include "../lexer/lexer.h"
#include "../lower/lower.h"
#include "../parser/parser.h"
#include "../semantic/sem_diagnostics.h"
#include "../semantic/semantic.h"
#include "module_resolve.h"
#include "variant_select.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif

#ifndef ARCHE_STDLIB_DIR
#define ARCHE_STDLIB_DIR "stdlib"
#endif

#ifndef ARCHE_RUNTIME_DIR
#define ARCHE_RUNTIME_DIR "build/runtime"
#endif

/* C99 doesn't expose mkdtemp; declare it explicitly (as we do for strdup). */
char *mkdtemp(char *tmpl);

/* ODR verification (unconditional in per-unit mode). Every `define
 * linkonce_odr` symbol emitted in more than one unit module MUST have a byte-identical body — that is
 * the invariant the linker's duplicate-folding relies on. A divergence would be a SILENT miscompile
 * (the linker keeps one arbitrary copy), so this turns the assumption into a checked, loud failure.
 * Returns 1 on any divergence (reported to stderr), 0 if all consistent. */
static int verify_odr(const char *workdir, int max_unit) {
	char **syms = NULL, **bodies = NULL;
	int n = 0, cap = 0, bad = 0;
	for (int u = 0; u <= max_unit; u++) {
		char uf[640];
		snprintf(uf, sizeof(uf), "%s/unit_%d.ll", workdir, u);
		FILE *f = fopen(uf, "r");
		if (!f)
			continue;
		char line[8192];
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "define linkonce_odr ", 20) != 0)
				continue;
			char *at = strchr(line, '@');
			char *lp = at ? strchr(at, '(') : NULL;
			if (!at || !lp)
				continue;
			size_t sl = (size_t)(lp - (at + 1));
			char *sym = malloc(sl + 1);
			memcpy(sym, at + 1, sl);
			sym[sl] = '\0';
			/* Accumulate the whole definition: the `define` line through the closing `}` line. */
			size_t bcap = 16384, blen = 0;
			char *body = malloc(bcap);
			for (;;) {
				size_t ln = strlen(line);
				if (blen + ln + 1 > bcap) {
					bcap = (blen + ln + 1) * 2;
					body = realloc(body, bcap);
				}
				memcpy(body + blen, line, ln + 1);
				blen += ln;
				if (line[0] == '}')
					break;
				if (!fgets(line, sizeof(line), f))
					break;
			}
			int found = -1;
			for (int i = 0; i < n; i++)
				if (strcmp(syms[i], sym) == 0) {
					found = i;
					break;
				}
			if (found >= 0) {
				if (strcmp(bodies[found], body) != 0) {
					fprintf(stderr, "ODR VIOLATION: linkonce_odr symbol @%s differs across units (unit_%d.ll)\n", sym,
					        u);
					bad = 1;
				}
				free(sym);
				free(body);
			} else {
				if (n == cap) {
					cap = cap ? cap * 2 : 64;
					syms = realloc(syms, (size_t)cap * sizeof(char *));
					bodies = realloc(bodies, (size_t)cap * sizeof(char *));
				}
				syms[n] = sym;
				bodies[n] = body;
				n++;
			}
		}
		fclose(f);
	}
	for (int i = 0; i < n; i++) {
		free(syms[i]);
		free(bodies[i]);
	}
	free(syms);
	free(bodies);
	return bad;
}

static char *source_dir_of(const char *path) {
	/* Return directory part of path. If no /, return "." */
	char *last_slash = strrchr(path, '/');
	if (!last_slash) {
		char *dir = malloc(2);
		strcpy(dir, ".");
		return dir;
	}

	int len = last_slash - path;
	char *dir = malloc(len + 1);
	strncpy(dir, path, len);
	dir[len] = '\0';
	return dir;
}

/* The program is non-empty iff the syntax tree root carries at least one declaration node
 * (any SN_*_DECL). A malformed top-level form is wrapped SN_ERROR (parse errors are
 * reported earlier), so it doesn't count as a declaration. */
static int syntax_root_has_decl(const SyntaxNode *syntax_root) {
	if (!syntax_root)
		return 0;
	for (int i = 0; i < syntax_root->child_count; i++) {
		if (syntax_root->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = syntax_root->children[i].as.node->kind;
		if ((k >= SN_WORLD_DECL && k <= SN_USE_DECL) || k == SN_REGION)
			return 1;
	}
	return 0;
}

static char *read_file_optional(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size == 0) {
		fclose(f);
		return malloc(1); /* return empty string */
	}
	char *buf = malloc(size + 2);
	size_t n = fread(buf, 1, size, f);
	fclose(f);
	if (n > 0)
		buf[n] = '\n';
	buf[n + (n > 0 ? 1 : 0)] = '\0';
	return buf;
}

/* Dedup set of already-loaded module names (per compilation; reset in resolve_uses). Marking a
 * name BEFORE loading also makes transitive `#import` cycle-safe. */
#define MAX_LOADED_MODS 256
static char *g_loaded_mods[MAX_LOADED_MODS];
static int g_loaded_count;

/* Names of loaded modules that are DEVICES (their folder has a `.ds.arche` datasheet). A bare-name
 * `#import { x }` must resolve to a device; a plain module is imported by path. Reset in resolve_uses. */
static char *g_device_mods[MAX_LOADED_MODS];
static int g_device_count;

/* Import-resolution errors (rule 1: bare-name import of a non-device). Reset per compilation in
 * resolve_uses; checked by compile_frontend so a violation fails the build. */
static int g_resolve_errors;

/* C shim files (`.c`) discovered in imported device folders / their selected variant subfolders, to
 * compile + link into the final executable (e.g. an X11 glue file). Deduped; reset in resolve_uses
 * (the in-process doctest runner compiles many programs — a leak would mislink the next one). */
static char *g_c_shims[MAX_LOADED_MODS];
static int g_c_shim_count;

static void compile_add_c_shim(void *ctx, const char *path) {
	(void)ctx;
	for (int i = 0; i < g_c_shim_count; i++)
		if (strcmp(g_c_shims[i], path) == 0)
			return;
	if (g_c_shim_count < MAX_LOADED_MODS)
		g_c_shims[g_c_shim_count++] = strcpy(malloc(strlen(path) + 1), path);
}

/* Append the collected device C shims and `#link` `-l<lib>` flags to a cc command being assembled in
 * `cmd` (current length *len, capacity cap). Shims precede the `-l` libs so the linker resolves a
 * shim's library references (left-to-right). Returns 0, or -1 if appending would overflow — the caller
 * fails the link rather than silently drop an input (mirrors the `--link` overflow policy). */
static int append_link_extras(char *cmd, int *len, size_t cap, char libs[][64], int nlib) {
	for (int i = 0; i < g_c_shim_count; i++) {
		int m = snprintf(cmd + *len, cap - (size_t)*len, " %s", g_c_shims[i]);
		if (m < 0 || m >= (int)cap - *len)
			return -1;
		*len += m;
	}
	for (int i = 0; i < nlib; i++) {
		int m = snprintf(cmd + *len, cap - (size_t)*len, " -l%s", libs[i]);
		if (m < 0 || m >= (int)cap - *len)
			return -1;
		*len += m;
	}
	return 0;
}

static void mark_device_module(const char *name) {
	for (int i = 0; i < g_device_count; i++)
		if (strcmp(g_device_mods[i], name) == 0)
			return;
	if (g_device_count < MAX_LOADED_MODS)
		g_device_mods[g_device_count++] = strcpy(malloc(strlen(name) + 1), name);
}

static int module_is_device(const char *name) {
	for (int i = 0; i < g_device_count; i++)
		if (strcmp(g_device_mods[i], name) == 0)
			return 1;
	return 0;
}

/* ---- Per-unit (device-granular) object cache: the "compile this part, leave that alone" engine.
 * In per-unit codegen each compilation unit (unit 0 = the driver, unit N = the Nth imported device)
 * is opt/llc/cc'd to its OWN object, keyed by a content hash of its IR. An unchanged device emits
 * byte-identical IR → same hash → its object is reused verbatim instead of recompiled. */

static void pe_fnv1a_hex(const char *data, size_t n, char out[17]) {
	unsigned long long h = 1469598103934665603ULL; /* FNV-1a */
	for (size_t i = 0; i < n; i++) {
		h ^= (unsigned char)data[i];
		h *= 1099511628211ULL;
	}
	snprintf(out, 17, "%016llx", h);
}

static int pe_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

/* Build (or reuse from cache) the object for one per-unit IR module `unit_ll`. The cache key is a
 * content hash of the IR salted with the toolchain/flag identity, so an unchanged unit's object is
 * reused across builds while a compiler/flag change invalidates everything. Writes the object path to
 * `obj_out` and sets *was_cached. Returns 0 on success, 1 on failure. */
static int build_unit_object_cached(const char *unit_ll, const char *workdir, int u, const char *cache_dir,
                                    char *obj_out, size_t obj_cap, int *was_cached) {
	char *ir = read_file_optional(unit_ll);
	if (!ir)
		return 1;
	char hash[17];
	{
		/* Salt with the exact toolchain + flags the steps below use; bump on any flag change. */
		static const char salt[] = "auc1|" ARCHE_VERSION "|opt-O2-v3|llc-fsec-large-v3|cc-nopie-large";
		size_t sn = sizeof(salt) - 1, n = strlen(ir);
		char *buf = malloc(sn + n);
		memcpy(buf, salt, sn);
		memcpy(buf + sn, ir, n);
		pe_fnv1a_hex(buf, sn + n, hash);
		free(buf);
	}
	free(ir);
	snprintf(obj_out, obj_cap, "%s/%s.o", cache_dir, hash);
	if (pe_exists(obj_out)) {
		*was_cached = 1;
		return 0; /* cache hit — this unit is left alone */
	}
	*was_cached = 0;
	char optf[700], asmf[700], objtmp[800], cmd[2048];
	snprintf(optf, sizeof(optf), "%s/unit_%d_opt.ll", workdir, u);
	snprintf(asmf, sizeof(asmf), "%s/unit_%d.s", workdir, u);
	snprintf(objtmp, sizeof(objtmp), "%s/unit_%d.o", workdir, u);
	snprintf(cmd, sizeof(cmd), "opt -O2 -mcpu=x86-64-v3 -S -o %s %s", optf, unit_ll);
	if (system(cmd) != 0)
		return 1;
	snprintf(cmd, sizeof(cmd), "llc -function-sections -data-sections -code-model=large -mcpu=x86-64-v3 -o %s %s", asmf,
	         optf);
	if (system(cmd) != 0)
		return 1;
	snprintf(cmd, sizeof(cmd), "cc -no-pie -mcmodel=large -c -o %s %s", objtmp, asmf);
	if (system(cmd) != 0)
		return 1;
	/* Publish atomically into the cache (rename; cp across filesystems). */
	if (rename(objtmp, obj_out) != 0) {
		snprintf(cmd, sizeof(cmd), "cp %s %s", objtmp, obj_out);
		if (system(cmd) != 0)
			return 1;
	}
	return 0;
}

/* Build ONE device unit's IR into a position-independent `.so` for dev hot-reload. Thin: no runtime/
 * shims/libs linked — every external (the runtime, device C shims, pools, `arche_hot_resolve`, libc) is
 * resolved from the host at `dlopen` time (the host is linked `-rdynamic`). */
/* Build a device unit's reloadable PIC `.so`, CONTENT-HASH GATED. The dev-loop watcher rebuilds on every
 * edit, but only the edited device's IR actually changes; an unchanged unit's `.so` must be LEFT ALONE so
 * its mtime stays put and the running host's reload check skips it (else every save reloads every device).
 * A sidecar `<out_so>.hash` records the IR hash (salted with the PIC toolchain identity); a hit leaves the
 * `.so` untouched. *rebuilt is set to 1 if the `.so` was (re)written, 0 if reused. */
static int build_unit_so(const char *unit_ll, const char *workdir, int u, const char *out_so, int *rebuilt) {
	if (rebuilt)
		*rebuilt = 0;
	char *ir = read_file_optional(unit_ll);
	if (!ir)
		return 1;
	char hash[17];
	{
		static const char salt[] = "uso1|" ARCHE_VERSION "|opt-O2-v3|llc-pic-small-v3|cc-shared-fpic";
		size_t sn = sizeof(salt) - 1, n = strlen(ir);
		char *buf = malloc(sn + n);
		memcpy(buf, salt, sn);
		memcpy(buf + sn, ir, n);
		pe_fnv1a_hex(buf, sn + n, hash);
		free(buf);
	}
	free(ir);

	char hashf[1400];
	snprintf(hashf, sizeof(hashf), "%s.hash", out_so);
	if (pe_exists(out_so)) {
		char *prev = read_file_optional(hashf);
		int hit = prev && strncmp(prev, hash, 16) == 0;
		free(prev);
		if (hit)
			return 0; /* unchanged unit — leave the .so (and its mtime) alone so the host won't reload it */
	}

	char optf[700], asmf[700], cmd[4096];
	snprintf(optf, sizeof(optf), "%s/unit_%d_opt.ll", workdir, u);
	snprintf(asmf, sizeof(asmf), "%s/unit_%d_pic.s", workdir, u);
	snprintf(cmd, sizeof(cmd), "opt -O2 -mcpu=x86-64-v3 -S -o %s %s", optf, unit_ll);
	if (system(cmd) != 0)
		return 1;
	snprintf(cmd, sizeof(cmd), "llc -relocation-model=pic -code-model=small -mcpu=x86-64-v3 -o %s %s", asmf, optf);
	if (system(cmd) != 0)
		return 1;
	/* Link to a temp then atomically rename into place. The running host polls `out_so`'s mtime and
	 * dlopens it on change; cc writes the `.so` non-atomically, so without this the host can catch a
	 * half-written file mid-rebuild ("dlopen: file too short"). A rename publishes it in one step — the
	 * host only ever sees the old `.so` or the complete new one.
	 * (`-Wl,-z,undefs`, the -shared default, leaves host-provided symbols undefined until load.) */
	char so_tmp[1400];
	snprintf(so_tmp, sizeof(so_tmp), "%s.tmp", out_so);
	snprintf(cmd, sizeof(cmd), "cc -shared -fPIC -o %s %s", so_tmp, asmf);
	if (system(cmd) != 0)
		return 1;
	if (rename(so_tmp, out_so) != 0) {
		snprintf(cmd, sizeof(cmd), "mv -f %s %s", so_tmp, out_so);
		if (system(cmd) != 0)
			return 1;
	}
	/* Record the IR hash so the next watcher pass can skip this unit if it didn't change. */
	FILE *hf = fopen(hashf, "w");
	if (hf) {
		fwrite(hash, 1, 16, hf);
		fclose(hf);
	}
	if (rebuilt)
		*rebuilt = 1;
	return 0;
}

static void load_module(const char *name, const char *source_dir);           /* fwd (mutual recursion) */
static void load_module_from_path(const char *path, const char *source_dir); /* fwd */

/* Load every import named by a `#import` node. An IDENT child is a DEVICE imported by name
 * (`#import io`); a STRING child is a plain MODULE imported by path (`#import { "./util" }`). */
static void load_uses_of(const SyntaxNode *ud, const char *src, const char *source_dir) {
	for (int k = 0; k < ud->child_count; k++) {
		if (ud->children[k].tag != SE_TOKEN)
			continue;
		TokenKind tk = ud->children[k].as.token.kind;
		if (tk != TOK_IDENT && tk != TOK_STRING)
			continue;
		char buf[512];
		size_t L = ud->children[k].as.token.length;
		size_t off = ud->children[k].as.token.offset;
		/* A STRING token spans its quotes — strip them. */
		if (tk == TOK_STRING && L >= 2) {
			off += 1;
			L -= 2;
		}
		if (L > sizeof(buf) - 1)
			L = sizeof(buf) - 1;
		memcpy(buf, src + off, L);
		buf[L] = '\0';
		if (tk == TOK_STRING) {
			load_module_from_path(buf, source_dir);
		} else {
			load_module(buf, source_dir);
			/* Rule 1: a bare name must resolve to a DEVICE (a unit with a `.ds.arche`). A plain module
			 * must be imported by path. `core` is exempt (prepended prelude, never a device). */
			if (strcmp(buf, "core") != 0 && !module_is_device(buf)) {
				fprintf(stderr,
				        "Error: imported '%s' by name but it is not a device (no .ds.arche datasheet) — "
				        "import a plain module by path: #import { \"./%s\" }\n",
				        buf, buf);
				g_resolve_errors++;
			}
		}
	}
}

/* Parse one module file, register its lossless syntax tree with both back-ends (which borrow the syntax tree +
 * source), then recurse into the module's own `#import`s. Returns 1 on success. */
static int register_module_file(const char *mod_name, const char *path, const char *source_dir, DeclOrigin origin) {
	char *mod_src = read_file_optional(path);
	if (!mod_src)
		return 0;
	ParseResult mp = parse_source(mod_src);
	if (mp.error_count > 0 || !mp.syntax_root) {
		fprintf(stderr, "Error: Failed to parse module file %s\n", path);
		for (size_t j = 0; j < mp.error_count; j++)
			fprintf(stderr, "  [Line %d] %s\n", mp.errors[j].line, mp.errors[j].message);
		parse_result_free(&mp);
		free(mod_src);
		return 0;
	}
	lower_add_module(mod_name, mp.syntax_root, mod_src, path);
	semantic_add_module(mod_name, mp.syntax_root, mod_src, path, origin);
	const SyntaxNode *root = mp.syntax_root;
	const char *src = mod_src;
	mp.syntax_root = NULL;
	parse_result_free(&mp); /* mod_src + syntax tree kept alive (borrowed by the registries) */
	/* Transitive imports: a module may `#import` other modules (e.g. csv → io). */
	for (int u = 0; u < root->child_count; u++) {
		if (root->children[u].tag != SE_NODE || root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		load_uses_of(root->children[u].as.node, src, source_dir);
	}
	return 1;
}

/* ---- Module resolution: the SEARCH POLICY (which dir, which files, dedup, variants) lives in the
 * shared resolver (compile/module_resolve.c) so the compiler and the editor analyzer can never
 * disagree about which file backs an `#import`. The compiler supplies its own registration (parse +
 * lower + semantic) and device/dedup bookkeeping via these callbacks. */

static int compile_mark_seen(void *ctx, const char *name) {
	(void)ctx;
	for (int i = 0; i < g_loaded_count; i++)
		if (strcmp(g_loaded_mods[i], name) == 0)
			return 1;
	if (g_loaded_count < MAX_LOADED_MODS)
		g_loaded_mods[g_loaded_count++] = strcpy(malloc(strlen(name) + 1), name); /* mark before load → cycle-safe */
	return 0;
}

static int compile_register_file(void *ctx, const char *mod_name, const char *path, const char *source_dir,
                                 DeclOrigin origin) {
	(void)ctx;
	return register_module_file(mod_name, path, source_dir, origin);
}

static void compile_mark_device(void *ctx, const char *mod_name) {
	(void)ctx;
	mark_device_module(mod_name);
}

/* Active per-device variant selection, rebuilt per compilation in resolve_uses from
 * manifest -> env -> CLI (precedence). The SAME resolution drives the analyzer, so the editor and
 * the build agree on backends. */
static VariantMap g_compile_variants;

static const char *compile_select_variant(void *ctx, const char *mod_name) {
	(void)ctx;
	return variant_map_lookup(&g_compile_variants, mod_name);
}

static const ModuleResolver g_compile_resolver = {
    NULL, compile_mark_seen, compile_register_file, compile_mark_device, compile_select_variant, compile_add_c_shim,
};

/* Load a plain MODULE imported by PATH (`#import { "./util" }`). */
static void load_module_from_path(const char *path, const char *source_dir) {
	if (!arche_module_load_by_path(&g_compile_resolver, path, source_dir))
		fprintf(stderr, "Error: module path not found: %s\n", path);
}

/* Load module `name` (dedup'd) by searching stdlib, then the source dir, then core. */
static void load_module(const char *name, const char *source_dir) {
	if (!arche_module_load_by_name(&g_compile_resolver, name, source_dir))
		fprintf(stderr, "Error: Module not found: %s\n", name);
}

/* Resolve `#import foo` from the syntax tree: locate each module (folder or file), register it with both
 * back-ends, and recurse into its transitive imports. syntax tree + source are kept alive (borrowed). */
static void resolve_uses(const SyntaxNode *syntax_root, const char *src, const char *source_path) {
	if (!syntax_root)
		return;
	/* Static registries — clear leftovers from a prior compilation (the doctest runner compiles
	 * many examples in one process) so modules aren't inlined twice. */
	lower_reset_modules();
	semantic_reset_modules();
	for (int i = 0; i < g_loaded_count; i++)
		free(g_loaded_mods[i]);
	g_loaded_count = 0;
	for (int i = 0; i < g_device_count; i++)
		free(g_device_mods[i]);
	g_device_count = 0;
	for (int i = 0; i < g_c_shim_count; i++)
		free(g_c_shims[i]);
	g_c_shim_count = 0;
	g_resolve_errors = 0;

	char *source_dir = source_dir_of(source_path);
	/* Resolve the active backend selection (manifest -> env -> CLI) before loading any module, so
	 * variant overlays merge the right subfolder. Rebuilt each compilation (the doctest runner
	 * compiles many in one process). */
	variant_map_free(&g_compile_variants);
	variant_map_load_resolved(&g_compile_variants, source_dir);
	for (int u = 0; u < syntax_root->child_count; u++) {
		if (syntax_root->children[u].tag != SE_NODE || syntax_root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		load_uses_of(syntax_root->children[u].as.node, src, source_dir);
	}
	free(source_dir);
}

/* Front-end products handed from compile_frontend() to its callers. On success the caller owns
 * `source` (free it) and `sem_ctx` (semantic_context_free); `syntax_root` is kept alive for lowering
 * and follows the same ownership it always had (see compile_source's cleanup — it is not freed). */
typedef struct {
	char *source;
	SyntaxNode *syntax_root;
	SemanticContext *sem_ctx;
	int core_lines;
} Frontend;

/* Prepend the core prelude, parse, resolve `#import` modules, and semantically analyze. On success
 * fills *out and returns 0; on any error prints diagnostics to stderr, releases its own partial
 * allocations, and returns 1. Shared by `build` (compile_source) and `check` (compile_check) so
 * both run a byte-identical front-end. */
static int compile_frontend(const char *user_source, const char *source_path, Frontend *out) {
	/* Load and prepend core library. Count its newlines so we can subtract them
	 * from any combined-source line numbers we surface — the user wrote line N of
	 * their file, not line N + core_lines of the combined stream. */
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", arche_resource_dir(ARCHE_RES_CORE));
	char *core_src = read_file_optional(core_path);

	/* Don't prepend core to core itself (double-defines every prelude symbol). When the
	 * compiled file IS core.arche, drop the prepend by treating core as empty here.
	 * Identity by (device, inode) so any path spelling of the prelude matches. */
	if (core_src && source_path && source_path[0]) {
		struct stat sp, sc;
		if (stat(source_path, &sp) == 0 && stat(core_path, &sc) == 0 && sp.st_dev == sc.st_dev &&
		    sp.st_ino == sc.st_ino) {
			free(core_src);
			core_src = NULL;
		}
	}

	int core_lines = 0;
	char *source;
	if (core_src && strlen(core_src) > 0) {
		for (const char *p = core_src; *p; p++)
			if (*p == '\n')
				core_lines++;
		/* Combine: core + user source */
		size_t cl = strlen(core_src), ul = strlen(user_source);
		source = malloc(cl + ul + 1);
		memcpy(source, core_src, cl);
		memcpy(source + cl, user_source, ul + 1);
		free(core_src);
	} else {
		if (core_src)
			free(core_src); /* core.arche was empty */
		source = malloc(strlen(user_source) + 1);
		strcpy(source, user_source);
	}

	/* Diagnostics from semantic analysis carry combined-source line numbers; tell
	 * the diagnostic printer to subtract the core prelude before printing. */
	semantic_set_print_line_offset(core_lines);

	/* Lexical analysis and parsing */
	ParseResult parse_result = parse_source(source);

	if (parse_result.error_count > 0) {
		for (size_t i = 0; i < parse_result.error_count; i++) {
			int line = parse_result.errors[i].line - core_lines;
			if (line < 1)
				line = 1; /* error inside core; clamp so we don't show negative lines */
			fprintf(stderr, "[Line %d, Col %d] Error: %s\n", line, parse_result.errors[i].column,
			        parse_result.errors[i].message);
		}
		parse_result_free(&parse_result);
		free(source);
		return 1;
	}

	/* The parser produces ONLY the lossless syntax tree; semantic analysis collects its resolved
	 * DeclSummary table directly from it (no abstract AST is built anywhere). Keep the syntax tree
	 * alive through lowering (syntax-tree-driven lowering reads it); the rest of the parse result
	 * is freed now. */
	SyntaxNode *syntax_root = parse_result.syntax_root;
	parse_result.syntax_root = NULL;
	parse_result_free(&parse_result);

	/* Empty-program check is now syntax-tree-based: the program is empty unless the syntax tree
	 * root carries at least one (non-error) declaration node. */
	if (!syntax_root_has_decl(syntax_root)) {
		fprintf(stderr, "Error: Empty program\n");
		free(source);
		return 1;
	}

	/* Resolve use declarations (module loading): register each module's syntax tree with the syntax tree
	 * analyzer + lowerer, which inline + name-prefix it. Tuple-group flattening for archetype
	 * fields happens inside those syntax tree passes too, so no AstProgram pre-pass is needed. */
	resolve_uses(syntax_root, source, source_path);
	if (g_resolve_errors > 0) { /* rule 1: a bare name imported a non-device */
		fflush(stderr);
		free(source);
		return 1;
	}

	/* Semantic analysis: reconstruct the abstract AST from the lossless syntax tree (+ registered
	 * module syntax trees) and analyze that (rustc/Go-style: syntax tree → AST → check). The parser-built
	 * AstProgram is not consulted. The side model (keyed by syntax tree node id) feeds lowering. */
	SemanticContext *sem_ctx = semantic_analyze_cst(syntax_root, source);

	if (!sem_ctx || semantic_has_errors(sem_ctx)) {
		fprintf(stderr, "Semantic analysis failed\n");
		fflush(stderr);
		if (sem_ctx)
			semantic_context_free(sem_ctx);
		free(source);
		return 1;
	}

	out->source = source;
	out->syntax_root = syntax_root;
	out->sem_ctx = sem_ctx;
	out->core_lines = core_lines;
	return 0;
}

int compile_check(const char *user_source, const char *source_path, const CompileOpts *opts) {
	(void)opts; /* lint config is applied globally via semantic_set_lint_* before this call */
	Frontend fe;
	if (compile_frontend(user_source, source_path, &fe) != 0)
		return 1;
	semantic_context_free(fe.sem_ctx);
	free(fe.source);
	return 0;
}

int compile_source(const char *user_source, const char *source_path, const char *out_path, const CompileOpts *opts) {
	EmitKind emit = opts ? opts->emit : EMIT_LINK;
	int quiet = opts ? opts->quiet : 0;

	/* `--emit=shared`: emit a loadable `.so`. Force whole-program (one clean export surface) and mark
	 * codegen shared so arche defs get external (dlsym-able) linkage. Set explicitly each call (globals;
	 * the in-process doctest runner reuses this entry) so shared-ness never leaks into a later build. */
	codegen_set_shared(emit == EMIT_SHARED ? 1 : 0);
	if (emit == EMIT_SHARED)
		codegen_force_whole_program();
	/* Dev hot-reload is driven internally (no user flag): the run path sets ARCHE_HOT_DIR. Enabling it
	 * implies per-unit (each device → its own reloadable `.so`). `arche build` never sets it → release
	 * stays direct-call. (Internal env, set by `arche run`; also lets tests exercise the path.) */
	if (getenv("ARCHE_HOT_DIR")) {
		codegen_set_hot(1);
		codegen_set_per_unit(1);
	} else {
		codegen_set_hot(0);
	}
	/* `--gpu`: a `run map @gpu` dispatches the embedded compute shader at runtime (CPU fallback). Force
	 * whole-program so the dispatch calls + the registry/dispatcher objects land in one link step (the
	 * standard executable path below); set explicitly each call so the mode never leaks into a later build. */
	if (opts && opts->gpu && emit == EMIT_LINK) {
		codegen_set_gpu(1);
		codegen_force_whole_program();
	} else {
		codegen_set_gpu(0);
	}

	Frontend fe;
	if (compile_frontend(user_source, source_path, &fe) != 0)
		return 1;
	char *source = fe.source;
	SyntaxNode *syntax_root = fe.syntax_root;
	SemanticContext *sem_ctx = fe.sem_ctx;

	/* Collect `#link` system libraries (driver + imported devices' SELECTED variants) once, now that
	 * every module is registered. Appended as `-l<name>` at the link step(s) below. A bad name (outside
	 * [A-Za-z0-9._-]) returns -1 — fail the build rather than emit it into the cc command. The collected
	 * `.c` device shims (g_c_shims) ride the same link sites. */
	char link_libs[ARCHE_MAX_LINK_LIBS][64];
	int link_lib_count = semantic_collect_link_libs(syntax_root, source, link_libs, ARCHE_MAX_LINK_LIBS);
	if (link_lib_count < 0) {
		semantic_context_free(sem_ctx);
		free(source);
		return 1;
	}

	/* Lower the lossless syntax tree → AST (the only lowering path). Resolved types come from the
	 * semantic side model (keyed by syntax tree node id, globally unique across inlined modules);
	 * `use` modules are inlined from their registered syntax trees (see resolve_uses / lower_add_module). */
	lower_set_model(sem_context_model(sem_ctx));
	lower_set_sem(sem_ctx);
	HirProgram *ast = lower_to_hir(syntax_root, source);

	/* `--emit-gpu=<dir>`: also lower every `@gpu` map to a GLSL compute shader (the GPU form of the
	 * kernel) alongside the normal CPU build. A side artifact — it never changes the executable. */
	if (opts && opts->emit_gpu_dir) {
		int gpu_maps = 0;
		int n = arche_gpu_emit(ast, opts->emit_gpu_dir, &gpu_maps);
		if (!opts->quiet && n >= 0)
			printf("Emitted %d GPU compute shader(s) from %d @gpu map(s) to %s\n", n, gpu_maps, opts->emit_gpu_dir);
	}

	/* Code generation */
	CodegenContext *codegen_ctx = codegen_create(ast, sem_ctx);

	/* All exits past this point flow through `cleanup:` so the toolchain temp
	 * files, the work dir, and the IR/AST/syntax tree allocations are released exactly
	 * once. rc stays 1 until a path proves success. */
	int rc = 1;

	/* Builds past codegen put every intermediate in a private mkdtemp work dir
	 * (mode 0700, unpredictable name) — so concurrent compiles never collide on
	 * a shared /tmp/arche_<pid> path and there is no predictable-name symlink
	 * race. `--emit=llvm-ir` writes straight to the caller's out_path. */
	const char *ir_file;
	char temp_ir[512] = "", opt_file[512] = "", asm_file[512] = "";
	char workdir[] = "/tmp/arche_XXXXXX";
	int have_workdir = 0;
	/* Per-unit codegen needs a work dir for the per-unit `.ll` modules even when the final deliverable
	 * is raw IR (it llvm-links them into out_path). */
	int per_unit_build = codegen_per_unit_enabled();
	if (emit == EMIT_LLVM_IR && !per_unit_build) {
		ir_file = out_path;
	} else {
		if (!mkdtemp(workdir)) {
			perror("Failed to create temp dir");
			goto cleanup;
		}
		have_workdir = 1;
		snprintf(temp_ir, sizeof(temp_ir), "%s/out.ll", workdir);
		snprintf(opt_file, sizeof(opt_file), "%s/out_opt.ll", workdir);
		snprintf(asm_file, sizeof(asm_file), "%s/out.s", workdir);
		ir_file = (emit == EMIT_LLVM_IR) ? out_path : temp_ir;
	}

	if (per_unit_build) {
		/* Per-unit codegen: emit one LLVM module per compilation unit (unit 0 = driver, unit N = the
		 * Nth imported device). Cross-unit references resolve via the declares each unit emits +
		 * external/mangled symbols. For a `--emit=link` build this is arche's INCREMENTAL
		 * separate-compilation mode: each unit is opt/llc/cc'd to its own content-hash-cached object,
		 * so an unchanged device is reused verbatim ("compile this part, leave that alone"). The
		 * tradeoff vs the default whole-program build is no cross-unit inlining. For non-link emit
		 * kinds the units are llvm-linked into one merged artifact. The ODR verifier below stays on. */
		int max_unit = 0;
		for (int i = 0; i < ast->decl_count; i++)
			if (ast->decls[i]->unit > max_unit)
				max_unit = ast->decls[i]->unit;
		/* 1) Emit one LLVM module per compilation unit. */
		int unit_err = 0;
		for (int u = 0; u <= max_unit; u++) {
			char uf[600];
			snprintf(uf, sizeof(uf), "%s/unit_%d.ll", workdir, u);
			CodegenContext *uctx = codegen_create(ast, sem_ctx);
			codegen_set_emit_unit(uctx, u);
			FILE *ufp = fopen(uf, "w");
			if (!ufp) {
				perror("Failed to open per-unit IR file");
				codegen_free(uctx);
				unit_err = 1;
				break;
			}
			codegen_generate(uctx, ufp);
			fclose(ufp);
			if (codegen_had_error(uctx))
				unit_err = 1;
			codegen_free(uctx);
			if (unit_err)
				break;
		}
		if (unit_err) {
			rc = 1;
			goto cleanup;
		}
		/* ODR tripwire — ALWAYS run in per-unit mode (not opt-in): `llvm-link` folds `linkonce_odr`
		 * duplicates, so a divergent shared body would be silently merged to one arbitrary copy. This
		 * makes that a loud, deterministic build failure instead. Cheap: string work over files on disk. */
		if (verify_odr(workdir, max_unit)) {
			fprintf(stderr, "Per-unit ODR verification failed (shared definitions diverge across units)\n");
			rc = 1;
			goto cleanup;
		}
		if (emit == EMIT_LINK && codegen_hot_enabled()) {
			/* Dev hot-reload: device units (1..max) → individual `.so`s under $ARCHE_HOT_DIR (so a rebuild
			 * of one device lands at the path the running host watches); the driver unit (0) → the host
			 * exe, linked `-rdynamic` with the runtime + device C shims + `#link` libs + the reload runtime,
			 * so the thin device `.so`s resolve every external from it at load. The driver's cross-unit
			 * calls are trampolines (codegen `ctx->hot`); a release `arche build` never takes this path. */
			const char *rt = arche_resource_dir(ARCHE_RES_RUNTIME);
			char hotdir[1024];
			const char *henv = getenv("ARCHE_HOT_DIR");
			if (henv && *henv) {
				snprintf(hotdir, sizeof(hotdir), "%s", henv);
			} else {
				char base[800];
				snprintf(base, sizeof(base), "%s", out_path);
				char *slash = strrchr(base, '/');
				if (slash) {
					*slash = '\0';
					snprintf(hotdir, sizeof(hotdir), "%s/.arche-hot", base);
				} else {
					snprintf(hotdir, sizeof(hotdir), ".arche-hot");
				}
			}
			char mk[1100];
			snprintf(mk, sizeof(mk), "mkdir -p %s", hotdir);
			if (system(mk) != 0) {
				fprintf(stderr, "Failed to create hot-reload dir %s\n", hotdir);
				rc = 1;
				goto cleanup;
			}
			int hot_rebuilt = 0;
			for (int u = 1; u <= max_unit; u++) {
				char unit_ll[700], so[1300];
				int one = 0;
				snprintf(unit_ll, sizeof(unit_ll), "%s/unit_%d.ll", workdir, u);
				snprintf(so, sizeof(so), "%s/unit_%d.so", hotdir, u);
				if (build_unit_so(unit_ll, workdir, u, so, &one)) {
					fprintf(stderr, "Failed to build hot device .so for unit %d\n", u);
					rc = 1;
					goto cleanup;
				}
				hot_rebuilt += one;
			}
			if (!quiet)
				printf("Hot-reload: %d device .so(s), %d rebuilt\n", max_unit, hot_rebuilt);
			/* Host exe = driver unit 0, -rdynamic so device .so's resolve runtime/shim/pool/hot symbols.
			 * Cache the host object in the same per-device object cache the non-hot incremental path uses
			 * (ARCHE_CACHE_DIR, else <out_dir>/.arche-cache) so an unchanged driver relinks from cache. */
			char cache0[1024];
			const char *cenv0 = getenv("ARCHE_CACHE_DIR");
			if (cenv0 && *cenv0) {
				snprintf(cache0, sizeof(cache0), "%s", cenv0);
			} else {
				char base[800];
				snprintf(base, sizeof(base), "%s", out_path);
				char *slash0 = strrchr(base, '/');
				if (slash0) {
					*slash0 = '\0';
					snprintf(cache0, sizeof(cache0), "%s/.arche-cache", base);
				} else {
					snprintf(cache0, sizeof(cache0), ".arche-cache");
				}
			}
			char mkc0[1100];
			snprintf(mkc0, sizeof(mkc0), "mkdir -p %s", cache0);
			if (system(mkc0) != 0) {
				fprintf(stderr, "Failed to create object cache dir %s\n", cache0);
				rc = 1;
				goto cleanup;
			}
			char u0_ll[700], u0_obj[1200];
			int cached = 0;
			snprintf(u0_ll, sizeof(u0_ll), "%s/unit_0.ll", workdir);
			if (build_unit_object_cached(u0_ll, workdir, 0, cache0, u0_obj, sizeof(u0_obj), &cached)) {
				fprintf(stderr, "Failed to build host object\n");
				rc = 1;
				goto cleanup;
			}
			char cc_cmd[1 << 16];
			int cl = snprintf(cc_cmd, sizeof(cc_cmd),
			                  "cc -rdynamic -no-pie -mcmodel=large -o %s %s %s/stack_check.o %s/io.o %s/net.o "
			                  "%s/term.o %s/hotreload.o -ldl -lc",
			                  out_path, u0_obj, rt, rt, rt, rt, rt);
			if (cl < 0 || cl >= (int)sizeof(cc_cmd)) {
				fprintf(stderr, "link command too long\n");
				rc = 1;
				goto cleanup;
			}
			int link_count = opts ? opts->link_count : 0;
			for (int li = 0; li < link_count; li++) {
				int m = snprintf(cc_cmd + cl, sizeof(cc_cmd) - (size_t)cl, " %s", opts->link_paths[li]);
				if (m < 0 || m >= (int)sizeof(cc_cmd) - cl) {
					fprintf(stderr, "link command too long; refusing to drop --link inputs\n");
					rc = 1;
					goto cleanup;
				}
				cl += m;
			}
			if (append_link_extras(cc_cmd, &cl, sizeof(cc_cmd), link_libs, link_lib_count) < 0) {
				fprintf(stderr, "link command too long; refusing to drop device shims / #link libs\n");
				rc = 1;
				goto cleanup;
			}
			if (system(cc_cmd) != 0) {
				fprintf(stderr, "Failed to link hot-reload host\n");
				rc = 1;
				goto cleanup;
			}
			rc = 0;
			goto cleanup;
		}
		if (emit == EMIT_LINK) {
			/* Incremental separate compilation: opt/llc/cc EACH unit to its own cached object, then link
			 * the objects. An unchanged device's object is reused verbatim — "compile this part, leave
			 * that alone". (linkonce_odr shared defs are folded by the linker's COMDAT handling; the ODR
			 * verifier above already confirmed they're byte-identical across units.) */
			char cache_dir[1024];
			const char *cenv = getenv("ARCHE_CACHE_DIR");
			if (cenv && *cenv) {
				snprintf(cache_dir, sizeof(cache_dir), "%s", cenv);
			} else {
				char base[800];
				snprintf(base, sizeof(base), "%s", out_path);
				char *slash = strrchr(base, '/');
				if (slash) {
					*slash = '\0';
					snprintf(cache_dir, sizeof(cache_dir), "%s/.arche-cache", base);
				} else {
					snprintf(cache_dir, sizeof(cache_dir), ".arche-cache");
				}
			}
			char mkcmd[1100];
			snprintf(mkcmd, sizeof(mkcmd), "mkdir -p %s", cache_dir);
			if (system(mkcmd) != 0) {
				fprintf(stderr, "Failed to create object cache dir %s\n", cache_dir);
				rc = 1;
				goto cleanup;
			}
			const char *rt = arche_resource_dir(ARCHE_RES_RUNTIME);
			char cc_cmd[1 << 16];
			const char *out_obj = out_path;
			int cl = snprintf(cc_cmd, sizeof(cc_cmd), "cc -Wl,--gc-sections -no-pie -mcmodel=large -o %s", out_obj);
			int reused = 0;
			for (int u = 0; u <= max_unit; u++) {
				char unit_ll[700], obj[1200];
				snprintf(unit_ll, sizeof(unit_ll), "%s/unit_%d.ll", workdir, u);
				int cached = 0;
				if (build_unit_object_cached(unit_ll, workdir, u, cache_dir, obj, sizeof(obj), &cached)) {
					fprintf(stderr, "Failed to build object for unit %d\n", u);
					rc = 1;
					goto cleanup;
				}
				reused += cached;
				int m = snprintf(cc_cmd + cl, sizeof(cc_cmd) - (size_t)cl, " %s", obj);
				if (m < 0 || m >= (int)sizeof(cc_cmd) - cl) {
					fprintf(stderr, "link command too long\n");
					rc = 1;
					goto cleanup;
				}
				cl += m;
			}
			int m = snprintf(cc_cmd + cl, sizeof(cc_cmd) - (size_t)cl,
			                 " %s/stack_check.o %s/io.o %s/net.o %s/term.o -lc", rt, rt, rt, rt);
			if (m < 0 || m >= (int)sizeof(cc_cmd) - cl) {
				fprintf(stderr, "link command too long\n");
				rc = 1;
				goto cleanup;
			}
			cl += m;
			int link_count = opts ? opts->link_count : 0;
			for (int li = 0; li < link_count; li++) {
				m = snprintf(cc_cmd + cl, sizeof(cc_cmd) - (size_t)cl, " %s", opts->link_paths[li]);
				if (m < 0 || m >= (int)sizeof(cc_cmd) - cl) {
					fprintf(stderr, "link command too long; refusing to drop --link inputs\n");
					rc = 1;
					goto cleanup;
				}
				cl += m;
			}
			if (append_link_extras(cc_cmd, &cl, sizeof(cc_cmd), link_libs, link_lib_count) < 0) {
				fprintf(stderr, "link command too long; refusing to drop device shims / #link libs\n");
				rc = 1;
				goto cleanup;
			}
			if (!quiet)
				printf("Incremental link: %d unit object(s), %d reused from cache\n", max_unit + 1, reused);
			if (system(cc_cmd) != 0) {
				fprintf(stderr, "Failed to link executable\n");
				rc = 1;
				goto cleanup;
			}
			if (!quiet)
				printf("Successfully generated executable: %s\n", out_path);
			rc = 0;
			goto cleanup;
		}
		/* Non-LINK emit (llvm-ir/asm/obj) under per-unit: llvm-link the per-unit modules into the
		 * combined IR the shared opt/llc/cc pipeline below consumes (a single merged artifact is wanted). */
		char link_cmd[16384];
		int lp = snprintf(link_cmd, sizeof(link_cmd), "llvm-link -S -o %s", ir_file);
		for (int u = 0; u <= max_unit; u++)
			lp += snprintf(link_cmd + lp, sizeof(link_cmd) - (size_t)lp, " %s/unit_%d.ll", workdir, u);
		if (system(link_cmd) != 0) {
			fprintf(stderr, "Failed to llvm-link per-unit modules\n");
			goto cleanup;
		}
	} else {
		FILE *ir_output = fopen(ir_file, "w");
		if (!ir_output) {
			perror("Failed to open IR output file");
			goto cleanup;
		}
		codegen_generate(codegen_ctx, ir_output);
		fclose(ir_output);
		if (codegen_had_error(codegen_ctx)) {
			/* Codegen already printed the diagnostic; fail the build, don't run the emitted IR. */
			rc = 1;
			goto cleanup;
		}
	}
	if (!quiet)
		printf("Generated LLVM IR: %s\n", ir_file);

	if (emit == EMIT_LLVM_IR) {
		rc = 0;
		goto cleanup;
	}

	/* Run `opt -O2` first to promote allocas to SSA (mem2reg), do basic CSE,
	 * loop-invariant code motion, etc. `-mcpu=x86-64-v3` matches llc so the loop
	 * vectorizer uses the AVX2 width instead of the generic 2-wide default. */
	{
		char opt_cmd[1024];
		int m = snprintf(opt_cmd, sizeof(opt_cmd), "opt -O2 -mcpu=x86-64-v3 -S -o %s %s", opt_file, ir_file);
		if (m < 0 || m >= (int)sizeof(opt_cmd)) {
			fprintf(stderr, "opt command too long\n");
			goto cleanup;
		}
		if (!quiet)
			printf("Optimizing IR...\n");
		if (system(opt_cmd) != 0) {
			fprintf(stderr, "Failed to optimize LLVM IR\n");
			goto cleanup;
		}
	}

	/* llc → assembly. -mcpu=x86-64-v3 = portable AVX2 baseline (Haswell+): lets
	 * `<4 x double>` lower to ymm-wide vmulpd/vfmadd without pinning to this CPU.
	 * For `--emit=asm` the assembly IS the deliverable, so llc writes it straight to out_path. */
	const char *asm_target = (emit == EMIT_ASM) ? out_path : asm_file;
	{
		char llc_cmd[1024];
		/* Per-unit codegen gives arche funcs external linkage (cross-object refs), so the optimizer no
		 * longer DCEs unreferenced ones. `-function-sections`/`-data-sections` + the linker's
		 * `--gc-sections` (below) restore dead-symbol stripping at link time. */
		const char *sections = codegen_per_unit_enabled() ? "-function-sections -data-sections " : "";
		/* A loadable `.so` needs position-independent code; `-code-model=small` is the standard, tested
		 * PIC model (the exe path keeps `-no-pie -mcmodel=large`). */
		const char *reloc = (emit == EMIT_SHARED) ? "-relocation-model=pic " : "";
		const char *cmodel = (emit == EMIT_SHARED) ? "small" : "large";
		int m = snprintf(llc_cmd, sizeof(llc_cmd), "llc %s%s-code-model=%s -mcpu=x86-64-v3 -o %s %s", reloc, sections,
		                 cmodel, asm_target, opt_file);
		if (m < 0 || m >= (int)sizeof(llc_cmd)) {
			fprintf(stderr, "llc command too long\n");
			goto cleanup;
		}
		if (!quiet)
			printf("Compiling to assembly...\n");
		if (system(llc_cmd) != 0) {
			fprintf(stderr, "Failed to compile LLVM IR to assembly\n");
			goto cleanup;
		}
	}

	if (emit == EMIT_ASM) {
		if (!quiet)
			printf("Generated assembly: %s\n", out_path);
		rc = 0;
		goto cleanup;
	}

	/* `--emit=obj`: assemble to an object file (`cc -c`), no link. */
	if (emit == EMIT_OBJ) {
		char obj_cmd[1024];
		int m = snprintf(obj_cmd, sizeof(obj_cmd), "cc -no-pie -mcmodel=large -c -o %s %s", out_path, asm_file);
		if (m < 0 || m >= (int)sizeof(obj_cmd)) {
			fprintf(stderr, "assemble command too long\n");
			goto cleanup;
		}
		if (!quiet)
			printf("Assembling object: %s\n", out_path);
		if (system(obj_cmd) != 0) {
			fprintf(stderr, "Failed to assemble object file\n");
			goto cleanup;
		}
		rc = 0;
		goto cleanup;
	}

	/* `--emit=shared`: link a position-independent shared library (`cc -shared -fPIC`) with the PIC
	 * runtime objects. Self-contained (.pic.o + -lc) so it loads standalone; device shims + `#link` libs
	 * still apply. */
	if (emit == EMIT_SHARED) {
		const char *rt = arche_resource_dir(ARCHE_RES_RUNTIME);
		char cc_cmd[8192];
		int cc_len =
		    snprintf(cc_cmd, sizeof(cc_cmd),
		             "cc -shared -fPIC -o %s %s %s/stack_check.pic.o %s/io.pic.o %s/net.pic.o %s/term.pic.o -lc",
		             out_path, asm_file, rt, rt, rt, rt);
		if (cc_len < 0 || cc_len >= (int)sizeof(cc_cmd)) {
			fprintf(stderr, "link command too long\n");
			goto cleanup;
		}
		int link_count = opts ? opts->link_count : 0;
		for (int li = 0; li < link_count; li++) {
			int m = snprintf(cc_cmd + cc_len, sizeof(cc_cmd) - (size_t)cc_len, " %s", opts->link_paths[li]);
			if (m < 0 || m >= (int)sizeof(cc_cmd) - cc_len) {
				fprintf(stderr, "link command too long; refusing to drop --link inputs\n");
				goto cleanup;
			}
			cc_len += m;
		}
		if (append_link_extras(cc_cmd, &cc_len, sizeof(cc_cmd), link_libs, link_lib_count) < 0) {
			fprintf(stderr, "link command too long; refusing to drop device shims / #link libs\n");
			goto cleanup;
		}
		if (!quiet)
			printf("Linking shared library...\n");
		if (system(cc_cmd) != 0) {
			fprintf(stderr, "Failed to link shared library\n");
			goto cleanup;
		}
		if (!quiet)
			printf("Successfully generated shared library: %s\n", out_path);
		rc = 0;
		goto cleanup;
	}

	/* cc: assemble + link the runtime objects (and any --link inputs). */
	{
		const char *rt = arche_resource_dir(ARCHE_RES_RUNTIME);

		/* `--gpu`: compile each `@gpu` map's GLSL to SPIR-V (via glslc) and emit a registry object that
		 * holds the bytes; it links beside runtime/gpu_runtime.o so the in-binary dispatcher can find a
		 * shader by map name. No glslc → an empty registry (every dispatch falls back to CPU). */
		char gpu_reg_obj[600] = "";
		if (opts && opts->gpu && have_workdir) {
			char reg_c[600];
			snprintf(reg_c, sizeof(reg_c), "%s/arche_gpu_reg.c", workdir);
			if (arche_gpu_embed(ast, reg_c, quiet) < 0) {
				fprintf(stderr, "Failed to embed GPU shaders\n");
				goto cleanup;
			}
			snprintf(gpu_reg_obj, sizeof(gpu_reg_obj), "%s/arche_gpu_reg.o", workdir);
			char rc_cmd[1400];
			snprintf(rc_cmd, sizeof(rc_cmd), "cc -std=c99 -O2 -c -o %s %s", gpu_reg_obj, reg_c);
			if (system(rc_cmd) != 0) {
				fprintf(stderr, "Failed to compile GPU shader registry\n");
				goto cleanup;
			}
		}

		char cc_cmd[8192];
		const char *gc = codegen_per_unit_enabled() ? "-Wl,--gc-sections " : "";
		int cc_len = snprintf(cc_cmd, sizeof(cc_cmd),
		                      "cc %s-no-pie -mcmodel=large -o %s %s %s/stack_check.o %s/io.o %s/net.o %s/term.o -lc",
		                      gc, out_path, asm_file, rt, rt, rt, rt);
		if (cc_len < 0 || cc_len >= (int)sizeof(cc_cmd)) {
			fprintf(stderr, "link command too long\n");
			goto cleanup;
		}
		int link_count = opts ? opts->link_count : 0;
		for (int li = 0; li < link_count; li++) {
			int m = snprintf(cc_cmd + cc_len, sizeof(cc_cmd) - (size_t)cc_len, " %s", opts->link_paths[li]);
			/* Refuse to silently drop a linker input on overflow — that would
			 * miscompile (missing symbols) rather than fail loudly. */
			if (m < 0 || m >= (int)sizeof(cc_cmd) - cc_len) {
				fprintf(stderr, "link command too long; refusing to drop --link inputs\n");
				goto cleanup;
			}
			cc_len += m;
		}
		if (append_link_extras(cc_cmd, &cc_len, sizeof(cc_cmd), link_libs, link_lib_count) < 0) {
			fprintf(stderr, "link command too long; refusing to drop device shims / #link libs\n");
			goto cleanup;
		}
		/* `--gpu`: link the generated shader registry + the Vulkan dispatcher. `-lvulkan` only when the
		 * compiler itself was built with the Vulkan headers (ARCHE_HAVE_VULKAN); otherwise gpu_runtime.o
		 * is the no-op stub that needs no library and every dispatch falls back to CPU. */
		if (gpu_reg_obj[0]) {
#ifdef ARCHE_HAVE_VULKAN
			const char *gpu_libs = " -lvulkan";
#else
			const char *gpu_libs = "";
#endif
			int m = snprintf(cc_cmd + cc_len, sizeof(cc_cmd) - (size_t)cc_len, " %s %s/gpu_runtime.o%s", gpu_reg_obj,
			                 rt, gpu_libs);
			if (m < 0 || m >= (int)sizeof(cc_cmd) - cc_len) {
				fprintf(stderr, "link command too long; refusing to drop GPU objects\n");
				goto cleanup;
			}
			cc_len += m;
		}
		if (!quiet)
			printf("Linking executable...\n");
		if (system(cc_cmd) != 0) {
			fprintf(stderr, "Failed to link executable\n");
			goto cleanup;
		}
	}

	if (!quiet)
		printf("Successfully generated executable: %s\n", out_path);
	rc = 0;

cleanup:
	if (have_workdir) {
		if (temp_ir[0])
			unlink(temp_ir);
		if (opt_file[0])
			unlink(opt_file);
		if (asm_file[0])
			unlink(asm_file);
		rmdir(workdir);
	}
	/* AST must be freed before syntax tree (HIR_TYPE_NAMED ptrs reference into the syntax tree). */
	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	hir_program_free(ast);
	free(source);
	return rc;
}
