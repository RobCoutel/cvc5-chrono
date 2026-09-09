# Bug report: `regress2/ooo.tag10.smt2` crashes with chronological backtracking

## Summary

With chronological backtracking (CB) enabled in the MiniSat backend
(branch `cb-backtrack`), the regression `ooo.tag10.smt2` (`QF_UFIDL`) crashes
in `--check-unsat-cores` and `--check-proofs` mode. The crash signature
points at `TheoryEngine::getExplanation`, but that is a downstream symptom.
Direct instrumentation shows the SAT solver's own bookkeeping is internally
consistent throughout the run; the arithmetic theory reports a conflict
that references a fact whose underlying SAT literal has already been
retracted (and never re-derived) at the moment the conflict is built. This
looks like a theory-side (arithmetic) issue in how conflict/fact state is
kept in sync with chronological (non-monotonic) backtracking, not a bug in
the MiniSat-level CB implementation itself.

## Environment

- Branch: `cb-backtrack`, commit `670302d02` ("don't backtrack on lemma
  implications"), on top of `1b94f65d1` ("bare bones CB implementation
  with backtracking to handle MLIs").
- Relevant options added on this branch:
  - `booleans::chronologicalBacktracking` (`--chronological-backtracking`),
    default `true`.
  - `prop::satSolverMode` default changed from `CADICAL` to `MINISAT`.
- `set_defaults.cpp` disables CB automatically outside a "validated" logic
  set (no quantifiers, no higher-order, no arrays/datatypes/sep/sets/bags/
  strings/ff/fp, arithmetic must be linear with no transcendentals). This
  test (`QF_UFIDL`) is inside that validated set, so CB stays enabled.

## Reproduction

```
ctest -R "regress2/ooo.tag10.smt2" -V
```

or directly:

```
cvc5 --check-unsat-cores test/regress/cli/regress2/ooo.tag10.smt2
```

`base` mode (no extra flags) passes. `unsat-core` mode
(`--check-unsat-cores`) and `proof` mode (`--check-proofs
--proof-check=lazy`) both crash the same way.

## Crash signature

```
Fatal failure within cvc5::internal::TrustNode
cvc5::internal::TheoryEngine::getExplanation(std::vector<cvc5::internal::NodeTheoryPair>&)
at src/theory/theory_engine.cpp:2030
Check failure

 explanation != toExplain.d_node
wasn't sent to you, so why are you explaining it trivially, for fact
(not (= (+ 1 @purify_8) (+ 1 @purify_21)))
```

(The exact `@purify_N` numbering varies slightly run to run depending on
purification order, but the shape and root cause are the same.)

Full backtrace (lldb, from the user's own session — matches gdb backtraces
taken independently):

```
frame #6:  TheoryEngine::getExplanation            theory_engine.cpp:2030
frame #7:  TheoryEngine::conflict                  theory_engine.cpp:1725
frame #8:  OutputChannel::trustedConflict          output_channel.cpp:123
frame #9:  TheoryInferenceManager::trustedConflict theory_inference_manager.cpp:137
frame #10: LinearSolver::outputTrustedConflict     linear_solver.cpp:97
frame #11: TheoryArithPrivate::outputTrustedConflict theory_arith_private.cpp:2185
frame #12: TheoryArithPrivate::outputConflicts     theory_arith_private.cpp:2129
frame #13: TheoryArithPrivate::postCheck           theory_arith_private.cpp:3769
frame #14: LinearSolver::postCheck                 linear_solver.cpp:79
frame #15: TheoryArith::postCheck                  theory_arith.cpp:255
frame #16: Theory::check                           theory.cpp:600
frame #17: TheoryEngine::check                     theory_engine.cpp:468
frame #18: TheoryProxy::theoryCheck                theory_proxy.cpp:271
frame #19: Minisat::Solver::theoryCheck            Solver.cc:1365
frame #20: Minisat::Solver::propagate              Solver.cc:1280
frame #21: Minisat::Solver::search                 Solver.cc:1585
...
```

The `id` at frame #7/#9 is `InferenceId::ARITH_CONF_FACT_QUEUE` — the
conflict originates spontaneously inside arithmetic's own `postCheck`, not
from a SAT-driven `Solver::analyze()` → `reason()` explanation request.

## Investigation

### 1. `getExplanation`'s assert is a downstream symptom, not the cause

`TheoryEngine::getExplanation` first checks `d_propagationMap` (a
context-dependent `CDHashMap`, see `theory_engine.h:620` /
`theory_engine.cpp:235`) for a record of who explains the fact; only on a
miss does it fall through to asking the theory directly for an explanation
(`d_sharedSolver->explain(...)`), and *that* result is what fails the
`explanation != toExplain.d_node` assert (`theory_engine.cpp:2030`).

Confirmed with gdb, at the crash frame:

```
(gdb) print d_propagationMap.find(toExplain) != d_propagationMap.end()
$1 = false
```

i.e. a genuine miss, not a stale/mistimed cache hit. `theoryExpPropagation
(THEORY_ARITH)` returns `THEORY_ARITH` (no shared-equality-engine
remapping), so the miss isn't a theory-id key mismatch either — the map
key really is absent.

`d_propagationMap` and `d_propagationMapTimestamp` are `context::CDO`-based,
tied to the same `d_context` that `Solver::cancelUntil` pops via
`d_context->pop()`. A miss here is *expected and correct* once the SAT
solver has chronologically backtracked past the point this fact was
registered — this part of the machinery is behaving exactly as designed.

Two earlier hypotheses were tested and **refuted**:

- *"A `CRef_Lazy`-reasoned (never-explained) literal gets requeued by
  `cancelUntil`'s `toRequeue` mechanism."* Disproved by adding
  `Assert(reason != CRef_Lazy)` at the `toRequeue` replay site
  (`Solver.cc`, in `cancelUntil`) and rerunning — it never fires. Also
  supported by static reading: the only place that lowers `vardata[x].d_level`
  below the level it was assigned at is `Solver::reason()`'s CB-demotion
  logic (`Solver.cc:394`), which always converts `d_reason` away from
  `CRef_Lazy` in the same step, so a lowered level and a still-`CRef_Lazy`
  reason can't co-occur via that path.
- *"`d_propagationMapTimestamp`'s context-dependent counter reverts on a
  mid-search pop and reissues timestamps, corrupting the cache-freshness
  check."* Refuted directly — `d_propagationMap.find()` returns a genuine
  miss (see above), not a stale-but-present entry, so the freshness
  comparison (`(*find).second.d_timestamp < toExplain.d_timestamp`) is
  never even reached for this fact.

### 2. The fact is asserted to the theory multiple times, via different mechanisms

Tracing `TheoryProxy::enqueueTheoryLiteral` / `TheoryEngine::assertFact`
for the exact crashing fact (matching on the interned `Node`'s pointer
identity, which is stable across repeated runs of this deterministic,
single-threaded build) shows it gets delivered to the theory several
times over the course of the run, via **both**:

- `Solver::propagateTheory()` → `uncheckedEnqueue(p, CRef_Lazy,
  decisionLevel())` — an arithmetic theory propagation, and
- `Solver::propagateBool()` → `uncheckedEnqueue(first, cr,
  computeClauseLevel(cr))` — ordinary BCP from a learned/existing clause
  (`cr` = clause ref `33134` in this run).

Each delivery does get drained by `TheoryProxy::theoryCheck()` and passed
to `TheoryEngine::assertFact`, which (since the atom is `EQUAL` and
sharing is enabled) calls `assertToTheory(..., THEORY_SAT_SOLVER)` twice
(once to `THEORY_ARITH`, once to `THEORY_BUILTIN`), each populating
`d_propagationMap` via `markPropagation`. So *TheoryEngine's* bookkeeping
is repeatedly, correctly re-established every time the fact is
(re)asserted.

### 3. The SAT-level bookkeeping is self-consistent — confirmed by full trace

The variable underlying this fact was identified precisely (SAT variable
`1893`, found by matching the `Node` pointer through
`TheoryProxy::enqueueTheoryLiteral`). A full instrumented run (all
`Solver::cancelUntil` calls, all writes to `vardata[1893]`, all
`uncheckedEnqueue` calls for it) was captured for the entire solve. Key
excerpts (`old_level`/`target_level` are the arguments to the
"keep-or-drop" check in `cancelUntil`, `d_level <= level`):

```
UNCHECKEDENQUEUE var=1893 from=-2(CRef_Lazy)  lvl=161   # theory propagation
...
POP-LOOP x=1893 old_level=161 target_level=160 WILL_KEEP=0   # correctly dropped (161>160)
UNCHECKEDENQUEUE var=1893 from=-2(CRef_Lazy)  lvl=161   # re-propagated by theory
...
POP-LOOP x=1893 old_level=161 target_level=160 WILL_KEEP=0
UNCHECKEDENQUEUE var=1893 from=-2(CRef_Lazy)  lvl=160
...
POP-LOOP x=1893 old_level=160 target_level=158 WILL_KEEP=0
...
UNCHECKEDENQUEUE var=1893 from=33134(clause)  lvl=162   # BCP from a real clause
...
POP-LOOP x=1893 old_level=162 target_level=161 WILL_KEEP=0
UNCHECKEDENQUEUE var=1893 from=33134(clause)  lvl=162
...
POP-LOOP x=1893 old_level=162 target_level=161 WILL_KEEP=0   # last event for var 1893
... (many more cancelUntil calls, incl. a full reset to level 0 and
     climbing back to level 161, with var 1893 unassigned throughout) ...
CANCELUNTIL target=161 cur_decLvl=161 v1893_level=-1 v1893_assign=l_Undef
Fatal failure ... (the crash)
```

Every single drop decision satisfies `old_level > target_level` — exactly
the CB invariant `d_level <= level ⇒ keep, else drop`. Every level value
came from an actual propagation event (a real theory propagation or a
real BCP derivation), never from anything unexplained. At no point does
the trace show a variable kept when it should have been dropped, dropped
when it should have been kept, or a level value that doesn't trace back to
a concrete propagation. **The MiniSat/CB-level bookkeeping for this
variable is internally consistent throughout.**

At the moment of the crash, confirmed directly:

```
vardata[1893] = { d_reason = 33134 (stale), d_level = -1, d_trail_index = -1 }
assigns[1893] = l_Undef
decisionLevel() = 161
```

The variable was correctly, chronologically retracted by the last
`POP-LOOP old_level=162 target_level=161` and **never re-derived
afterward**, through several further `cancelUntil` calls (including
another full reset to level 0 and a climb back to 161) — right up to the
point where arithmetic's `postCheck` builds a conflict that references it.

### Conclusion

The bug is not in the SAT-level CB implementation (`Solver.cc`): every
assign/retract decision traced is correct by construction, and
`TheoryEngine`'s own context-dependent bookkeeping (`d_propagationMap`)
correctly forgets a fact once the SAT context pops past it.

The bug is that **`TheoryArithPrivate`'s own conflict-detection state
(the `ARITH_CONF_FACT_QUEUE` path in `postCheck`/`outputConflicts`,
`theory_arith_private.cpp`) is not being invalidated in sync with a
chronological backtrack.** It reports a conflict built from a fact that,
at that point in the search, has no live SAT-level justification — the
corresponding literal is `l_Undef`. Under standard (non-chronological)
backtracking this situation cannot arise, because SAT decision levels and
`d_context` push/pop nest 1:1 and monotonically; CB breaks that 1:1
nesting (a `cancelUntil` can pop context levels without necessarily
undoing exactly the same set of facts a *linear* backtrack to that target
would have undone, and the search can climb back to a higher decision
level without re-deriving everything that was true the last time it was
at that level). Whatever internal state arithmetic uses to accumulate
facts for `ARITH_CONF_FACT_QUEUE` conflicts appears to either not be
purely `context::CDO`-based, or to buffer/queue facts across `check()`
calls in a way that can outlive the SAT-level retraction of one of those
facts.

## Suggested next steps (for someone familiar with the arithmetic theory)

- Look at how `TheoryArithPrivate` accumulates facts/rows for
  `ARITH_CONF_FACT_QUEUE` conflicts (`theory_arith_private.cpp`, around
  `outputConflicts` / `postCheck`, and wherever that fact queue is filled)
  and check whether that state is torn down by `notifyBacktrack`/context
  pop the same way `d_propagationMap` is, including for a chronological
  (non-monotonic, "jumps around instead of strictly decreasing") pop
  sequence rather than the simple nested case.
- In particular check whether the queue is populated incrementally across
  multiple `check()`/`postCheck()` invocations (rather than fully rebuilt
  fresh each call) — if so, a fact queued in one round could survive an
  intervening `cancelUntil` that retracts it, and still be present when a
  later round finally reports the conflict.
- It may be worth asserting, at the point `outputConflicts` builds a
  conflict node, that every literal in it currently has a SAT value
  matching what's expected (i.e. add a check analogous to what was used
  to pin this down: `d_propEngine->hasValue(lit, value)` for each
  conjunct) to catch this class of staleness at the source rather than
  later in `getExplanation`.

## How this was diagnosed

- `ctest -R "regress2/ooo.tag10.smt2" -V` to reproduce and see per-mode
  results.
- gdb on the `build/` binary (assertions enabled) to get backtraces at the
  point of the `Check failure` (breaking on the process's natural abort,
  since the `FatalStream` destructor calls `abort()`).
- Conditional breakpoints keyed on the specific fact's interned `Node`
  pointer (stable across repeated runs of this deterministic build, with
  gdb's default ASLR-disabled inferior) to trace every
  `enqueueTheoryLiteral` / `assertFact` / `cancelUntil` event touching
  that one fact across the whole solve, without drowning in unrelated
  noise.
- Cross-checked against the user's own lldb session/backtrace.

No source changes were made as part of this investigation (aside from one
experimental `Assert(reason != CRef_Lazy)` the user added and removed
themselves to test and refute the `toRequeue`/`CRef_Lazy` hypothesis).
