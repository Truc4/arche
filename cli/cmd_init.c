#include "cli.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* `arche init device <name>` / `arche init driver <name>` — scaffold a device or a driver.
 *
 * A device is a group of files defining shapes + systems (with a doctest that drives them); a
 * driver imports devices, sizes their shapes, and runs their systems. The templates are starting
 * points — edit them. We never overwrite an existing file. */

static int write_new_file(const char *path, const char *content) {
	FILE *f = fopen(path, "wx"); /* `x`: fail if it already exists — never clobber */
	if (!f) {
		fprintf(stderr, "arche init: cannot create '%s' (it may already exist)\n", path);
		return ARCHE_ERR;
	}
	fputs(content, f);
	fclose(f);
	printf("created %s\n", path);
	return ARCHE_OK;
}

int init_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	/* argv[0] is "init"; expect "<device|driver> <name>". */
	if (argc < 3) {
		fprintf(stderr, "usage: arche init <device|driver> <name>\n");
		return ARCHE_USAGE;
	}
	const char *kind = argv[1];
	const char *name = argv[2];
	char path[600];

	if (strcmp(kind, "device") == 0) {
		/* A device is a folder of files; scaffold <name>/<name>.arche. */
		mkdir(name, 0755);
		snprintf(path, sizeof(path), "%s/%s.arche", name, name);
		char content[2048];
		snprintf(content, sizeof(content),
		         "// %s — a device: it defines shapes + systems but does not size the pool.\n"
		         "// A driver sizes the shape (`%s.Particle[N]`) and runs the systems (`run %s.integrate`).\n"
		         "#import { fmt }\n"
		         "\n"
		         "Particle :: arche { pos :: float, vel :: float }\n"
		         "\n"
		         "/// A device is tested by writing a driver for it. This doctest (a tiny driver) sizes\n"
		         "/// the shape, inserts, drives the system, and checks the result.\n"
		         "/// ```arche\n"
		         "/// Particle[4]\n"
		         "/// main :: proc() {\n"
		         "///   insert(Particle, 10.0, 1.0);\n"
		         "///   run integrate;\n"
		         "///   fmt.assert(Particle.pos[0] * 10 == 110, \"integrate did not run\\n\");\n"
		         "/// }\n"
		         "/// ```\n"
		         "integrate :: sys (pos, vel) {\n"
		         "  pos = pos + vel;\n"
		         "}\n",
		         name, name, name);
		return write_new_file(path, content);
	}

	if (strcmp(kind, "driver") == 0) {
		/* A driver is the top-level program; scaffold <name>.arche. */
		snprintf(path, sizeof(path), "%s.arche", name);

		/* `arche init driver <name> <dev>...` — scaffold a driver that imports the named devices and
		 * pre-fills a pool for each shape they require, at the datasheet minimum (editable). */
		if (argc > 3) {
			char content[2048];
			int n =
			    snprintf(content, sizeof(content), "// %s — a driver importing the named device(s).\n#import { ", name);
			for (int i = 3; i < argc; i++)
				n += snprintf(content + n, sizeof(content) - (size_t)n, "%s ", argv[i]);
			n += snprintf(content + n, sizeof(content) - (size_t)n,
			              "fmt }\n\nmain :: proc() {\n  // call the imported devices' systems here\n  "
			              "fmt.printf(\"ran\\n\");\n}\n");
			if (write_new_file(path, content) != ARCHE_OK)
				return ARCHE_ERR;
			/* Pull each device's required pools from its datasheet into the driver source. */
			arche_fill_driver(path);
			return ARCHE_OK;
		}

		char content[2048];
		snprintf(content, sizeof(content),
		         "// %s — a driver: it imports devices, sizes their shapes, and runs their systems.\n"
		         "// Replace `physics` with the device(s) you depend on.\n"
		         "#import { physics fmt }\n"
		         "\n"
		         "physics.Particle[1000]    // the driver picks the storage size\n"
		         "\n"
		         "main :: proc() {\n"
		         "  insert(physics.Particle, 10.0, 1.0);\n"
		         "  run physics.integrate;\n"
		         "  fmt.assert(physics.Particle.pos[0] * 10 == 110, \"integrate did not run\\n\");\n"
		         "  fmt.printf(\"ran\\n\");\n"
		         "}\n",
		         name);
		return write_new_file(path, content);
	}

	fprintf(stderr, "arche init: kind must be 'device' or 'driver' (got '%s')\n", kind);
	return ARCHE_USAGE;
}
