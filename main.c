#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif

#ifndef ARCHE_RUNTIME_DIR
#define ARCHE_RUNTIME_DIR "build/runtime"
#endif

static char *read_file_optional(const char *path);

static int file_exists(const char *path) {
	struct stat sb;
	return stat(path, &sb) == 0;
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

/* =========================
   Module auto-prefix
   =========================
   When `use foo;` is resolved, every top-level name declared by the foo module
   is rewritten to `foo_<name>`, and every internal reference to those names is
   rewritten the same way.  External references (e.g. into core, or to extern
   C functions) are left alone because they don't appear in the local-name set. */

static int name_in_set(char **set, int count, const char *name) {
	if (!name)
		return 0;
	for (int i = 0; i < count; i++)
		if (strcmp(set[i], name) == 0)
			return 1;
	return 0;
}

static char *prefix_name(const char *prefix, const char *name) {
	int p = (int)strlen(prefix), n = (int)strlen(name);
	char *r = malloc(p + 1 + n + 1);
	memcpy(r, prefix, p);
	r[p] = '_';
	memcpy(r + p + 1, name, n);
	r[p + 1 + n] = 0;
	return r;
}

static void maybe_rename(char **slot, const char *prefix, char **set, int count) {
	if (!*slot || !name_in_set(set, count, *slot))
		return;
	char *old = *slot;
	*slot = prefix_name(prefix, old);
	free(old);
}

static void rename_typeref(TypeRef *t, const char *prefix, char **set, int count);
static void rename_expr(Expression *e, const char *prefix, char **set, int count);
static void rename_stmt(Statement *s, const char *prefix, char **set, int count);

static void rename_typeref(TypeRef *t, const char *prefix, char **set, int count) {
	if (!t)
		return;
	switch (t->kind) {
	case TYPE_NAME:
		maybe_rename(&t->data.name, prefix, set, count);
		break;
	case TYPE_ARRAY:
		rename_typeref(t->data.array.element_type, prefix, set, count);
		break;
	case TYPE_SHAPED_ARRAY:
		rename_typeref(t->data.shaped_array.element_type, prefix, set, count);
		break;
	case TYPE_TUPLE:
		for (int i = 0; i < t->data.tuple.field_count; i++)
			rename_typeref(t->data.tuple.field_types[i], prefix, set, count);
		break;
	case TYPE_HANDLE:
		maybe_rename(&t->data.handle.archetype_name, prefix, set, count);
		break;
	case TYPE_ARCHETYPE:
		break;
	case TYPE_OPAQUE:
		break;
	}
}

static void rename_expr(Expression *e, const char *prefix, char **set, int count) {
	if (!e)
		return;
	switch (e->type) {
	case EXPR_LITERAL:
	case EXPR_STRING:
		break;
	case EXPR_NAME:
		maybe_rename(&e->data.name.name, prefix, set, count);
		break;
	case EXPR_FIELD:
		rename_expr(e->data.field.base, prefix, set, count);
		break;
	case EXPR_INDEX:
		rename_expr(e->data.index.base, prefix, set, count);
		for (int i = 0; i < e->data.index.index_count; i++)
			rename_expr(e->data.index.indices[i], prefix, set, count);
		break;
	case EXPR_BINARY:
		rename_expr(e->data.binary.left, prefix, set, count);
		rename_expr(e->data.binary.right, prefix, set, count);
		break;
	case EXPR_UNARY:
		rename_expr(e->data.unary.operand, prefix, set, count);
		break;
	case EXPR_CALL:
		rename_expr(e->data.call.callee, prefix, set, count);
		for (int i = 0; i < e->data.call.arg_count; i++)
			rename_expr(e->data.call.args[i], prefix, set, count);
		break;
	case EXPR_ALLOC:
		maybe_rename(&e->data.alloc.archetype_name, prefix, set, count);
		for (int i = 0; i < e->data.alloc.field_count; i++)
			rename_expr(e->data.alloc.field_values[i], prefix, set, count);
		rename_expr(e->data.alloc.init_length, prefix, set, count);
		break;
	case EXPR_ARRAY_LITERAL:
		for (int i = 0; i < e->data.array_literal.element_count; i++)
			rename_expr(e->data.array_literal.elements[i], prefix, set, count);
		break;
	}
}

static void rename_stmt(Statement *s, const char *prefix, char **set, int count) {
	if (!s)
		return;
	switch (s->type) {
	case STMT_BIND:
		rename_typeref(s->data.bind_stmt.type, prefix, set, count);
		rename_expr(s->data.bind_stmt.value, prefix, set, count);
		break;
	case STMT_ASSIGN:
		rename_expr(s->data.assign_stmt.target, prefix, set, count);
		rename_expr(s->data.assign_stmt.value, prefix, set, count);
		break;
	case STMT_FOR:
		rename_expr(s->data.for_stmt.iterable, prefix, set, count);
		rename_stmt(s->data.for_stmt.init, prefix, set, count);
		rename_expr(s->data.for_stmt.condition, prefix, set, count);
		rename_stmt(s->data.for_stmt.increment, prefix, set, count);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			rename_stmt(s->data.for_stmt.body[i], prefix, set, count);
		break;
	case STMT_IF:
		rename_expr(s->data.if_stmt.cond, prefix, set, count);
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			rename_stmt(s->data.if_stmt.then_body[i], prefix, set, count);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			rename_stmt(s->data.if_stmt.else_body[i], prefix, set, count);
		break;
	case STMT_BREAK:
		break;
	case STMT_RUN:
		break; /* world/system names — module-local sys not currently supported via use */
	case STMT_EXPR:
		rename_expr(s->data.expr_stmt.expr, prefix, set, count);
		break;
	case STMT_FREE:
		rename_expr(s->data.free_stmt.value, prefix, set, count);
		break;
	case STMT_RETURN:
		rename_expr(s->data.return_stmt.value, prefix, set, count);
		break;
	case STMT_MULTI_BIND:
		for (int i = 0; i < s->data.multi_bind.target_count; i++)
			rename_typeref(s->data.multi_bind.targets[i].type, prefix, set, count);
		rename_expr(s->data.multi_bind.value, prefix, set, count);
		break;
	case STMT_EACH_FIELD:
		rename_typeref(s->data.each_field.filter_type, prefix, set, count);
		for (int i = 0; i < s->data.each_field.body_count; i++)
			rename_stmt(s->data.each_field.body[i], prefix, set, count);
		break;
	}
}

static void collect_module_names(Program *mod, char ***out_set, int *out_count) {
	int cap = 0;
	int n = 0;
	char **set = NULL;
	for (int i = 0; i < mod->decl_count; i++) {
		Decl *d = mod->decls[i];
		const char *name = NULL;
		switch (d->kind) {
		case DECL_ARCHETYPE:
			name = d->data.archetype->name;
			break;
		case DECL_PROC:
			name = d->data.proc->name;
			break;
		case DECL_SYS:
			name = d->data.sys->name;
			break;
		case DECL_FUNC:
			name = d->data.func->name;
			break;
		case DECL_FUNC_GROUP:
			name = d->data.func_group->name;
			break;
		case DECL_STATIC:
			name = (d->data.static_decl->kind == STATIC_KIND_ARRAY) ? d->data.static_decl->array.name
			                                                        : d->data.static_decl->archetype.archetype_name;
			break;
		case DECL_CONST:
			name = d->data.constant->name;
			break;
		case DECL_WORLD:
			name = d->data.world->name;
			break;
		case DECL_USE:
			break;
		}
		if (!name)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			set = realloc(set, cap * sizeof(char *));
		}
		set[n] = malloc(strlen(name) + 1);
		strcpy(set[n], name);
		n++;
	}
	*out_set = set;
	*out_count = n;
}

static void free_name_set(char **set, int count) {
	for (int i = 0; i < count; i++)
		free(set[i]);
	free(set);
}

static void rename_decl(Decl *d, const char *prefix, char **set, int count) {
	switch (d->kind) {
	case DECL_ARCHETYPE:
		maybe_rename(&d->data.archetype->name, prefix, set, count);
		for (int i = 0; i < d->data.archetype->field_count; i++)
			rename_typeref(d->data.archetype->fields[i]->type, prefix, set, count);
		break;
	case DECL_PROC:
		maybe_rename(&d->data.proc->name, prefix, set, count);
		for (int i = 0; i < d->data.proc->param_count; i++)
			rename_typeref(d->data.proc->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.proc->statement_count; i++)
			rename_stmt(d->data.proc->statements[i], prefix, set, count);
		break;
	case DECL_SYS:
		maybe_rename(&d->data.sys->name, prefix, set, count);
		for (int i = 0; i < d->data.sys->param_count; i++)
			rename_typeref(d->data.sys->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.sys->statement_count; i++)
			rename_stmt(d->data.sys->statements[i], prefix, set, count);
		break;
	case DECL_FUNC:
		maybe_rename(&d->data.func->name, prefix, set, count);
		rename_typeref(d->data.func->return_type, prefix, set, count);
		for (int i = 0; i < d->data.func->param_count; i++)
			rename_typeref(d->data.func->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.func->statement_count; i++)
			rename_stmt(d->data.func->statements[i], prefix, set, count);
		break;
	case DECL_FUNC_GROUP:
		maybe_rename(&d->data.func_group->name, prefix, set, count);
		for (int i = 0; i < d->data.func_group->member_count; i++)
			maybe_rename(&d->data.func_group->member_names[i], prefix, set, count);
		break;
	case DECL_STATIC:
		if (d->data.static_decl->kind == STATIC_KIND_ARRAY) {
			maybe_rename(&d->data.static_decl->array.name, prefix, set, count);
			rename_typeref(d->data.static_decl->array.element_type, prefix, set, count);
		} else {
			maybe_rename(&d->data.static_decl->archetype.archetype_name, prefix, set, count);
			for (int i = 0; i < d->data.static_decl->archetype.field_count; i++)
				rename_expr(d->data.static_decl->archetype.field_values[i], prefix, set, count);
			rename_expr(d->data.static_decl->archetype.init_length, prefix, set, count);
		}
		break;
	case DECL_CONST:
		maybe_rename(&d->data.constant->name, prefix, set, count);
		rename_expr(d->data.constant->value, prefix, set, count);
		break;
	case DECL_WORLD:
		maybe_rename(&d->data.world->name, prefix, set, count);
		break;
	case DECL_USE:
		break;
	}
}

static void prefix_module(Program *mod, const char *prefix) {
	char **set = NULL;
	int count = 0;
	collect_module_names(mod, &set, &count);
	for (int i = 0; i < mod->decl_count; i++)
		rename_decl(mod->decls[i], prefix, set, count);
	free_name_set(set, count);
}

static void resolve_uses(Program *prog, const char *source_path) {
	int i = 0;

	/* Accumulate each module's prefix + original exported names so that, after all
	   modules are inlined, a bare cross-module reference can be resolved (below). */
	char **acc_prefix = NULL;
	char ***acc_set = NULL;
	int *acc_count = NULL;
	int acc_n = 0, acc_cap = 0;

	while (i < prog->decl_count) {
		if (prog->decls[i]->kind != DECL_USE) {
			i++;
			continue;
		}

		char *mod_name = prog->decls[i]->data.use->name;

		/* Try local directory first */
		char *source_dir = source_dir_of(source_path);
		char path1[512];
		snprintf(path1, sizeof(path1), "%s/%s.arche", source_dir, mod_name);
		free(source_dir);

		char path2[512];
		snprintf(path2, sizeof(path2), "%s/%s.arche", ARCHE_CORE_DIR, mod_name);

		char *found_path = NULL;
		if (file_exists(path1)) {
			found_path = malloc(strlen(path1) + 1);
			strcpy(found_path, path1);
		} else if (file_exists(path2)) {
			found_path = malloc(strlen(path2) + 1);
			strcpy(found_path, path2);
		}

		if (!found_path) {
			fprintf(stderr, "Error: Module not found: %s\n", mod_name);
			i++;
			continue;
		}

		/* Read and parse module */
		FILE *f = fopen(found_path, "r");
		if (!f) {
			fprintf(stderr, "Error: Failed to open module: %s\n", found_path);
			free(found_path);
			i++;
			continue;
		}

		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);

		char *mod_src = malloc(size + 2);
		size_t n = fread(mod_src, 1, size, f);
		fclose(f);

		if (n > 0)
			mod_src[n] = '\n';
		mod_src[n + (n > 0 ? 1 : 0)] = '\0';

		/* Parse module without prepending core (avoid duplicate declarations) */
		ParseResult mod_parse = parse_source(mod_src);
		free(mod_src);

		if (mod_parse.error_count > 0) {
			fprintf(stderr, "Error: Failed to parse module %s\n", mod_name);
			for (size_t j = 0; j < mod_parse.error_count; j++) {
				fprintf(stderr, "  [Line %d] %s\n", mod_parse.errors[j].line, mod_parse.errors[j].message);
			}
			parse_result_free(&mod_parse);
			i++;
			continue;
		}

		Program *mod = mod_parse.ast;
		parse_result_free(&mod_parse);

		if (!mod || mod->decl_count == 0) {
			if (mod)
				program_free(mod);
			i++;
			continue;
		}

		/* Record this module's prefix + original exported names (before prefixing,
		   while the names are still bare) for cross-module resolution after the loop. */
		{
			char **mset;
			int mcount;
			collect_module_names(mod, &mset, &mcount);
			if (acc_n == acc_cap) {
				acc_cap = acc_cap ? acc_cap * 2 : 8;
				acc_prefix = realloc(acc_prefix, acc_cap * sizeof(char *));
				acc_set = realloc(acc_set, acc_cap * sizeof(char **));
				acc_count = realloc(acc_count, acc_cap * sizeof(int));
			}
			acc_prefix[acc_n] = malloc(strlen(mod_name) + 1);
			strcpy(acc_prefix[acc_n], mod_name);
			acc_set[acc_n] = mset;
			acc_count[acc_n] = mcount;
			acc_n++;
		}

		/* Auto-prefix every top-level name and its internal references
		 * with the module name. After `use csv;`, csv.arche's `open` becomes
		 * `csv_open` to callers and inside the module. */
		prefix_module(mod, mod_name);

		/* Insert module declarations at current position i (before DECL_USE) */
		int new_count = prog->decl_count + mod->decl_count;
		Decl **new_decls = malloc(sizeof(Decl *) * new_count);

		/* Copy declarations before position i */
		memcpy(new_decls, prog->decls, sizeof(Decl *) * i);

		/* Copy module decls */
		memcpy(new_decls + i, mod->decls, sizeof(Decl *) * mod->decl_count);

		/* Copy declarations from position i onward (DECL_USE and rest) */
		memcpy(new_decls + i + mod->decl_count, prog->decls + i, sizeof(Decl *) * (prog->decl_count - i));

		free(prog->decls);
		prog->decls = new_decls;
		prog->decl_count = new_count;

		int mod_decl_count = mod->decl_count;
		free(mod->decls); /* free array, not contents */
		free(mod);
		free(found_path);

		/* Skip past inserted module declarations and DECL_USE node */
		i += mod_decl_count + 1;
	}

	/* Cross-module resolution.
	   A module's exported names were rewritten to `<module>_<name>`, so a *bare*
	   reference from another module (or main) to such a name is now dangling. For
	   each module export whose bare name has NO top-level definition in the final
	   program, rewrite bare references to it into the prefixed name. Exports that
	   collide with an existing top-level name (e.g. a core function like `open`)
	   are left alone, so bare `open` still means core's `open` — current scoping is
	   preserved and only previously-undefined references gain a meaning. */
	if (acc_n > 0) {
		char **top_set;
		int top_count;
		collect_module_names(prog, &top_set, &top_count); /* all current top-level names */
		for (int m = 0; m < acc_n; m++) {
			char **dangling = malloc((acc_count[m] + 1) * sizeof(char *));
			int dn = 0;
			for (int s = 0; s < acc_count[m]; s++) {
				if (!name_in_set(top_set, top_count, acc_set[m][s]))
					dangling[dn++] = acc_set[m][s];
			}
			if (dn > 0) {
				for (int k = 0; k < prog->decl_count; k++)
					rename_decl(prog->decls[k], acc_prefix[m], dangling, dn);
			}
			free(dangling);
		}
		free_name_set(top_set, top_count);
	}
	for (int m = 0; m < acc_n; m++) {
		free(acc_prefix[m]);
		free_name_set(acc_set[m], acc_count[m]);
	}
	free(acc_prefix);
	free(acc_set);
	free(acc_count);
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

static void usage(const char *prog) {
	fprintf(stderr, "Usage: %s [-o executable] [--link <path>] input.arche\n", prog);
	fprintf(stderr, "       %s [-emit-llvm -o output.ll] input.arche\n", prog);
	fprintf(stderr, "       --link <path>  Pass additional .c or .o file to cc at link time\n");
	exit(1);
}

/* Maximum number of --link paths accepted on one command line */
#define MAX_LINK_PATHS 32

int main(int argc, char *argv[]) {
	const char *input_file = NULL;
	const char *output_file = NULL;
	int emit_llvm = 0;

	/* Extra files to pass to cc at link time (--link <path>) */
	const char *link_paths[MAX_LINK_PATHS];
	int link_count = 0;

	/* Lint config — both on by default; CLI can disable or promote to errors. */
	int lint_pcbf_enabled = 1, lint_pcbf_werror = 0;
	int lint_pne_enabled = 1, lint_pne_werror = 0;

	/* Parse command-line arguments */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0) {
			if (i + 1 >= argc) {
				usage(argv[0]);
			}
			output_file = argv[++i];
		} else if (strcmp(argv[i], "--link") == 0) {
			if (i + 1 >= argc) {
				usage(argv[0]);
			}
			if (link_count < MAX_LINK_PATHS) {
				link_paths[link_count++] = argv[++i];
			} else {
				fprintf(stderr, "Error: too many --link arguments (max %d)\n", MAX_LINK_PATHS);
				return 1;
			}
		} else if (strcmp(argv[i], "-emit-llvm") == 0) {
			emit_llvm = 1;
		} else if (strcmp(argv[i], "-Wno-proc-could-be-func") == 0) {
			lint_pcbf_enabled = 0;
		} else if (strcmp(argv[i], "-Wno-proc-no-effect") == 0) {
			lint_pne_enabled = 0;
		} else if (strcmp(argv[i], "-Werror=proc-could-be-func") == 0) {
			lint_pcbf_werror = 1;
		} else if (strcmp(argv[i], "-Werror=proc-no-effect") == 0) {
			lint_pne_werror = 1;
		} else if (strcmp(argv[i], "-Werror") == 0) {
			lint_pcbf_werror = 1;
			lint_pne_werror = 1;
		} else if (argv[i][0] != '-') {
			input_file = argv[i];
		}
	}

	semantic_set_lint_proc_could_be_func(lint_pcbf_enabled, lint_pcbf_werror);
	semantic_set_lint_proc_no_effect(lint_pne_enabled, lint_pne_werror);

	if (!input_file) {
		usage(argv[0]);
	}

	/* Limit memory to 512MB to prevent runaway compilation */
	struct rlimit mem_limit;
	mem_limit.rlim_cur = 512 * 1024 * 1024;
	mem_limit.rlim_max = 512 * 1024 * 1024;
	int limit_result = setrlimit(RLIMIT_AS, &mem_limit);
	if (limit_result != 0) {
		perror("Error: Could not set memory limit");
		return 1;
	}

	if (!output_file) {
		/* Default output: build/basename without extension */
		const char *base = strrchr(input_file, '/');
		if (!base)
			base = input_file;
		else
			base++;

		int len = strlen(base) + 20;
		output_file = malloc(len);
		strcpy((char *)output_file, "build/");
		strcat((char *)output_file, base);
		char *dot = strrchr((char *)output_file, '.');
		if (dot) {
			strcpy(dot, "");
		}
	}

	/* Read input file */
	FILE *input = fopen(input_file, "r");
	if (!input) {
		perror("Failed to open input file");
		return 1;
	}

	fseek(input, 0, SEEK_END);
	long file_size = ftell(input);
	fseek(input, 0, SEEK_SET);

	char *source = malloc(file_size + 1);
	if (fread(source, 1, file_size, input) != (size_t)file_size) {
		perror("Failed to read input file");
		free(source);
		fclose(input);
		return 1;
	}
	source[file_size] = '\0';
	fclose(input);

	/* Load and prepend core library */
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", ARCHE_CORE_DIR);
	char *core_src = read_file_optional(core_path);

	char *combined_source = NULL;
	if (core_src && strlen(core_src) > 0) {
		/* Combine: core + user source */
		combined_source = malloc(strlen(core_src) + strlen(source) + 1);
		strcpy(combined_source, core_src);
		strcat(combined_source, source);
		free(core_src);
		free(source);
		source = combined_source;
	} else if (core_src) {
		free(core_src); /* core.arche was empty */
	}

	/* Lexical analysis and parsing */
	ParseResult parse_result = parse_source(source);

	if (parse_result.error_count > 0) {
		for (size_t i = 0; i < parse_result.error_count; i++) {
			fprintf(stderr, "[Line %d, Col %d] Error: %s\n", parse_result.errors[i].line, parse_result.errors[i].column,
			        parse_result.errors[i].message);
		}
		Program *prog = parse_result.ast;
		parse_result_free(&parse_result);
		program_free(prog);
		free(source);
		return 1;
	}

	Program *prog = parse_result.ast;
	parse_result_free(&parse_result);

	if (!prog || prog->decl_count == 0) {
		fprintf(stderr, "Error: Empty program\n");
		if (prog)
			program_free(prog);
		free(source);
		return 1;
	}

	/* Resolve use declarations (module loading) */
	resolve_uses(prog, input_file);

	/* Semantic analysis */
	SemanticContext *sem_ctx = semantic_analyze(prog);

	if (!sem_ctx || semantic_has_errors(sem_ctx)) {
		fprintf(stderr, "Semantic analysis failed\n");
		fflush(stderr);
		if (sem_ctx)
			semantic_context_free(sem_ctx);
		program_free(prog);
		free(source);
		return 1;
	}

	/* Lower CST → AST */
	AstProgram *ast = lower_cst_to_ast(prog);

	/* Code generation */
	CodegenContext *codegen_ctx = codegen_create(ast, sem_ctx);

	/* Generate LLVM IR to temporary file or specified output */
	const char *ir_file;
	char temp_ir[256] = "";
	if (emit_llvm) {
		ir_file = output_file;
	} else {
		snprintf(temp_ir, sizeof(temp_ir), "/tmp/arche_%d.ll", (int)getpid());
		ir_file = temp_ir;
	}

	FILE *ir_output = fopen(ir_file, "w");
	if (!ir_output) {
		perror("Failed to open IR output file");
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		ast_program_free(ast);
		program_free(prog);
		free(source);
		return 1;
	}

	codegen_generate(codegen_ctx, ir_output);
	fclose(ir_output);

	printf("Generated LLVM IR: %s\n", ir_file);

	/* If emit-llvm flag, we're done */
	if (emit_llvm) {
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		ast_program_free(ast);
		program_free(prog);
		free(source);
		return 0;
	}

	/* Compile IR to executable using opt, llc, and cc */
	char opt_file[256], asm_file[256];
	snprintf(opt_file, sizeof(opt_file), "/tmp/arche_%d_opt.ll", (int)getpid());
	snprintf(asm_file, sizeof(asm_file), "/tmp/arche_%d.s", (int)getpid());

	/* Run `opt -O2` first to promote allocas to SSA (mem2reg), do basic CSE,
	 * loop-invariant code motion, etc. The codegen emits stack-resident loop
	 * counters and accumulators because that's easier than building SSA
	 * directly; opt cleans them up before llc sees them. */
	char opt_cmd[512];
	snprintf(opt_cmd, sizeof(opt_cmd), "opt -O2 -S -o %s %s", opt_file, ir_file);
	printf("Optimizing IR...\n");
	int ret = system(opt_cmd);
	if (ret != 0) {
		fprintf(stderr, "Failed to optimize LLVM IR\n");
		unlink(ir_file);
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		ast_program_free(ast);
		program_free(prog);
		free(source);
		return 1;
	}

	/* Call llc to generate assembly. -mcpu=x86-64-v3 = portable AVX2 baseline
	 * (Haswell-era and newer): AVX, AVX2, BMI, BMI2, F16C, FMA, LZCNT, MOVBE,
	 * XSAVE. Lets `<4 x double>` IR lower to ymm-wide vmulpd/vfmadd instead of
	 * paired SSE2 mulpd, without tying the binary to the dev-machine CPU. */
	char llc_cmd[512];
	snprintf(llc_cmd, sizeof(llc_cmd), "llc -code-model=large -mcpu=x86-64-v3 -o %s %s", asm_file, opt_file);
	printf("Compiling to assembly...\n");
	ret = system(llc_cmd);
	if (ret != 0) {
		fprintf(stderr, "Failed to compile LLVM IR to assembly\n");
		/* Copy IR for debugging */
		char debug_copy[256];
		snprintf(debug_copy, sizeof(debug_copy), "cp %s tests/tmp/ir_debug.ll 2>/dev/null || cp %s /tmp/debug.ll",
		         ir_file, ir_file);
		system(debug_copy);
		unlink(ir_file);
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		ast_program_free(ast);
		program_free(prog);
		free(source);
		return 1;
	}

	/* Call cc to assemble and link with runtime objects */
	/* Base command: fixed runtime objects */
	char cc_cmd[4096];
	int cc_len =
	    snprintf(cc_cmd, sizeof(cc_cmd),
	             "cc -no-pie -mcmodel=large -o %s %s " ARCHE_RUNTIME_DIR "/stack_check.o " ARCHE_RUNTIME_DIR
	             "/io.o " ARCHE_RUNTIME_DIR "/net.o " ARCHE_RUNTIME_DIR "/term.o "
	             "-lc",
	             output_file, asm_file);

	/* Append any --link paths supplied on the command line */
	for (int li = 0; li < link_count && cc_len < (int)sizeof(cc_cmd) - 1; li++) {
		cc_len += snprintf(cc_cmd + cc_len, sizeof(cc_cmd) - (size_t)cc_len, " %s", link_paths[li]);
	}

	printf("Linking executable...\n");
	ret = system(cc_cmd);
	if (ret != 0) {
		fprintf(stderr, "Failed to link executable\n");
		unlink(ir_file);
		unlink(asm_file);
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		ast_program_free(ast);
		program_free(prog);
		free(source);
		return 1;
	}

	printf("Successfully generated executable: %s\n", output_file);

	/* Cleanup temporary files (save IR for inspection during development) */
	char save_ir[256];
	snprintf(save_ir, sizeof(save_ir), "cp %s tests/tmp/ir_last.ll 2>/dev/null", ir_file);
	system(save_ir);
	unlink(ir_file);
	unlink(asm_file);

	/* Cleanup — AST must be freed before CST (AST_TYPE_NAMED ptrs into CST) */
	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	ast_program_free(ast);
	program_free(prog);
	free(source);

	return 0;
}
