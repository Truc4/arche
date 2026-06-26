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
		/* A device is a folder: its component TYPES + storage requirements live in <name>.ds.arche (the
		 * datasheet, which defines no shape); its SHAPE + BEHAVIOR (systems/procs) live in <name>.arche.
		 * A driver provides the pool (the datasheet states the minimum). The doctest in the impl drives
		 * the device's own shape — `arche test` compiles it as a generated driver over the device folder. */
		mkdir(name, 0755);

		char ds_path[600];
		snprintf(ds_path, sizeof(ds_path), "%s/%s.ds.arche", name, name);
		const char *ds = "// Datasheet: the device's required components + storage requirements (NOT its impl). A\n"
		                 "// datasheet describes requirements only — it never defines a shape. A driver provides the\n"
		                 "// `Particle` pool; `[4]Particle` is the minimum it must size.\n"
		                 "pos :: float;\n"
		                 "vel :: float;\n"
		                 "[4]Particle;\n";
		if (write_new_file(ds_path, ds) != ARCHE_OK)
			return ARCHE_ERR;

		snprintf(path, sizeof(path), "%s/%s.arche", name, name);
		const char *impl =
		    "// A device: its shape + behavior. The `Particle` shape is defined here (a shape is global\n"
		    "// vocabulary, defined where it is used, not in the datasheet); the datasheet states the\n"
		    "// components + the `[4]Particle` storage requirement. The doctest below drives the shape —\n"
		    "// `arche test` compiles it as a generated driver, so the requirement provides the pool.\n"
		    "#import { fmt }\n"
		    "\n"
		    "Particle :: arche { pos, vel }\n"
		    "\n"
		    "// A query names the columns a map runs over — define it once, run maps over it by name.\n"
		    "Movers :: query { pos, vel }\n"
		    "\n"
		    "/// ```arche\n"
		    "/// seed :: system {\n"
		    "///   insert(Particle{ pos: 10.0, vel: 1.0 })(_:, _:);\n"
		    "/// }\n"
		    "/// check :: system {\n"
		    "///   fmt.assert(Particle.pos[0] * 10 == 110, \"integrate did not run\\n\")();\n"
		    "/// }\n"
		    "/// #run seq({ seed, integrate, check })\n"
		    "/// ```\n"
		    "integrate :: map(Movers) {\n"
		    "  pos = pos + vel;\n"
		    "}\n";
		return write_new_file(path, impl);
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
			              "fmt }\n\nreport :: system {\n  fmt.printf(\"ran\\n\");\n}\n"
			              "// schedule the imported devices' systems before `report` in this `#run`.\n"
			              "#run seq({ report })\n");
			if (write_new_file(path, content) != ARCHE_OK)
				return ARCHE_ERR;
			/* Pull each device's required pools from its datasheet into the driver source. */
			arche_fill_driver(path);
			return ARCHE_OK;
		}

		char content[2048];
		snprintf(content, sizeof(content),
		         "// %s — a driver: it imports devices, sizes their shapes, and runs their systems.\n"
		         "// Replace `physics` with the device(s) you depend on. A shape (`Particle`) is GLOBAL\n"
		         "// vocabulary — bare, never `physics.Particle`; only a device's systems are qualified.\n"
		         "#import { physics fmt }\n"
		         "\n"
		         "[1000]Particle;    // the driver picks the storage size for the global shape\n"
		         "\n"
		         "seed :: system {\n"
		         "  insert(Particle{ pos: 10.0, vel: 1.0 })(_:, _:);\n"
		         "}\n"
		         "check :: system {\n"
		         "  fmt.assert(Particle.pos[0] * 10 == 110, \"integrate did not run\\n\")();\n"
		         "  fmt.printf(\"ran\\n\");\n"
		         "}\n"
		         "#run seq({ seed, physics.integrate, check })\n",
		         name);
		return write_new_file(path, content);
	}

	fprintf(stderr, "arche init: kind must be 'device' or 'driver' (got '%s')\n", kind);
	return ARCHE_USAGE;
}
