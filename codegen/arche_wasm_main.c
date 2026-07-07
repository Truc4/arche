// arche_wasm_main.c — the BROWSER entry point for the Arche compiler compiled to wasm32-wasi. It calls the
// compile pipeline DIRECTLY (bypassing the CLI, whose signal/rlimit/popen aren't in WASI), so the ~1MB
// compiler runs client-side: read /work/in.arche, compile with the DIRECT wasm backend (ARCHE_WASMGEN set by
// the host env), write /work/out.wasm. The host (browser) supplies the source via a virtual FS and reads the
// result back. This is what makes the playground serverless.
#include "../compile/compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = 0;
  fclose(f);
  return buf;
}

int main(void) {
  char *src = slurp("/work/in.arche");
  if (!src) { fprintf(stderr, "arche-wasm: cannot read /work/in.arche\n"); return 2; }
  CompileOpts opts;
  memset(&opts, 0, sizeof(opts));
  opts.target = TARGET_WASM32; // wasmgen fires via ARCHE_WASMGEN (set in the host env)
  int rc = compile_source(src, "/work/in.arche", "/work/out.wasm", &opts);
  free(src);
  return rc;
}
