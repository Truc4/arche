#include "args.h"
#include <string.h>

/* Does `forms` (space-separated list) contain the exact spelling `tok`? */
static int forms_contain(const char *forms, const char *tok, size_t toklen) {
	const char *p = forms;
	while (*p) {
		while (*p == ' ')
			p++;
		const char *start = p;
		while (*p && *p != ' ')
			p++;
		size_t len = (size_t)(p - start);
		if (len == toklen && strncmp(start, tok, len) == 0)
			return 1;
	}
	return 0;
}

/* The canonical (first) spelling of a spec, for help/error text. Writes into buf. */
static const char *first_form(const ArgSpec *s, char *buf, size_t cap) {
	const char *p = s->forms;
	while (*p == ' ')
		p++;
	size_t n = 0;
	while (p[n] && p[n] != ' ' && n + 1 < cap)
		n++;
	memcpy(buf, p, n);
	buf[n] = '\0';
	return buf;
}

static void fail(ArgParse *out, const char *what, const char *tok) {
	snprintf(out->err, sizeof(out->err), "%s: %s", what, tok ? tok : "");
}

int args_parse(const ArgSpec *specs, int argc, char **argv, ArgParse *out) {
	memset(out, 0, sizeof(*out));

	for (int i = 1; i < argc; i++) {
		const char *tok = argv[i];

		if (strcmp(tok, "--") == 0) {
			out->saw_dashdash = 1;
			for (int j = i + 1; j < argc && out->fwd_count < ARG_MAX_POS; j++)
				out->fwd[out->fwd_count++] = argv[j];
			break;
		}

		/* `-h` / `--help` is recognized for every command — record it and let the caller render the
		 * spec-generated usage. Not a usage error even if the command declares no other flags. */
		if (strcmp(tok, "-h") == 0 || strcmp(tok, "--help") == 0) {
			out->want_help = 1;
			continue;
		}

		/* Positional: anything not starting with '-' (a lone "-" is also positional, e.g. stdin), plus a
		 * negative number (`-5`, `-.5`) — a value, not a flag (no flag spelling is `-<digit>` / `-.`). */
		if (tok[0] != '-' || tok[1] == '\0' || (tok[1] >= '0' && tok[1] <= '9') || tok[1] == '.') {
			if (out->pos_count < ARG_MAX_POS)
				out->pos[out->pos_count++] = tok;
			continue;
		}

		/* Split off an inline value: the part before the first '=' is the candidate flag spelling.
		 * Only relevant to ARG_VALUE specs; ARG_FLAG specs (e.g. `-Werror=proc-no-effect`) match the
		 * whole token exactly, so we try a full-token match before the '='-split match. */
		const char *eq = strchr(tok, '=');
		size_t toklen = strlen(tok);
		size_t headlen = eq ? (size_t)(eq - tok) : toklen;

		int matched = 0;
		for (const ArgSpec *s = specs; s->id != 0; s++) {
			/* exact whole-token match (covers all ARG_FLAG and the `<form> <value>` ARG_VALUE form) */
			if (forms_contain(s->forms, tok, toklen)) {
				if (s->kind == ARG_VALUE) {
					if (i + 1 >= argc) {
						char b[64];
						fail(out, "flag needs a value", first_form(s, b, sizeof b));
						return 2;
					}
					if (out->hit_count < ARG_MAX_HITS) {
						out->hits[out->hit_count].id = s->id;
						out->hits[out->hit_count].value = argv[++i];
						out->hit_count++;
					}
				} else {
					if (out->hit_count < ARG_MAX_HITS) {
						out->hits[out->hit_count].id = s->id;
						out->hits[out->hit_count].value = NULL;
						out->hit_count++;
					}
				}
				matched = 1;
				break;
			}
			/* inline `<form>=<value>` for ARG_VALUE specs */
			if (s->kind == ARG_VALUE && eq && forms_contain(s->forms, tok, headlen)) {
				if (out->hit_count < ARG_MAX_HITS) {
					out->hits[out->hit_count].id = s->id;
					out->hits[out->hit_count].value = eq + 1;
					out->hit_count++;
				}
				matched = 1;
				break;
			}
		}

		if (!matched) {
			fail(out, "unknown flag", tok);
			return 2;
		}
	}
	return 0;
}

int args_has(const ArgParse *p, int id) {
	for (int i = 0; i < p->hit_count; i++)
		if (p->hits[i].id == id)
			return 1;
	return 0;
}

const char *args_value(const ArgParse *p, int id) {
	const char *v = NULL;
	for (int i = 0; i < p->hit_count; i++)
		if (p->hits[i].id == id)
			v = p->hits[i].value; /* last-wins */
	return v;
}

void args_usage(FILE *f, const char *prog, const char *cmd, const char *synopsis, const ArgSpec *specs) {
	if (cmd)
		fprintf(f, "usage: %s %s %s\n", prog, cmd, synopsis ? synopsis : "");
	else
		fprintf(f, "usage: %s %s\n", prog, synopsis ? synopsis : "");
	int any = 0;
	for (const ArgSpec *s = specs; s->id != 0; s++)
		if (!s->hidden)
			any = 1;
	if (!any)
		return;
	fprintf(f, "\nflags:\n");
	for (const ArgSpec *s = specs; s->id != 0; s++) {
		if (s->hidden)
			continue;
		char left[96];
		if (s->kind == ARG_VALUE)
			snprintf(left, sizeof(left), "%s %s", s->forms, s->metavar ? s->metavar : "<value>");
		else
			snprintf(left, sizeof(left), "%s", s->forms);
		fprintf(f, "  %-32s %s\n", left, s->help ? s->help : "");
	}
}
