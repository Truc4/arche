#include "arche_analyzer.h"

/* Standalone `arche-analyzer` binary: a thin shim over analyze_main so existing editor/LSP wiring
 * that spawns `arche-analyzer --serve` by name keeps working byte-for-byte. The same logic is also
 * reachable as `arche analyze`. */
int main(int argc, char *argv[]) {
	return analyze_main(argc, argv);
}
