# Missed lower implications (MLI) under chronological backtracking

Background: R. Coutelier, M. Fleury, L. Kovács, "Lazy Reimplication in Chronological
Backtracking", SAT 2024. Terminology (MLI, λ, invariants 1/4/6/8/9, WCB/RSCB/ESCB/LSCB)
follows that paper.

## Symptom

On the `minisat-rscb` branch, `--check-proofs` fails on ~57 regress0 benchmarks
(e.g. `regress0/seq/rev.smt2`, 8/8 reproducible standalone) with:

```
Fatal failure within NodeBuilder::constructNV() at node_builder.cpp:411
Nodes with kind `or` must have at least 2 children (the one under construction has 1)
```

SATSentinel's own invariant checker (`--sat-sentinel "--check-only"`), run on the same
input, aborts earlier and more informatively with:

```
Invariant violation (No Missed Implications): clause C1536 ... has only one undefined
literal v973.
Invariant violation (Strong Watched Literals):
 c1 = ~v832,  δ(c1) = 18
 c2 = v973,   δ(c2) = 21
 b  = v973,   δ(b)  = 21
```

This is a violation of the paper's Invariant 4 (Strong watched literals) / Invariant 6:
a clause's satisfied watched literal (v973) sits at a decision level (21) strictly higher
than the level implied by the rest of the clause (18) — the paper's definition of an MLI.

## Root-cause trace (gdb, conditional breakpoint on the sentinel's clause-id counter at
`SentinelWrapper.h:265`, `id.value == 1536`)

- Clause 1536 (`CRef` 11099, 15 literals) is built by `Solver::reason(Var x=973)`
  ([Solver.cc:380-485](Solver.cc#L380)), the lazy path that materializes a theory's
  explanation clause on demand. It is called from `analyze()` ([Solver.cc:1104](Solver.cc#L1104))
  while resolving an unrelated conflict and needing the justification for var 973.
- At attach time (`ca[cr]`, dumped via gdb):
  - `c[0]` = var 973, l_True, **level 21**, trail_index 886 (past `qhead`=883 — not yet
    BCP-processed)
  - `c[1]` = var 832, l_False, **level 18** — the highest-level falsified literal
  - the remaining 13 literals are l_False at levels ≤ 10
- v973 was originally assigned with reason `CRef_Lazy` by `Solver::propagateTheory()`
  ([Solver.cc:1415-1444](Solver.cc#L1415)) — the only call site in the file that does this:
  ```cpp
  if (value(p) == l_Undef) {
    uncheckedEnqueue(p, CRef_Lazy, decisionLevel());
  }
  ```
  This unconditionally stamps the literal with whatever `decisionLevel()` happens to be at
  the moment the theory propagates it — it has no way to know what level the theory's real
  explanation will eventually justify, because that explanation is not computed until
  `reason()` is lazily called (here, much later, mid-`analyze()`, when it is too late to
  backtrack and fix it).
- `updateLemmas()` *does* have an analogous eager check-and-correct step today
  ([Solver.cc:2328-2342](Solver.cc#L2328), the "missed lower implication" branch: detects
  `l0_level > implicationLevel` and backtracks to reimply at the lower level) but this only
  covers lemmas processed through that function's batch, not literals enqueued lazily via
  `propagateTheory()`. `propagateTheory()` has no equivalent correction.
- `propagateTheory()` is unmodified by the current diff — this looks like a latent gap that
  the new CB-invariant checks (`attachClause`'s asserts, SATSentinel) are newly surfacing,
  not something the rewrite introduced.

## Planned fixes (two variants, both to be implemented)

### 1. Lazy reimplication (paper Section 4, Algorithm 1/3)

Maintain a λ map (`literal → clause`) of missed-lower-implication reasons, analogous to the
paper's Invariant 8. When a clause is attached (or otherwise observed) and found to be an
MLI for one of its literals — i.e. a satisfied watched literal at a level higher than the
rest of the clause justifies — record it in λ instead of fixing it immediately.
`cancelUntil` is extended to consult λ while unassigning: for each literal it unassigns
whose λ-recorded implied level is ≤ the target backtrack level, remember it, then after the
unassignment pass, re-enqueue those literals with their λ clause as reason and the correct
(lower) level.

Adaptation for cvc5 vs. the paper's Algorithm 1: cvc5 does not implement the paper's
BCP-time MLI detection (Algorithm 2) — only detection at clause-attachment time (an
RSCB-style detection, not full LSCB). `cancelUntil` continues to unassign-and-reimply
everything down to the decision as it already does today; the only addition is that MLI
literals get looked up in λ and reimplied with their corrected (lower) level and real
reason, instead of being silently reassigned at their old stale level.

Concretely, for the traced bug: when clause C1536 is attached, record λ[973] = C1536.
When later backtracking crosses level 18, v973 gets reimplied there instead of staying
wrongly pinned at 21.

Open design questions (unit lemmas have no backing clause to key λ on; how λ interacts
with `updateLemmas()`'s existing MLI branch; whether λ needs "keep the lowest" update
semantics or last-write-wins is sufficient given the reduced RSCB-style detection scope)
are still being worked out — see conversation/PR for resolution.

### 2. Elevate

At MLI-detection time, look at the clause's literal order relative to the trail: if some
other literal in the clause sits later in the trail than the currently-satisfied one, watch
that literal instead — ordinary repropagation on backtrack then rederives the implication
correctly with no extra bookkeeping. Otherwise (the satisfied literal is the last-placed one
in the trail among the clause's literals), it can be reimplied in place immediately, for
free, without violating topological order (Invariant 3), since nothing else in the trail can
depend on it being at its old, too-high position.

Caveat: decision levels may become approximately correct rather than exact. If an MLI
reimplies a variable that was a *decision*, the decision level it occupied needs to be
collapsed (merged into the level below), since it is no longer an independent decision.

### Scope

Both techniques replace `updateLemmas()`'s current bespoke MLI detection/correction
(the eager backtrack-based logic at [Solver.cc:2328-2342](Solver.cc#L2328)) as well: once
MLI handling is centralized, the batch-lemma processing order in `updateLemmas()` no longer
matters for correctness (MLI handling takes care of it), simplifying that function.

## Implementation log

Design decisions (confirmed with Robin): `d_lazyReason` (λ) is a separate `vec<CRef>` parallel
to `vardata`, not a new `VarData` field. `updateLemmas()` is fully simplified once
`attachClause()`/`cancelUntil()` handle MLI centrally (batch pre-sort and eager backtrack
branch deleted). Elevate and LSCB are separate strategies, not combined: LSCB keeps decision
levels exact at all times; elevate tolerates approximate levels on the rest of the trail as
long as topological order (Invariant 3) holds.

Bugs found and fixed, in the order they surfaced (each reproduced deterministically via
`--sat-sentinel "--check-only" --check-proofs --proof-check=lazy regress0/seq/rev.smt2`,
picked because it hits the bug 8/8 standalone, unlike the original `uaddo1.smt2` repro which
only failed under heavy parallel `ctest` load and was otherwise flaky in isolation):

1. **GC relocation root missing.** `d_lazyReason[v]` holds a `CRef` but `relocAll()` never
   relocated it, so a clause a variable's λ entry pointed at could be freed/moved under it.
   Fixed by relocating it alongside `vardata[v].d_reason` in `relocAll()`.
2. **MLI detection assumed too much clause shape.** `handleMissedLowerImplication()`
   originally treated *any* clause with a satisfied `c[0]`/`c[1]` as an MLI candidate and
   scanned the rest assuming they're all falsified — true for `reason()`'s explanation
   clauses, false for an ordinary theory lemma with other literals still `l_Undef`. Fixed by
   requiring every non-satisfied literal to actually be `l_False` before treating it as an
   MLI at all.
3. **`analyze()` can find a UIP that turns out to be a missed lower implication.**
   `cancelUntil()`'s own deferred reimplication (of an *unrelated*, earlier-recorded λ entry)
   can reassign the very variable `analyze()` just picked as the new clause's asserting
   literal, before `search()` gets to `uncheckedEnqueue()` it — violating that function's
   `value(p) == l_Undef` precondition. Fixed per Robin's guidance (matches the paper's
   Algorithm 4): if the freshly learned clause's asserting literal is still `l_False` after
   backtracking, the clause is genuinely still conflicting at the new (lower) level — attach
   it and re-run `analyze()` on it as the new conflict, looping until it isn't. Later
   extended to also finalize-and-return-UNSAT if the backtrack level ever reaches 0 inside
   this loop (see point 7) — `analyze()` cannot be called again at level 0.
4. **BCP's blocker optimization can hide an MLI entirely.** `propagateBool()`'s fast path
   (skip a watcher whose blocker is already satisfied, without even looking at the clause)
   silently swallows exactly the case where that satisfied blocker sits at a level higher
   than the literal just falsified. Per Robin: this needed real BCP-time detection (closer to
   the paper's Algorithm 2), not just attach-time detection — a watch-list rescan on
   reimplication wouldn't have been enough anyway, since a blocker can hide a clause from
   BCP without ever walking its watch list at all. Fixed by gating both the blocker fast path
   and the analogous `first`-is-satisfied fast path inside the clause-inspection loop on
   `level(blocker) <= level(propagated literal)` (elevate excluded — its BCP-time path is not
   attempted, see below), falling through to full inspection (and
   `detectMissedLowerImplication()`) otherwise. Required splitting the old
   `handleMissedLowerImplication()` into a watch-list-safe core
   (`detectMissedLowerImplication()`, handles the level-0 and LSCB-record cases, callable from
   BCP) and an elevate-only wrapper that does the in-place watch swap (only safe where the
   caller controls `c[]`/watches, e.g. `attachClause()` before they're published — *not*
   attempted from BCP, a known scope gap).
5. **Stale λ entries.** Once BCP records λ entries continuously (point 4), it's much easier
   for a `d_lazyReason[x]` entry recorded earlier to go stale before it's read again: nothing
   keeps that clause's *other* literals pinned in place just because `x`'s own reimplication
   is still pending. Fixed by adding `isValidLazyReason()` (all-other-literals-still-false
   check, no `Assert`) and calling it before every read of an *existing* λ entry (the "keep
   the lowest" comparison in `detectMissedLowerImplication()`, and the consumption check in
   `cancelUntil()`) — as opposed to a clause just freshly discovered, which doesn't need it.
6. **Eager level-0 fix broke topological order (Invariant 3).** The original design treated
   level 0 as always safe to fix immediately in place, for both CB strategies, reasoning only
   about *other* things depending on the stale value — not about whether the *new* reason's
   antecedents were positioned before the literal in the trail, which under CB is a separate
   axis from level. Robin: instead of eager reimplication (which "complicated everything"),
   add a dedicated sentinel `CRef_LazyRoot` (next to `CRef_Undef`/`CRef_Lazy` in
   SolverTypes.h) meaning "known to be reimpliable at level 0, no clause reference needed or
   kept." `detectMissedLowerImplication()`'s level-0 case now just does
   `d_lazyReason[x] = CRef_LazyRoot` via `recordLevelZeroMissedLowerImplication()`, for both
   CB strategies (elevate's in-place fix is only attempted for nonzero levels now).
   `cancelUntil()`'s λ-consumption special-cases `CRef_LazyRoot`: implied level is always 0
   (no clause scan), and it reimplies with `CRef_Lazy` as the *real* reason -- so a fresh
   explanation is (re)derived from the theory via `reason()` if one is ever asked for, through
   the exact same path (and the same proof handling within it, via
   `SatProofManager::notifyCurrPropagationInsertedAtLevel`) already used for every other
   `CRef_Lazy` literal, rather than something bespoke to this marker. This also let
   `eagerlyReimplyAtLevel0()`/`recomputeDecisionLevelsFrom()` be deleted outright — deferring
   to `cancelUntil()`, which by construction only ever reimplies once a literal's old trail
   position has already been unassigned, sidesteps the topological-order problem entirely
   rather than requiring a fix-up pass. `cancelUntil()`'s gate on this marker specifically
   ignores the `elevate` option (elevate otherwise does its own thing and should never have a
   *nonzero*-level λ entry, but the level-0 marker applies to both strategies).
   `updateLemmas()`'s unit-lemma case (no clause to attach, so it never reaches
   `handleMissedLowerImplication()` via `attachClause()`) uses the same
   `recordLevelZeroMissedLowerImplication()` instead of the old eager call.
7. **Conflict can become level-0 mid-retry (point 3's loop).** Robin flagged this
   proactively: a missed lower implication resolved via reimplication during the point-3 retry
   loop can lower the true conflict level to 0 without the outer `conflict_level == 0` check
   (checked once, before the loop) ever seeing it. Fixed by checking `backtrack_level == 0`
   inside the loop, after each `cancelUntil()`, before calling `analyze()` again — routes to
   the same `finalizeProof()`+`return l_False` path as the outer check instead.

### Open: theory confused by reimplication (not yet resolved)

After fixes 1-7, `rev.smt2` clears every purely-SAT-level MLI invariant (No Missed
Implications, Strong Watched Literals, Topological Order) but now hits a *theory-level*
contradiction: `reason(x=121)` asks `d_proxy->explainPropagation()` for `v121`'s
justification and gets back an explanation containing **both** `v121` and `¬v121` (same
variable, opposite polarity) -- a tautological, self-contradictory "explanation".
`vardata[121]` at the point of failure: `d_reason=CRef_Lazy, d_level=0`, i.e. this is a
variable `cancelUntil()` reimplied via the new `CRef_LazyRoot` marker mechanism (point 6).

Robin's hypothesis: the theory likely treats root-level (level 0) literals specially --
since they can never be backtracked away, the theory can permanently fold them in (a form of
inprocessing/clause-shortening), and does not expect to ever be asked to "un-propagate" or
re-propagate one via the normal backtrack/re-enqueue machinery. My reimplication path
currently goes through the *same* `uncheckedEnqueue()` as any other reimplication, which
calls `d_proxy->enqueueTheoryLiteral()` unconditionally whenever `theory[var(p)]` is set --
telling the theory about this (re-)assignment again, which may be exactly what confuses it
if it already permanently incorporated the fact the first time and isn't expecting to hear
about it again.

Proposed direction (Robin): once a literal is known to be at level 0, stop sharing further
(re-)assignment events for it with the theories at all -- add it back to the trail for the
SAT solver's own bookkeeping only, skipping the `enqueueTheoryLiteral()` call, rather than
going through the normal `uncheckedEnqueue()` path unconditionally. Needs investigating how
the theory/prop-engine layer currently treats level-0 facts before implementing, to confirm
this diagnosis and find the right place to special-case it.

### Investigation: theory-side mechanics (background agent, confirmed against cvc5 source)

- `Solver::d_proxy` is a concrete `prop::TheoryProxy*`
  ([Solver.h:121](Solver.h#L121)). `enqueueTheoryLiteral` (theory_proxy.cpp:370-378) just
  pushes onto a CD queue consumed by `theoryCheck` -> `TheoryEngine::assertFact`. No dedup
  guard against being told about the same literal twice.
- `explainPropagation`/`getExplanation` (theory_proxy.cpp:288-325, theory_engine.cpp:1396-1465)
  looks up a `(literal, THEORY_SAT_SOLVER)` entry in `d_propagationMap`, a `CDHashMap`. That
  entry is created *only* by a theory-initiated `TheoryEngine::propagate()` ->
  `assertToTheory(..., THEORY_SAT_SOLVER, ...)` -> `markPropagation` -- a different direction
  than `enqueueTheoryLiteral`'s `assertFact` path. Being a `CDHashMap`, the entry is erased
  automatically whenever the context pops past the level it was created at.
- Existing "level 0 is permanent" machinery in cvc5 (`Valuation::isFixed`/`PropEngine::isFixed`,
  `ZeroLevelLearner`/`getLearnedZeroLevelLiterals`) is all *query-on-demand*, not a push
  notification -- nothing a reimplication could "call" to tell a theory "this is now fixed."

First attempted fix (superseded, see below): skip `enqueueTheoryLiteral()` entirely for a
`CRef_LazyRoot` reimplication (added a `notifyTheory` parameter to `uncheckedEnqueue`), and use
`CRef_Undef` (not `CRef_Lazy`) as the real reason, matching `updateLemmas()`'s existing
unconditional-fact convention -- so nothing ever calls `explainPropagation` on it again either.
Result: traded the crash for a livelock. Traced via `-t minisat`: zero `"Conflict at level"`
messages over 18k+ trace lines / 6s, but ~380 `cancelUntil` calls cycling repeatedly across
levels 0-19, driven by `updateLemmas()` reacting to a stream of `"Theory propagated"` events for
the same handful of literals recurring over and over -- the theory kept re-deriving facts it was
never told had settled.

### Robin's second direction: reorder re-enqueued literals to group by level

Stable-sort `toRepropagate` (built highest-original-trail-index-first) by decreasing level
before the reverse-order re-enqueue loop, so literals sharing a level -- especially level 0 --
land as a contiguous run instead of interleaved with other levels, with an option to disable.
Implemented as `options().booleans.reorderRepropagation` (default true) in
[booleans_options.toml](../../../options/booleans_options.toml), applied to a `toRequeue`
vector merging what were previously the separate `toRepropagate`/`toReimply` lists (they'd
become structurally identical 3-tuples again once the `notifyTheory` skip was reverted, and
merging groups level-0 literals from *both* sources together, not just within each list
separately). `enqueueTheoryLiteral()`/`CRef_Undef` behavior otherwise reverted to normal
(always notify).

Result: no effect -- identical livelock signature (0 conflicts, ~460 `cancelUntil` calls, 5543
`"Theory propagated"` events across only 1230 distinct literals). A parallel investigation into
how Z3 handles level-0/base-level facts (`/home/robin/programs/z3`, background agent) came back
contradicting the ordering hypothesis directly: Z3's conflict-resolution code explicitly
tolerates base-level literals at *any* trail position
(`smt_conflict_resolution.cpp:570-572`, an explicit comment: "it may also be an (out-of-order)
asserted literal"); theories get no special notification for base-level facts at all (the
`assign_eh` callback carries no level information -- theories that care query
`ctx.at_base_level()` themselves). The one structurally relevant piece: Z3's delayed-unit-
reassertion mechanism (`smt_context.cpp:4186-4346`, `reassert_units`/`m_units_to_reassert`)
never tries to preserve/relocate a *prior* assignment event across a pop -- it fully re-derives
the fact via the same ordinary `assign(l, b_justification::mk_axiom())` call used for a fresh
axiom, every time.

### Root cause, finally identified: context-level mismatch, not ordering or notification-skipping

`Solver::d_context` (`context::Context*`) tracks SAT decision level 1:1: `Context::push()` is
called only from `newDecisionLevel()` ([Solver.h:876-880](Solver.h#L876-L880)), and
`context::Context` itself (`src/context/context.cpp:58-96`) is a pure scope stack -- every
`CDO<T>`/`CDHashMap` (including the theories' own bookkeeping) records its modifications at
*whichever context level is current* when touched, unwound automatically when that level is
popped. Nothing distinguishes level 0 to `Context` itself except that it's the floor nothing
pops below.

`cancelUntil(level)`'s context-pop loop ([Solver.cc](Solver.cc), the `for (... ) d_context->pop()`
right before the unassign loop) only pops down to *that call's target level*, not to 0. A
`CRef_LazyRoot` reimplication re-enqueued (and re-shared with the theory) during a call whose
target is, say, level 13, gets its theory-side support registered at *context level 13* -- even
though `vardata[x].d_level` claims 0 on the SAT side. The SAT solver believes `x` permanent and
never revisits it (nothing in `cancelUntil` re-examines a `d_level == 0` literal), but the
theory's record of it lives at context level 13 and vanishes the moment anything later pops
below 13, at which point the theory has no memory of `x` and re-derives/re-propagates it from
scratch -- repeatedly, matching the exact livelock signature above (~15-25 literals recurring
hundreds of times). `toRepropagate`'s pre-existing (non-MLI) mechanism never had this problem:
it always re-enqueues a literal at its *own, unchanged* original level, which by construction is
always `<=` the current backtrack target, so the context is guaranteed to still be at or above
that level -- it's specifically *lowering* a level below the current target (what a λ
reimplication does) that breaks the invariant.

Fix (current state): a new persistent (non-context-dependent, survives arbitrarily many
intermediate backtracks) member `d_pendingLevelZeroReimplications` (`vec<Lit>`,
[Solver.h](Solver.h) near `d_lazyReason`). A `CRef_LazyRoot` entry found in `cancelUntil`'s
unassign loop is pushed here instead of into `toRequeue`, regardless of that call's target
level. Only once a `cancelUntil` call's target is *actually* 0 (context truly at its floor,
right after the pop loop) is the queue drained into `toRequeue` (each entry re-checked for
`value(p) == l_Undef` first, since var(p) may have been legitimately reassigned by unrelated
means in the meantime) and cleared. A second `value(l) == l_Undef` check was needed in the
final consumption loop too: the same variable can be queued more than once across separate
`cancelUntil` calls before ever being drained (rediscovered as a level-0 MLI again before the
first entry got the chance to fire), which the build-time check alone didn't catch, causing an
`uncheckedEnqueue` precondition crash the first time this was tried.

TODO(minisat-rscb): the general (nonzero-level) λ reimplication in the `else if` branch right
after the `CRef_LazyRoot` case shares the exact same context-level mismatch whenever
`impliedLvl` ends up strictly below the current call's target level -- not yet addressed, only
the level-0 case has the deferred-queue treatment. Flagged inline at the call site too.

**Result of the deferred-queue fix**: the livelock is gone (confirmed via `-t minisat` trace:
no repeating pattern). But a new, deterministic (5/5) failure appeared: SATSentinel's
"Strong Watched Literals" invariant fires for a variable sitting in the pending queue, still at
its stale (too-high) level, because no `cancelUntil(0)` call has happened yet to drain it. This
is the flip side of fixing the theory-context mismatch: correctness from the *theory's*
perspective now requires waiting for a genuine level-0 backtrack, but correctness from the
*SAT invariant checker's* perspective requires the level tag to be fixed immediately -- these
two requirements are in direct tension.

### Simplification (Robin): backtrack to level 0 outright, abort analysis when it happens

Robin: this had become too complicated. Rather than deferring reimplication until a
convenient level-0 moment (the pending-queue design above), just force the backtrack to
level 0 immediately whenever a root-level reimplication is discovered, and "deal with it
the old way" -- i.e. revert to plain, immediate `toRequeue`-based reimplication, now made
correct because the context really is at 0 by the time it runs.

First attempt: made `cancelUntil(level)` scan for a pending `CRef_LazyRoot` marker and
silently widen its own `level` argument to 0 when found. This does not work: `cancelUntil`'s
contract with *every* one of its callers (`analyze()`, `search()`, `updateLemmas()`) is "pop to
*exactly* this level" -- silently doing more breaks whichever in-flight computation assumed
the smaller target. Caught concretely twice: (1) scanning the *whole* trail for a marker let an
unrelated variable trigger an escalation that also wiped out literals a different, unrelated
`cancelUntil` call's caller expected to survive (`computeClauseLevel` asserting on a literal
that should have stayed `l_False`); (2) even after scoping the *trigger* scan to just the range
`cancelUntil` was already going to touch, the escalation itself still over-reaches: any literal
sitting between level 1 and the original (smaller) target -- entirely unrelated to the marker --
gets wiped too, because that's what backtracking further inherently does, and `analyze()`'s
`learnt_clause` depends on exactly those literals staying `l_False`.

Robin: the fix has to be that **the analysis is aborted** when this happens, not that
`cancelUntil` tries to transparently absorb a wider backtrack. Implemented:

- A new counter `d_numPendingLevelZeroMLIs` (kept in sync at every site that sets/clears a
  `CRef_LazyRoot` marker) lets `analyze()` cheaply check, once per resolution step, whether a
  root-level MLI is pending -- checked right after each `reason()` call (which is also where a
  *new* one is most likely to appear, but the check equally catches one that was already
  pending before `analyze()` was even called, since the do-while loop always runs at least
  once).
- If found, `analyze()` aborts: cleans up the `seen[]` flags it has set so far (mirrors the
  ordinary end-of-function cleanup, using `trail.size()-1` as the same upper bound `index`
  started at, since resolution never grows the trail), abandons its in-progress resolution
  chain via a new `SatProofManager::abortResChain()` (`d_resLinks.clear(); d_redundantLits.clear();`
  -- without this the *next* `startResChain()` call would silently append onto the abandoned
  chain's leftover links), and returns `-1` as a sentinel.
- All three call sites in `search()` that read `analyze()`'s return value now check for `-1`
  first: on abort, they call `cancelUntil(0)` themselves (context now genuinely at its floor,
  since nothing in between touched the trail) and `continue` the outer loop, without reading
  `out_learnt`/`out_btlevel`.
- `cancelUntil()`'s escalation scan was removed entirely -- it no longer widens its own target
  under any circumstance; the comment there now states the contract explicitly.

Known residual gap: this only handles markers discovered during (or already pending before)
an `analyze()` call. A marker set during ordinary BCP/`updateLemmas()` propagation that never
leads to (or coincides with) any conflict analysis in the current `check-sat` call is never
drained -- it just sits pending, which `d_numPendingLevelZeroMLIs`/SATSentinel would still
correctly flag as a live "Strong Watched Literals" violation in the meantime. Given how
frequently conflicts (and hence `analyze()` calls) occur in practice this is expected to be
rare/transient, but it is not proven eliminated.

**Result**: `rev.smt2` with `--sat-sentinel "--check-only" --check-proofs --proof-check=lazy`
now runs 8/8 with **zero** SATSentinel invariant violations and no hang -- the original bug
class (SAT-level MLI correctness under CB) appears structurally resolved. Two further,
apparently separate issues surfaced by continued testing, not yet investigated in depth:

1. With `--check-proofs`: fails during final proof checking with "Generated a proof that is
   not closed by the scope" (a free/dangling assumption). Suspected cause: `abortResChain()`
   discards a resolution chain that something else (e.g. an assumption registered via
   `registerSatLitAssumption`/`registerSatAssumptions`) still expects to be completed/consumed.
2. *Without* `--check-proofs`: crashes with `RegionAllocator::operator[]` `Assert(r < sz)`,
   from `analyze()` being called with `confl == CRef_Lazy` directly (`ca[CRef_Lazy]` is an
   invalid dereference). Traced to `updateLemmas()`'s empty-lemma path (`conflict = CRef_Lazy`,
   guarded by `Assert(!produceUnsatCores && !needProof())` -- consistent with reproducing only
   without proofs) reaching `search()`'s `conflict_level == 0` fast path, which is supposed to
   intercept any `CRef_Lazy` conflict before `analyze()` ever sees it, but doesn't: under CB,
   `conflict_level = computeClauseLevel(confl)`, and `computeClauseLevel(CRef_Lazy)` returns
   `decisionLevel()` (not 0) -- this is the exact same latent bug identified at the very start
   of this investigation (see "Root-cause trace" above), in a code path none of today's changes
   have touched. Looks pre-existing rather than newly introduced, but not yet confirmed on a
   clean `main`/pre-session checkout.
   With `--check-unsat-cores` specifically: fails differently, with
   `SolverEngine::checkUnsatCore(): produced core was satisfiable` -- a genuine soundness-
   flavored symptom (the extracted core doesn't actually justify UNSAT) rather than a crash,
   not yet root-caused; may or may not share a cause with (1) or (2).

### Robin: delay attachment of the lazy clause until after conflict analysis is done

Traced the proof-not-closed failure (issue 1 above) precisely first: `reason()`'s call to
`d_pfManager->notifyCurrPropagationInsertedAtLevel()` ([Solver.cc:469-475](Solver.cc#L469))
registers a promise -- "this fact's proof will be preserved across a future pop, as an
assumption the enclosing scope at that level must discharge" -- *unconditionally*, before
`attachClause(real_reason)` (later in the same function) has even revealed that this reason
clause is itself a level-0 MLI. Every abort actually observed was self-triggered exactly this
way: `analyze()`'s abort discards its own resolution state via `abortResChain()`, but does
nothing to unwind `reason()`'s already-made promise, leaving it dangling when the proof is
later scoped.

Fix: added `d_pendingAttach` (`vec<CRef>`), a queue of reason clauses `reason()` has allocated
but not yet attached. `reason()` now pushes onto it instead of calling `attachClause()`
directly (the sentinel's `update_reason` notification moves with it, into the flush, since
notifying the sentinel about a reason before it even knows the clause exists via `add_clause`
would be inconsistent). `reason()` is also called from `SatProofManager::processRedundantLit`
(via `endResChain()`, called by `search()` *after* `analyze()` returns) and from
`litRedundant()` (clause minimization, inside `analyze()`), so the flush -- a new
`flushPendingAttach()` -- happens once per `search()` loop iteration, at the top, covering the
whole conflict-handling episode rather than just `analyze()` itself. `analyzeFinal()` also
calls `reason()` and has its own early return, so it needed its own explicit flush too.

Needed one more guard once implemented: a queued clause's asserting literal can be unassigned
(or reassigned via a different reason) by an intervening backtrack before the flush runs --
observed via a SATSentinel `update_reason` rejection (`!state->lit_undef(lit)`). Fixed by only
emitting the `update_reason` notification if `value(ca[cr][0]) == l_True` and
`vardata[var(ca[cr][0])].d_reason == cr` still hold at flush time; `attachClause()` itself is
unconditional (it's a no-op safe on a clause whose asserting literal is no longer satisfied --
`detectMissedLowerImplication` just finds no satIdx and returns).

**Result so far**: the proof-not-closed-by-scope failure is gone. But deferring attachment
also defers *detection* -- since it no longer happens inside the `analyze()` call that fetched
the clause, that same `analyze()` call can no longer self-abort on it (intended), but nothing
else picks it up either until some *later*, unrelated `analyze()` call happens to check
`d_numPendingLevelZeroMLIs`, or never does. This reopened the "stale level sits pending,
checker complains" gap from the earlier deferred-queue design. Closed by checking
`d_numPendingLevelZeroMLIs > 0` immediately after `flushPendingAttach()`, at the top of
`search()`'s loop (nothing in-flight there, so `cancelUntil(0)` is safe unconditionally) and
resolving it right then rather than waiting.

That reintroduced a *different* problem: with `--check-proofs`, this now hangs (confirmed via
`-t minisat` trace: 0 conflicts logged, 388 `cancelUntil` calls, the same literals'
"Theory propagated" messages recurring) -- the same livelock signature from much earlier in
this investigation, apparently reintroduced by making `cancelUntil(0)` fire far more eagerly
and frequently than before. Not yet root-caused; last state committed to the tree, not
reverted, pending Robin's input on whether to keep pulling this thread or step back to a
simpler design.

Separately, `--check-unsat-cores` now surfaces a violation that turns out to be the
**already-documented, not-yet-fixed nonzero-level case**: `c1 = v10, δ=4` vs `c2 = ~v68, δ=9`
-- not level 0 -- exactly the gap flagged in point 6's fix ("TODO(minisat-rscb): a nonzero-
level reimplication here can still register its theory-side support at a context level above
its own ... not yet addressed for the nonzero case"). Confirms that gap is real and reachable,
not just theoretical, but is a pre-existing, separately-scoped problem rather than something
today's changes newly broke.

### Status / open items for Robin

- Core level-0 MLI correctness (the original bug class): appears resolved when analyze() is
  allowed to abort-and-retry (see the escalation/abort work above) *and* attachment is
  deferred past the fetching episode (this section) -- confirmed via the `rev.smt2` sentinel
  repro across all of no-proofs/proofs/unsat-cores at various points, though not all
  simultaneously with the current HEAD state.
- Regression: `--check-proofs` now hangs (livelock) with the latest (eager post-flush
  `cancelUntil(0)`) change. Not yet diagnosed.
- Known, unaddressed, separately-scoped gap: nonzero-level λ reimplications share the same
  context-level-mismatch risk as the level-0 case did, with no fix yet.
- Still open, unrelated to lazy reasons specifically: `computeClauseLevel(CRef_Lazy)` returning
  `decisionLevel()` instead of 0 lets a `CRef_Lazy` conflict from `updateLemmas()`'s
  empty-lemma path slip past `search()`'s `conflict_level == 0` fast path into `analyze()`,
  which then dereferences `ca[CRef_Lazy]` and crashes. Looks pre-existing (untouched code path,
  guarded by `!produceUnsatCores && !needProof()`), not confirmed against a clean checkout.
