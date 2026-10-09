# llvm-ir-mutator

Produces mutated variants of a given LLVM IR functions.

This tool reuses some code from `llvm/lib/FuzzMutate`: the eight upstream
strategies are used as-is, and nine more are added alongside them through the
same `IRMutationStrategy` interface.

## Prerequisites

- Recent build of [LLVM](https://github.com/llvm/llvm-project)

## Build

```
cmake -S . -B build -G Ninja -DLLVM_PATH=/path/to/llvm-project/build
ninja -C build
```

`LLVM_PATH` is a build or install root; `LLVM_DIR` is derived from it. RTTI and
exception settings are mirrored from `LLVMConfig.cmake`, so they cannot drift
out of sync with the LLVM being linked.

## Use

```
./build/llvm-ir-mutator seed.ll                        # endless, NUL-separated
./build/llvm-ir-mutator seed.ll --count=1000 > corpus  # bounded
./build/llvm-ir-mutator seed.ll --only=int-width --report
./build/llvm-ir-mutator --list                         # every pathway
```

Each mutant carries a header naming the pathways that fired and its own seed:

```
; mutant=7 seed=6878563960102566144 applied=insert-call,inject,wrap,attrs,wrap
```

`--seed` fixes the master seed and makes a whole run byte-reproducible.
`--mutant-seed` takes the value out of a header and replays that one mutant on
its own, which is what you want once a mutant has found something.

| Flag | Meaning |
|---|---|
| `--seed=N` | master seed (default: nondeterministic, echoed to stderr) |
| `--mutant-seed=N` | replay a single mutant from its header seed |
| `--count=N` | stop after N mutants (default: endless) |
| `--mutations=N` | mutations per mutant (0: uniform in [1,5]) |
| `--sep=nul\|nl\|none\|<str>` | record separator (default `nul`) |
| `--only=A,B` / `--disable=A,B` | restrict the pathway set |
| `--rollback=step\|mutant` | verify each mutation, or once per mutant |
| `--max-size=N` | module object budget |
| `--emit=ll\|bc` | output format |
| `--alive2-safe` | emit only what Alive2 can reason about (see below) |
| `--lifter-types` | reject mutations that add vector types or FP types other than `half`/`float`/`double` |
| `--report` | per-pathway table to stderr on exit |

## Pathways

Eight come from `llvm/lib/FuzzMutate`: `inject`, `delete`, `modify`,
`insert-call`, `insert-cfg`, `insert-phi`, `sink`, `shuffle`.

Nine are defined in `src/Strategies.cpp`:

| Pathway | What it does |
|---|---|
| `flags` | `disjoint`, `nneg`, `samesign`, `trunc nuw/nsw`, GEP `nusw`/`nuw`, `is_zero_poison` immediates, load/store volatility and alignment |
| `const` | replaces constants with: 0/1/-1/signed extremes, ±1 ULP, inf and NaN, other values that appear in the original IR, or with a completely random value of the type|
| `opcode` | swaps an opcode for a sibling in its signature class (i.e. `add`->`sub` or `shl`->`ashr`), rebuilding the instruction so the old opcode's flags cannot survive |
| `int-width` | rebuilds a chain of integer arithmetic at a different bit width with `zext`/`trunc` adapters. This is the only way types are resized |
| `break-use` | gives a single-use value a second user, via the same `connectToSink` machinery `sink` uses, so every `hasOneUse()`-guarded fold stops firing |
| `attrs` | toggles function, parameter and return attributes, including `range`, `align`, and `dereferenceable` |
| `wrap` | interposes `freeze`, `fneg`, `fabs` or a widen/narrow round-trip on an operand |
| `move` | relocates an instruction inside the window bounded by its operands and its uses |
| `splice` | retargets a call at another function of the same signature, or inlines the callee |

## Configuring

`src/Config.h` is the single edit point. It holds the pathway weight table, the
type pool, the retry bounds and the `RandomIRBuilder` knobs. Nothing else needs
touching to re-weight, disable, or extend the mix.

`src/Ops.cpp` holds the operation table the `inject` pathway draws from. It is
plain data, and adding an operation is about five lines.

## Design notes

**The driver.** `llvm::IRMutator::mutateModule` applies exactly one strategy per
call, reseeds its `RandomIRBuilder` every time, and reports nothing about what
it did. `src/Mutator.cpp` keeps the strategy interface but replaces that loop, so
mutations accumulate under one engine and each pathway's effect is measured.

**Observability.** Every pathway records `chosen` / `applied` / `no-op` /
`invalid`. `no-op` is decided by comparing the printed module before and after,
which is the only fully general way to answer "did that actually change
anything". Without it a pathway can look busy while doing nothing, which is the
failure mode `tools/coverage.sh` exists to catch.

**Validity.** Under the default `--rollback=step` each mutation is applied to a
clone, verified, and rolled back if it does not change the module or does not
verify. Mutants are therefore verifier-clean by construction. `--rollback=mutant`
verifies once per mutant instead: faster, but per-pathway `invalid` counts are
no longer attributable.

**Diversity.** Upstream's `InjectorIRStrategy::getDefaultOps()` omits `select`
and `fneg`, and LLVM ships no cast, `freeze`, memory, or intrinsic descriptors at
all. `src/Ops.cpp` adds all of them. Measured over 300 mutants of
`tests/seeds/all.ll`:

| Operation set | distinct opcodes | distinct intrinsic families |
|---|---|---|
| upstream default (`--baseline-ops`) | 35 | 3 |
| expanded table | 45 | 45 |
| all 17 pathways | 55 | 37 |

The type pool is also wider: odd integer widths (`i2`, `i3`, `i7`, `i17`),
`half`, `bfloat`, `fp128`, and vectors including a non-power-of-two lane count.
backend-tv's RISC-V lifter refuses vectors and every FP type but `half`,
`float` and `double`; `--lifter-types` keeps those out of the pool and rolls
back any mutation that introduces one anyway, unless the seed already had one.

**Alive2 compatibility.** By default the mutator emits anything the LLVM
verifier accepts, which includes a few things Alive2 cannot use. `--alive2-safe`
removes them. They fall into two groups.

Refused outright by Alive2's translator, so the mutant is wasted:

| Construct | Alive2 |
|---|---|
| `load volatile` / `store volatile` | `ERROR: Unsupported instruction` |
| `noalias` on a pointer argument | `ERROR: Unsupported attribute` |
| scalable vectors | `ERROR: Unsupported type` |
| a recursive call | callee reads as `function did not return` |
| `range(...)` on a parameter | an out-of-range argument is poison that lifted code cannot mirror |
| fast-math flags on `sitofp`/`uitofp` | `ERROR: Unsupported instruction` |
| a load or store through `poison`/`undef` | `ERROR: Source function is always UB` |

`range` is never added under `--alive2-safe`; attributes already in the seed
are kept.

FuzzMutate invents a `poison` pointer when it needs one and has none; the
scrubber erases the loads and stores through it, giving a load's uses poison.

Recursion is the one entry that is scrubbed rather than refused: the mutant
still verifies, but Alive2 models the recursive callee as never returning, so
the refinement check reports a difference for every such mutant no matter what
the backend did. `--alive2-safe` therefore breaks every cycle in the module's
direct call graph, erasing the call that closes it and giving its uses poison.
Indirect calls and calls to declarations are left alone, since neither can be
shown to close a cycle.

The more annoying ones are those which can silently fail: `afn`, `arcp`,
`contract` and `reassoc` are modelled as uninterpreted functions (`ir/instr.cpp`,
via `doesApproximation`). Alive2 stays sound, but any counterexample that
touches one is discarded with "Alive2 approximated the semantics ... cannot
conclude whether the bug found is valid or not". A genuine miscompile that
Alive2 catches as `1 incorrect transformations` becomes `0 incorrect
transformations` once the source carries `afn`. Since `fast` implies all four,
unrestricted fast-math mutation is the single biggest source of masked bugs.

`nnan`, `ninf` and `nsz` are modelled exactly and are kept. In practice
`--alive2-safe` *increases* their frequency, because `fast` decomposes into the
three sound flags instead of being dropped whole.

The scrub runs after the strategy rather than inside it, so it also covers the
upstream strategies, whose code this project does not own. A mutation whose only
effect was a scrubbed construct reads as a no-op and is resampled, so the
pathway budget is not wasted.

Everything else the mutator emits is ordinary IR. Verified empirically: across
800 `--alive2-safe` mutants, Alive2 reported zero unsupported constructs.

**Termination.** The default endless stream is deliberate; bound it with
`--count`, or let the consumer close the pipe. Every internal retry loop is
bounded by a constant in `src/Config.h`, so no run can wedge. Generated mutants
may themselves contain loops — the mutator only parses and verifies IR, never
executes it.

## Verifying

```
./tools/coverage.sh   # every pathway must change a module at least once
./tools/smoke.sh      # every emitted mutant must verify and survive -O2
```

Both bound every invocation with `--count` and `timeout`. `coverage.sh` exits
non-zero if any pathway never fires, which is what makes an inert pathway a
build failure rather than a silent gap.

`tools/smoke.sh` additionally asserts that `--alive2-safe` emits none of the
hostile constructs while keeping the soundly-modelled flags. It greps the
output rather than invoking Alive2, so it needs no Z3 and no Alive2 build.

`tests/seeds/all.ll` is deliberately broad — integer and float arithmetic,
vectors, pointers and GEPs, repeated call signatures, a loop, and single-use
chains — because a pathway with nothing to act on would otherwise report as
broken.
