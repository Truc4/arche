#ifndef ARCHE_ANALYZER_H
#define ARCHE_ANALYZER_H

/* The arche-analyzer entry point (editor analysis service). Accepts argv of the form
 * `<prog> --dump [file]` or `<prog> --serve`. Defined in arche_analyzer.c; called by the folded
 * `arche analyze` subcommand and by the standalone `arche-analyzer` shim. */
int analyze_main(int argc, char *argv[]);

#endif /* ARCHE_ANALYZER_H */
