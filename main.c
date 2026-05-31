#include "cli/cli.h"

/* The `arche` entry point is a thin shim: all CLI logic (subcommand dispatch, argument parsing,
 * help) lives under cli/. See cli/cli.c (cli_main) and cli/cmd_*.c. */
int main(int argc, char *argv[]) {
	return cli_main(argc, argv);
}
