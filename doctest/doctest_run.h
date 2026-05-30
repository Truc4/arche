#ifndef ARCHE_DOCTEST_RUN_H
#define ARCHE_DOCTEST_RUN_H

/* Run all doctests in a single `.arche` file: parse it, extract every ```arche
 * example from its doc comments, compile each (with the file's API in scope via
 * `use`) and run it. An example passes iff it compiles and exits 0. Prints a
 * per-example report and a summary. Returns 0 if all passed (or none found),
 * non-zero if any failed or the file could not be read/parsed. */
int doctest_run_file(const char *path);

/* Run doctests over a path spec:
 *   - a `.arche` file      → that file (same as doctest_run_file);
 *   - a directory          → every `.arche` file under it, recursively;
 *   - a Go-style `<dir>/...` or `...` → recursive from <dir> (or cwd).
 * Recursive runs print one `ok`/`FAIL` status line per file that has examples
 * (go-test style) and stay silent on files with none. `verbose` adds a per-
 * example PASS/ignore line. Returns non-zero if any example failed. */
int doctest_run_path(const char *spec, int verbose);

#endif /* ARCHE_DOCTEST_RUN_H */
