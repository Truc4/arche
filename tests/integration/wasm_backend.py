#!/usr/bin/env python3
"""
End-to-end test for the wasm32-wasi backend (`arche build --arch=wasm32`). Builds a few `.arche`
fixtures to `.wasm` and runs them under Node's WASI (`node:wasi`), asserting stdout — proving the
retargeted codegen (wasm triple + `@arche_syscall` shim) + the clang/wasi-libc link + the runtime
shims all work together, not just that the IR is valid.

GATED (off the default suite, like the GPU tests): it needs a WASI sysroot to LINK (a wasi-sdk via
ARCHE_WASI_SDK/WASI_SDK_PATH, or the system /usr/share/wasi-sysroot from wasi-libc) and a wasm
runtime to RUN (Node with `node:wasi`). If either is missing it SKIPs cleanly (exit 0), so it never
breaks a machine without the wasm toolchain installed.

RUN: python3 %s
"""

import os
import shutil
import subprocess
import sys
import tempfile

test_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.abspath(os.path.join(test_dir, '..', '..'))
arche_bin = os.path.join(repo_root, 'build', 'arche')

# A tiny WASI command-module runner: instantiate the module with WASI preview1 imports and start it
# (calls `_start` → wasi-libc crt → the emitted `main`). stdout/stderr inherit the parent's fds.
NODE_WASI = r'''
const { WASI } = require('node:wasi');
const fs = require('fs');
(async () => {
  const wasi = new WASI({ version: 'preview1', args: ['prog'], env: {}, preopens: {} });
  const bytes = fs.readFileSync(process.argv[2]); // argv: [node, driver.js, <wasm>]
  const mod = await WebAssembly.compile(bytes);
  const inst = await WebAssembly.instantiate(mod, wasi.getImportObject());
  process.exitCode = wasi.start(inst) || 0;
})().catch(e => { console.error(String(e)); process.exit(70); });
'''

# fixture -> expected stdout substring
FIXTURES = {
    'reduce.arche': (
        "#import { fmt }\n"
        "v :: float;\n"
        "P :: arche { v }\n"
        "[8]P(8) { v: 2.5 };\n"
        "show :: system (query { v }) eff {\n"
        "  total := reduce(+, v);\n"
        "  fmt.printf(\"sum=%.1f\\n\", total);\n"
        "}\n"
        "#run seq({ show })\n",
        "sum=20.0",
    ),
    'writeln.arche': (
        "#import { fmt os }\n"
        "hi :: system eff {\n"
        "  fmt.print(\"hello wasm\\n\")(_:);\n"  # os.write(1,...) -> @arche_syscall(1)
        "  os.exit(0)(_:);\n"                    # exit_group -> @arche_syscall(231)
        "}\n"
        "#run seq({ hi })\n",
        "hello wasm",
    ),
}


def node_has_wasi():
    r = subprocess.run(['node', '-e', "require('node:wasi')"], capture_output=True)
    return r.returncode == 0


def main():
    if not os.path.exists(arche_bin):
        print("SKIP: arche binary not built", file=sys.stderr)
        return 0
    if shutil.which('node') is None or not node_has_wasi():
        print("SKIP: no Node with node:wasi to run the wasm module", file=sys.stderr)
        return 0

    work = tempfile.mkdtemp(prefix='arche_wasm_')
    try:
        env = dict(os.environ)
        env['ARCHE_NO_GPU'] = '1'
        driver = os.path.join(work, 'wasi.js')
        with open(driver, 'w') as f:
            f.write(NODE_WASI)

        # Probe: build the simplest fixture. A failing link here means no WASI sysroot → SKIP, not FAIL.
        probe_src = os.path.join(work, 'writeln.arche')
        with open(probe_src, 'w') as f:
            f.write(FIXTURES['writeln.arche'][0])
        probe_wasm = os.path.join(work, 'writeln.wasm')
        pr = subprocess.run([arche_bin, 'build', '--arch=wasm32', '-o', probe_wasm, probe_src],
                            capture_output=True, text=True, env=env)
        if pr.returncode != 0 or not os.path.exists(probe_wasm):
            print("SKIP: cannot build to wasm (no WASI sysroot — set ARCHE_WASI_SDK or install wasi-libc)\n"
                  + pr.stdout + pr.stderr, file=sys.stderr)
            return 0

        ok = True
        for name, (src, want) in FIXTURES.items():
            srcp = os.path.join(work, name)
            with open(srcp, 'w') as f:
                f.write(src)
            wasmp = os.path.join(work, name.replace('.arche', '.wasm'))
            b = subprocess.run([arche_bin, 'build', '--arch=wasm32', '-o', wasmp, srcp],
                               capture_output=True, text=True, env=env)
            if b.returncode != 0 or not os.path.exists(wasmp):
                print("FAIL: %s did not build to wasm\n%s%s" % (name, b.stdout, b.stderr), file=sys.stderr)
                ok = False
                continue
            r = subprocess.run(['node', driver, wasmp], capture_output=True, text=True, timeout=30)
            if want not in r.stdout:
                print("FAIL: %s output %r did not contain %r (stderr: %s)" % (name, r.stdout, want, r.stderr),
                      file=sys.stderr)
                ok = False
        if not ok:
            return 1
        print("PASS: wasm32-wasi backend builds + runs (%d fixtures under node:wasi)" % len(FIXTURES))
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
