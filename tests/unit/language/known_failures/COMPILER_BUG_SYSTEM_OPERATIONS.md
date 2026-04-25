# Compiler Bug: System Operations Don't Modify Archetype Fields

## Issue
When running systems with `run` statements, the system does not correctly modify archetype field values. The system executes but field modifications are lost.

## Example
```arche
arche Particle {
  pos: float,
  vel: float,
}

sys move(pos, vel) {
  pos = pos + vel;  // Should add velocity to position
}

proc main() {
  alloc Particle(10);
  insert(Particle, 1.0, 0.1);
  run move;
  let p = Particle.pos[0];  // Expected: 1.1, Actual: 1.0
  assert(p * 10.0 == 11, "Expected 1.1");  // FAILS
}
```

## Impact
All tests that use `sys` with `run` fail because field modifications don't persist after system execution.

## Tests Affected
13 tests fail due to this bug. They are in `known_failures/`:
- test_arch_full.arche
- test_arch_two_sys_two_archs.arche
- test_arch_single_sys_single_arch.arche
- test_arch_single_sys_two_archs.arche
- test_arch_two_run.arche
- test_arch_insert_then_run_both.arche
- test_arch_partial_field_match.arche
- test_arch_run_only.arche
- test_dealloc.arche
- test_pooling.arche (uses indirect system ops)
- test_soa.arche (uses system ops)
- test_two_archs.arche
- test_sys_ops.arche

## Root Cause
Likely in codegen or semantic analysis - system calls aren't properly mapping field modifications back to archetype data.

## Fix Priority
HIGH - Blocks majority of integration tests.
