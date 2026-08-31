# MLI (missed lower implication) work — compact status

Full verbose narrative/evidence log: [mli-notes.md](mli-notes.md) — covers the now-abandoned
"root-level avoidance" rabbit hole in detail; kept for historical record only, largely superseded
by this file as of 2026-08-24. Paper: Coutelier/Fleury/Kovács, "Lazy Reimplication in
Chronological Backtracking", SAT 2024.

## 2026-08-24: simplified back down after a rabbit hole

Robin's assessment: the whole `d_pendingAttach`/`d_numPendingLevelZeroMLIs`/`analyze()`-abort
apparatus (see mli-notes.md) was solving a problem that never needed solving. It was built to
stop a root-level (level-0) literal from being re-notified to the theories more than once. But in
the pre-MLI implementation, root-level literals were already sometimes notified multiple times,
and that was never an issue — theories tolerate redundant reassertion fine. So all of that
machinery has been removed, and the design is back to its original, much simpler shape. Kept, per
Robin's explicit instruction: `CRef_LazyRoot` itself, as the `d_lazyReason[x]` marker for "this
literal is reimplied at level 0 lazily" (LSCB only — elevate never uses it, see below).

## Core mechanism (current, simplified)

- **λ** = `d_lazyReason` (`vec<CRef>` parallel to `vardata`, Solver.h). Set by
  `detectMissedLowerImplication()` (called from `attachClause()` and from BCP's
  `propagateBool()`) when a clause's satisfied watched literal sits at a level higher than the
  rest of the clause justifies.
- **Two CB strategies, both driven by `detectMissedLowerImplication()`**:
  - **elevate**: reimplies eagerly, in place, synchronously — `handleMissedLowerImplication()`
    (called from `attachClause()`, before this clause's watches are published, so it's safe to
    touch `c[]`/watches) either re-watches a later-trail-position literal, or fixes the
    level/reason directly. Needed because a lazily-assigned literal's justification must respect
    topological order (an invariant the checker verifies) — elevate can't defer this without
    violating it. Applies at every level, including 0 (there's no special root-level case in the
    elevate path — level 0 is just a smaller number `handleMissedLowerImplication` is not called
    for in the first place, see below).
  - **LSCB (default, chronologicalBacktracking on + elevate off)**: does *not* reimply eagerly.
    Just records `d_lazyReason[x] = cr` (or `CRef_LazyRoot` if `impliedLevel == 0` — see below)
    and lets `cancelUntil()` reimply it later, whenever backtracking naturally unassigns x.
- **`CRef_LazyRoot`** (SolverTypes.h, next to `CRef_Undef`/`CRef_Lazy`): the `d_lazyReason[x]`
  marker for "known reimpliable at level 0, no clause reference kept" — set by
  `recordLevelZeroMissedLowerImplication()`. `detectMissedLowerImplication()` intercepts
  `impliedLevel == 0` *before* the elevate/LSCB branch, uniformly for both strategies (matching
  the "eagerly fixing this in place turned out unsound" reasoning still documented on
  `recordLevelZeroMissedLowerImplication()`'s comment: a level-0 target says nothing about a
  literal's trail *position*, which under CB can differ from level order, so an immediate in-place
  fix could violate topological order). `CRef_LazyRoot` only ever lives in `d_lazyReason[]` — it
  is converted to `CRef_Undef` (the existing "unconditional root fact" convention, e.g.
  `updateLemmas()`'s `lemma_ref == CRef_Undef` case) the moment `cancelUntil()` actually reimplies
  it via `uncheckedEnqueue`. Nothing outside `d_lazyReason`/`cancelUntil`/`detectMissedLowerImplication`
  needs to know `CRef_LazyRoot` exists.
- **`cancelUntil(level)`**: unassign loop reimplies λ entries into `toRequeue` whenever
  `impliedLevel <= level`, *regardless of whether `level` is 0* — no eager-cancel-to-0, no
  counting, no analyze() abort. This can reimply (and re-notify the theory of) a literal at a
  level below the current backtrack target if `level > 0` but `impliedLevel == 0`; per Robin this
  is fine, matching the historical pre-MLI behavior. `cancelUntil` itself never widens its own
  target level — every caller relies on backtracking to *exactly* the requested level.
- BCP's blocker optimization can hide an MLI — both the blocker-skip and the first-is-satisfied
  fast paths in `propagateBool()` are level-gated to still call `detectMissedLowerImplication()`.
- `analyze()`'s retry loop for "UIP is itself an MLI" (paper's Algorithm 4): if the UIP literal
  `analyze()` picked is itself an MLI, `cancelUntil(backtrack_level)` can leave `learnt_clause[0]`
  still `l_False` (conflicting) after backtracking — attach it and analyze again. `analyze()` now
  always returns a plain `int` (`>= 0`); it no longer has an abort/`-1` sentinel.
- `reason()` calls `attachClause()` synchronously again (no more `d_pendingAttach` deferral) —
  this was the original shape; the deferral was invented specifically to protect the now-removed
  `analyze()` abort path and isn't needed for anything else.

## Removed 2026-08-24 (were the rabbit hole; do not reintroduce without a concrete new reason)

- `d_numPendingLevelZeroMLIs` counter and everything that read it (`analyze()`'s abort-on-pending
  check, `search()`'s eager loop-top `cancelUntil(0)`).
- `d_pendingAttach` / `flushPendingAttach()` and every call site (search() loop top, before
  `reduceDB()`, inside `simplify()`, `analyzeFinal()`'s early return, `solve_()`'s search loop) —
  along with the guards in `reduceDB()`/`simplify()`/`solve_()` that existed purely to protect
  this deferred-attachment mechanism from GC/incremental-pop corruption. Verified:
  `issue4367.smt2`, the incremental-mode crash that motivated the `solve_()` guard, still passes
  cleanly with the simpler synchronous `reason()` — the guard's problem doesn't reproduce here.
- `SatProofManager::abortResChain()` (only caller was `analyze()`'s abort path).
- `--reorder-repropagation` option and `cancelUntil()`'s `toRequeue` stable-sort — built while
  chasing the (now understood to be non-existent) "theory context mismatch" concern; never
  verified to fix anything on its own.

## Confirmed fixed (still holds after the simplification)

1. GC relocation root missing for `d_lazyReason` (`relocAll()`).
2. MLI detection needs an all-other-literals-false shape guard, not just "c[0]/c[1] satisfied".
3. BCP's blocker optimization can hide an MLI (see above).
4. Stale λ entries — `isValidLazyReason()` guards every read of an *existing* `d_lazyReason`
   entry.
5. `analyze()` retry loop for "UIP is itself an MLI" (see above).
6. **2026-08-26: sentinel `add_clause` rejected on reattach.** `SimpSolver::strengthenClause()`
   (`SimpSolver.cc:226-255`) detaches a clause, shrinks it, and calls `attachClause()` again on the
   *same* `CRef` without ever telling the sentinel the clause was deleted — deliberate, so the
   clause keeps the identifier it already has (the shrink itself is reported separately via
   `NOTIFY(shrink_clause, ...)`). But `attachClause()` (`Solver.cc:838`) used to call
   `NOTIFY(add_clause, ...)` unconditionally on every call, violating `add_clause`'s documented
   precondition (`Sentinel-API.hpp`: `@pre cl ∉ F`) on the second call — the sentinel library
   flags this internally as "clause already active" and the `NOTIFY` macro's `Assert(false)`
   turns that into a crash: `Fatal failure ... attachClause ... Check failure` /
   `SATSentinel rejected the add_clause notification`. Not an MLI/CB bug — reproducible even
   without chronological backtracking, purely from `--sat-sentinel` plus MiniSat's ordinary
   variable-elimination preprocessing (`--minisat-simplification` default `ALL`, on whenever no
   proofs/unsat-cores/incremental). Repro (any of these, no other flags needed):
   `./bin/cvc5 --sat-sentinel "--check-only" regress0/uf/cnf-iff.smt2` (also `uf/cnf_abc.smt2`,
   `bug576a.smt2`). **Fixed** in two steps, per Robin's call to enforce the invariant explicitly
   rather than infer it: first by gating the `add_clause` notification on
   `!d_sentinelState.knowsClause(cr)` in `attachClause()` (self-contained in `Solver.cc`); then
   hardened by giving `attachClause()` an explicit `bool reattach = false` parameter (`Solver.h`)
   that every call site states outright — only `strengthenClause()`'s reattach passes `true` — and
   replacing the silent `knowsClause()` branch with `Assert(d_sentinelState.knowsClause(cr) ==
   reattach)`, so a caller's belief about whether cr is fresh is checked against the sentinel's
   own bookkeeping rather than trusted, catching a future CRef-reuse bug (e.g. a stale identifier
   silently absorbing a genuinely different clause's content) immediately instead of masking it.
   Confirmed via a 400-file random sample of `regress0` (`--sat-sentinel "--check-only"`, no
   proofs) at each step: 8/400 files hit this exact crash before any fix; 0/400 after either fix,
   identical failure set both times, and the `reattach`-vs-`knowsClause()` assert never fires —
   the invariant holds everywhere in the sample.

   **Self-inflicted regression, caught and fixed same session**: the new
   `Assert(!d_sentinelState || d_sentinelState.knowsClause(cr) == reattach)` above initially
   omitted the `!d_sentinelState ||` runtime guard, checking only `#ifdef CVC5_USE_SATSENTINEL`
   (compile-time). Since the fix was developed by reconfiguring `build/` with
   `-DUSE_SATSENTINEL=ON` (needed to exercise the sentinel code at all) and that flag was left on,
   every subsequent plain `ctest` run (no `--sat-sentinel`) started tripping this assert on every
   `strengthenClause()` reattach — `knowsClause()` always reads `false` when no sentinel is
   attached at runtime, so it never matched `reattach=true`. Caught immediately from a full
   `ctest` run showing ~26+ new failures across totally unrelated theories; root-caused by
   reproducing one (`regress0/uf/cnf-iff.smt2`) with a plain, flag-less `cvc5` invocation.
7. **2026-08-26: `attachClause()`'s Strong Watched Literals asserts didn't account for
   "already satisfied via the other watch."** This turned out to be the root cause of the
   session-opening bug report (`regress1/proofs/macro-res-exp-crowding-lit-inside-unit.smt2`),
   and is unrelated to CB/MLI/reattach-notification entirely — a plain gap in the general
   watched-literal sanity check added in `9e5415f70` (`Solver.cc:879-892`, pre-dates `main`).
   Root-caused via valgrind + `--sat-sentinel "--check-only"`'s trace log: clause 656
   (`~v98 v329 ...`) has `~v98` (`c[0]`) assigned `ROOT`/`PROPAGATE`d — genuinely, permanently
   satisfied — while `v329` (`c[1]`) is a *separate*, independently-settled `l_False` literal,
   also long since `PROPAGATE`d. `SimpSolver`'s `remove_satisfied` is deliberately `false` for the
   whole duration of active elimination (`SimpSolver.cc:80`, flipped to `true` only once
   elimination fully turns off, `SimpSolver.cc:726`, consumed by `Solver::simplify()` at
   `Solver.cc:1885`) — this is vanilla MiniSat behavior, not a bug: a clause satisfied by a level-0
   fact is expected to sit around dormant, unpurged, mid-elimination. When
   `backwardSubsumptionCheck()` later strengthens 656 (removing an unrelated literal, `v340`) and
   reattaches it, the `c[1]`-side assert only ever consulted `c[1]`'s own state
   (`value(c[1]) != l_False || trail_index(...) >= qhead || ...`) — it never checked whether `c[0]`
   already satisfies the clause, in which case `c[1]`'s staleness is moot (the clause is inert and
   will never contribute anything again regardless). Symmetric gap on the `c[0]`-side block.
   **Fixed** by adding `value(c[1]) == l_True ||` / `value(c[0]) == l_True ||` to the respective
   blocks. Confirmed: the original repro now correctly outputs `unsat` with no crash (both
   plain and `--sat-sentinel "--check-only"`); all 27 tests from the session's `ctest` failure
   report now pass; the 400-file `regress0` sentinel sample's `push-pop/boolean/fuzz_48.smt2`
   crash (previously catalogued below as a separate "Strong Watched Literals" repro) is also
   gone, down to just the 3 `uflra/pb_real_10_0200_10_{22,25,29}` crashes (a different bug, see
   below).

## 2026-08-27: fixed a second proof-path regression from the rewrite (missing storeUnitConflict)

Repro: `valgrind bin/cvc5 --sat-sentinel "--check-only" --check-proofs --proof-check=lazy
regress0/arrays/dd_ios_t1_norm_op.smt2` crashed with `Assert(d_conflictLit != undefSatLiteral)`
inside `SatProofManager::finalizeProof()` (no-arg overload, sat_proof_manager.cpp:751).

Root cause: `updateLemmas()`'s second loop (Solver.cc, the `if (value(lemma[0]) == l_False)`
branch around line 2645) sets `conflict = lemma_ref` whenever a lemma is found already falsified.
For a **unit** lemma (`lemma.size() == 1`), no clause is ever allocated for it (the `lemma.size() >
1` block that calls `ca.alloc()`/`attachClause()` is skipped), so `lemma_ref` is left at its
initialized sentinel value `CRef_Lazy`. That `CRef_Lazy` propagates up through `search()` as
`confl`, and when `repairConflict()` later sees `conflict_level == 0` and `confl == CRef_Lazy`, it
calls the no-arg `finalizeProof()`, which expects `SatProofManager::storeUnitConflict()` to have
already recorded which literal (`d_conflictLit`) the lazy conflict is about. Pre-rewrite (main and
this branch before `2b27a2e194`), that exact spot called `d_pfManager->storeUnitConflict(lemma[0])`
right when `conflict = CRef_Lazy` was set — the "major rewrite" commit dropped that call while
keeping everything downstream that depends on it.

Distinct from the two other `Assert(!needProof())`-guarded early-return unit-conflict paths in
`updateLemmas()` (the empty-lemma case at ~line 2535 and the CB-specific level-0 case at ~line
2554) — those are deliberate, documented "no proof support yet" limitations (comment explains no
real reason/clause exists to build a proof from) and were not touched. The bug here is the general,
unguarded path that silently dropped proof support for *any* level unit-lemma conflict, not just
level 0.

**Fixed** (Solver.cc, in the `if (value(lemma[0]) == l_False)` block): call
`d_pfManager->storeUnitConflict(lemma[0])` when `lemma.size() == 1 && needProof()`. Verified: the
valgrind repro now returns `unsat` cleanly (both plain and under valgrind); the two proof repros
fixed earlier this session (`arith-mixed-types-no-tighten.smt2`, `bv/core/bitvec3.smtv1.smt2`)
still pass; a 40-file sweep of `regress0/arrays/*.smt2` with `--sat-sentinel "--check-only"
--check-proofs --proof-check=lazy` shows no new crashes (remaining "FAIL" hits were pre-existing
benign config-error messages, e.g. unsupported `STORE_ALL`/Ackermannization, not crashes).

## Currently open (unaffected by today's simplification — separate issues)

- **`ProofNodeManager::mkScope` "proof not closed by scope" (free assumption escaping) — TWO
  DISTINCT BUGS FOUND 2026-08-25, one fixed, the operative one for the known repro still open.**
  Hits ~24+ regression tests across many theories (arrays, bv, nl, decision, fmf, ho, parser,
  proofs) whenever `--check-proofs --proof-check=lazy` is combined with an MLI reimplication.
  Repro: `./bin/cvc5 --check-proofs --proof-check=lazy regress0/bv/core/bitvec3.smtv1.smt2` (also
  `regress0/arrays/incorrect8.smtv1.smt2`). Confirmed via `--sat-sentinel "--check-only"` that this
  is *not* a SAT-level invariant violation — the trace/proof-DAG is fine at the SAT level; the bug
  is purely in how a reimplied literal's proof gets threaded.
  - **Bug A — reason-clause pivot not normalized (real bug, FIXED, but not the cause of the known
    repro).** `detectMissedLowerImplication()` (Solver.cc:719) computes `satIdx` — the index (0 or
    1) of the satisfied watched literal `x` within `cr` — but used to stash `cr` into
    `d_lazyReason[x]`/`vardata[x].d_reason` verbatim, without normalizing so that `x` sits at
    `ca[cr][0]`. Every other reason-installation site in the file (`reason()`'s built explanation,
    BCP's `c[0]`, learnt clauses, `updateLemmas()`) maintains "reason clause's literal 0 is the
    literal being justified" — load-bearing for `analyze()`'s resolution loop (Solver.cc:~1240:
    `for (int j = (p == lit_Undef) ? 0 : 1, ...)`, which assumes `ca[confl][0] == p`). If
    `satIdx == 1`, `ca[cr][0]` would be a different, genuinely-falsified literal that the loop
    silently skips, never resolving it out or reaching `addResolutionStep`, so its own upstream
    `ASSUME` escapes the scope. **Fixed** by normalizing in `detectMissedLowerImplication()` itself
    right after `satIdx`/`impliedLevel` are computed (swap `c[0]`/`c[1]` when `satIdx == 1`) — safe
    because the only call site that can ever see `satIdx == 1` is via `attachClause()` (before this
    clause's watches are published), and once `x` is on the trail its literal can't be falsified
    out from under slot 0 until `x` itself is unassigned. **However**, traced empirically (a
    temporary `Trace` counting `satIdx == 1` occurrences) and confirmed `satIdx` is *always already
    0* on `bitvec3.smtv1.smt2` — every λ clause here comes from `reason()`'s on-demand explanation
    path, which already places the propagated literal at index 0 by construction
    (`Assert(explanation[0] == l)`, Solver.cc:414). So this fix is a correct hardening (matters for
    clauses that reach `attachClause()` some other way, e.g. plain theory lemmas) but is **not**
    what was breaking the known repro. Kept anyway; harmless and closes a real gap.
  - **Bug B — level-0 MLI's `CRef_Undef` substitution defeats proof explanation. This IS the cause
    of the known repro, confirmed empirically, NOT YET FIXED.** Traced with a temporary
    instrumentation of `SatProofManager::explainLit`'s "no SAT reason" early return
    (sat_proof_manager.cpp:405-409): it fires for exactly the crash's free-assumption literal,
    `~13` / `(not (= #b1 ((_ extract 0 0) c2)))`, which at that point has `value = l_False`,
    `level = 0`, `d_lazyReason = CRef_Undef` — i.e. it went through the level-0 MLI path
    (`recordLevelZeroMissedLowerImplication` → `CRef_LazyRoot` → `cancelUntil()` converts to
    `CRef_Undef` on reimplication, per the documented "unconditional root fact" convention, mirror
    of `updateLemmas()`'s `lemma_ref == CRef_Undef` case).
    - **Mechanism**: `SatProofManager::explainLit()` (sat_proof_manager.cpp:372) has exactly two
      ways to consider a literal justified: (1) it's in `d_assumptions` (a genuine registered
      top-level input/lemma, via `registerSatAssumptions`/`registerSatLitAssumption`), or (2) it
      calls `d_solver->reason(x)` and gets a real clause to recurse into. `CRef_Undef` hits neither
      — case (2) short-circuits at sat_proof_manager.cpp:405-409 ("no SAT reason") and *silently
      returns without adding any resolution step for it*. But a level-0 MLI-reimplied literal is
      **not** actually a genuine axiom — by definition it's a theory propagation that had a real
      justifying clause (that's what triggered the MLI detection in the first place); `CRef_Undef`
      is a fine marker for the *SAT search* (it correctly signals "permanent regardless of future
      backtracking"), but it lies to the *proof* layer, which reads `CRef_Undef` as "self-evident,
      no premises needed." The real justifying clause was deliberately discarded by design
      (`CRef_LazyRoot` "no clause reference kept" — see `recordLevelZeroMissedLowerImplication()`'s
      comment on why an eager in-place fix was unsound for level 0), so by the time
      `explainLit`/`finalizeProof` needs it, it's gone, and the literal's own upstream `ASSUME`
      (from CNF conversion of the original BV atom) is left permanently unresolved — exactly the
      observed "free assumption."
    - **Why registering it in `d_assumptions` instead would be the wrong fix**: it would silence
      the crash but make the proof *unsound* — `d_assumptions` means "genuine input axiom, needs no
      derivation," which is false for a derived fact; that would produce a proof that "checks out"
      structurally while resting on an unproven premise.
    - **Open question needing your call** (this is the same soundness tradeoff the paper's design
      already grappled with for `CRef_LazyRoot`, not something to patch blindly): LSCB's level-0
      recording is deferred exactly like the nonzero-level case (record now, reimply later in
      `cancelUntil()`), so it isn't obviously subject to the same in-place/topological-order hazard
      that ruled out the eager `elevate` fix — worth checking whether LSCB's level-0 case actually
      needs `CRef_LazyRoot` at all, versus just recording the *real* clause in `d_lazyReason[x]` the
      same way nonzero levels do (with the usual `satIdx`-at-0 normalization from Bug A applied) and
      letting `cancelUntil()` install it as a real reason at level 0 uniformly. If there *is* a
      genuine reason level-0 differs (something specific to `cancelUntil(0)`/root-level handling
      not present for nonzero-level backtracks), the fix instead needs a way to keep the justifying
      clause reachable for `explainLit` (e.g. a side-table from `x` to its discarded λ clause, kept
      only for proof purposes, decoupled from the SAT reason field) without resurrecting the
      unsound in-place mutation.
- **`uflra/pb_real_10_0200_10_{22,25,29}.smtv1.smt2`** (3 near-identical uflra pigeonhole-style
  benchmarks; surfaced by fixing item 6, then item 7 cleared a 4th, `push-pop/boolean/fuzz_48.smt2`,
  that was originally caught alongside these) — `SATSentinel rejected the assign notification` /
  `state->lit_false(lit)` in `uncheckedEnqueue` (`Solver.cc:1532`), assigning a literal the
  sentinel already considers false. Plausibly the nonzero-level counterpart of item 6's TODO in
  mli-notes.md ("the general (nonzero-level) λ reimplication ... shares the exact same
  context-level mismatch ... not yet addressed for the nonzero case") — not yet traced to
  confirm.
- **`RegionAllocator::operator[]` OOB (`r < sz`)**: a stale `CRef` surviving across an incremental
  `checkSat()`/`pop()` boundary, in cases that don't involve `d_pendingAttach` at all (e.g.
  `bug486.cvc.smt2` under `--finite-model-find -i`, no `--check-proofs`). Different leak path than
  `issue4367`'s (already-removed) one; not yet root-caused. Repro:
  `./bin/cvc5 --finite-model-find -i regress0/bug486.cvc.smt2`.
- **CB/theory performance blowup**: `regress0/arith/issue9131-inverse-of-int.smt2`
  (`--nl-ext-split-zero`) solves in 0.024s on `main` (pre-CB), 0.116s on this branch with
  `--no-chronological-backtracking`, but times out (>15s, still climbing) with CB on — ~40-60x
  more `NonlinearExtension::checkFullEffort`/`cancelUntil` calls than the entire non-CB solve
  needs, within the first 25s alone. `--elevate` doesn't help either. Likely an inherent
  small-step-backtracking-vs-expensive-full-theory-check interaction rather than an MLI
  correctness bug; not yet root-caused.
- `computeClauseLevel(CRef_Lazy)` returns `decisionLevel()` not 0 — pre-existing, unconfirmed
  against a clean checkout, separate from MLI work.
- Full regression suite has not been run to completion (harness's own "stop time" cuts it off
  around ~1500/4461 tests each attempt) — only ~1/3 covered so far. The three items above are
  what surfaced in that partial run; there may be more.

## 2026-08-27: FIXED a fourth proof-path regression — `updateLemmas()`'s unit-lemma `varTrue`
  padding was never proof-registered (post-rewrite; everything above this section predates commit
  `2b27a2e19` and may not reflect current line numbers/mechanism names)

  Repro: `./bin/cvc5 --check-proofs --proof-check=lazy regress0/bv/fuzz28.smtv1.smt2` (default
  options — CB on, `elevate`/`lscb` both off). Same `ProofNodeManager::mkScope` "not closed by the
  scope" failure as the historical entries above, but a different, freshly-introduced mechanism.
  The escaping free assumption is always shaped `(or <real-atom> (not true))`.

  - `--no-chronological-backtracking` (NCB): clean `unsat`, no crash.
  - `--elevate`: also clean `unsat` on this repro — but that's very likely coincidental (elevate
    takes a different reimplication branch in `reimplyLit()` and so drives a different search
    trajectory that happens not to exercise the same lemma shape here), **not** evidence that
    elevate mode is immune to the same underlying flaw. Not re-tested on other benchmarks.
  - `--lscb` (lazyReimplication): crashes too, but with a *different*, unrelated failure
    (`Check failure: value(c[i]) == l_True`) — a separate bug, not investigated here.

  **Root cause, confirmed via gdb** (breakpoint at `Solver.cc:2704`, the `attachClause(lemma_ref)`
  call in the block below): `updateLemmas()`'s handling of a lemma that is a genuinely new unit
  propagation (`lemma.size()==1 || value(lemma[1])==l_False`, `value(lemma[0])==l_Undef`,
  Solver.cc:2676-2706) now *eagerly* attaches a real backing clause for size-1 lemmas instead of
  leaving `lemma_ref = CRef_Lazy` (see the comment right above it at Solver.cc:2680-2689 — this is
  deliberate, added to avoid `reason()`'s on-demand `d_proxy->explainPropagation()` returning a
  stale/self-contradictory explanation under CB when called later). Because MiniSat clauses need
  size ≥ 2, it fabricates `paddedClause = [lemma[0], ¬varTrue]` (Solver.cc:2695-2698) and
  `attachClause()`s it — mirroring the *pre-existing* padding trick `reason()` itself uses at
  Solver.cc:472-497 when an on-demand explanation shrinks to one real literal.

  The bug: unlike its two neighboring code paths — the non-unit-lemma clause path right above it
  (Solver.cc:2628-2646, which calls `d_pfManager->notifyClauseInsertedAtLevel(...)`) and `reason()`
  itself (Solver.cc:482-488, `d_pfManager->notifyCurrPropagationInsertedAtLevel(...)`) — this new
  padded-clause allocation calls **no** `d_pfManager` registration function at all. When this CRef
  later serves as a reason clause in conflict `analyze()` (`d_pfManager->addResolutionStep(ca[confl],
  p)`, Solver.cc:1302), `SatProofManager::getClauseNode()` (sat_proof_manager.cpp:64-75) builds its
  Node the ordinary way — `d_cnfStream->getNode(satLit)` for *every* literal, including `¬varTrue`.
  That lookup does succeed (confirmed: `varTrue` is not some unregistered internal-only variable —
  `CnfStream::newLiteral()` deliberately maps the Boolean constant `true` onto
  `d_satSolver->trueVar()`, i.e. `Solver::varTrue` is intentionally *the same* variable the CNF
  stream uses for the node `true`; `cnf_stream.cpp:182-192`), so `getClauseNode` happily returns
  `(or lemma[0]_node (not true))` — but nothing has ever built or registered a *proof* for that
  exact compound Node. The theory lemma's real justification (from whichever theory produced
  `lemma[0]`, delivered earlier via `d_proxy`) is presumably keyed to `lemma[0]`'s own Node (or to
  the lemma's original, un-padded clause shape) — not to this MiniSat-CB-internal
  `(or X, not-true)` artifact, which no other part of the system ever constructs a proof for. The
  result: this Node reaches the final resolution proof as an irreducible leaf with no derivation,
  and (unlike a genuine input assumption) it's also absent from whatever list the top-level scope
  closes over — hence "free assumption."

  - **Why this doesn't look like the historical `CRef_LazyRoot` bug above**: that one was about
    the real justifying clause being *discarded on purpose* (level-0 MLI design tradeoff). This one
    has no such excuse — `lemma[0]`'s justification is sitting right there in the theory engine
    when `updateLemmas()` runs (the comment even says "already fully known here"); the padded
    clause just never gets *told* to point at it.
  - **Not yet clear why `reason()`'s own identical padding trick (Solver.cc:472-497) doesn't hit
    the same bug** (or does, but more rarely) — it's gated the same way
    (`needProof() && explLevel < assertionLevel`, which is also false at level 0 in this
    non-incremental repro, so *neither* site's `notify*` call would fire here even if present).
    Worth checking whether `d_proxy->explainPropagation()` (called just before `reason()`'s pad,
    Solver.cc:403) has some side effect — e.g. via `TheoryProofStepBuffer` — that independently
    registers a proof for the padded-clause-shaped Node, which `updateLemmas()`'s eager path
    skips entirely because it never calls `explainPropagation()` at all (that's the whole point of
    the optimization — avoiding that call). If so, the fix is probably to route
    `updateLemmas()`'s padding through whatever registration `explainPropagation()`/
    `TheoryProofStepBuffer` does for a single-literal explanation, rather than fabricating the
    clause structurally identically but proof-blind. Not attempted — didn't want to guess at the
    right `ProofRule`/registration call without being sure it matches the convention the rest of
    the proof infrastructure expects.
  - **Why just adding `notifyClauseInsertedAtLevel` unconditionally (dropping the
    `clauseLevel < assertionLevel` gate) is *not* the fix**: that function's first act is
    `pf->getProofFor(clauseNode)` followed by `Assert(clauseCnfPf->getRule() != ProofRule::ASSUME)`
    (sat_proof_manager.cpp:891-892) — i.e. it *fetches* an already-existing proof, it doesn't
    manufacture one. Since nothing has registered a proof for this Node anywhere, calling this
    would either return a bare `ASSUME` (tripping that Assert) or hit the same "unknown formula"
    failure mode one step earlier and more loudly.

  **Fix (Robin's suggestion, implemented and verified same day)**: don't try to register a proof
  for the padded compound clause at all — instead make the proof-facing Node representation of
  such a clause *ignore* the padding literal entirely, so it degenerates back to the bare
  `lemma[0]` Node, matching whatever already justifies the real unit fact (the same convention the
  codebase already uses for genuine unit facts elsewhere, e.g. `finalizeProof(Lit, bool)` at
  sat_proof_manager.cpp:760-771, which represents a unit fact as its bare literal Node, never as a
  unary `OR`). This is sound because the padding literal (`¬varTrue` or `varFalse`, both always
  false by construction — Solver.cc initializes them at level 0 as
  `uncheckedEnqueue(mkLit(varTrue, false))` / `uncheckedEnqueue(mkLit(varFalse, true))`) never
  resolves against anything else in the whole SAT proof: no other clause in the problem ever
  mentions `varTrue`/`varFalse` again, since they're solver-internal-only variables never handed
  out to any theory or CNF atom. So it was never contributing to the resolution proof's *content*,
  only to MiniSat's clause-representation mechanics (`Assert(c.size() > 1)` in `attachClause()`)
  — dropping it from the Node is a representation fix, not a soundness-affecting shortcut, and
  applies uniformly to *both* padding call sites (`reimplyLit()`'s `CRef_LazyRoot`/`varFalse` case
  and `updateLemmas()`'s `varTrue` case) even though only the latter was observed to fail so far.

  Implemented in `SatProofManager::getClauseNode(const Minisat::Clause&)`
  (sat_proof_manager.cpp:64-92): skip any literal whose var is `d_solver->trueVar()` with positive
  sign (i.e. the literal `¬varTrue`) or `d_solver->falseVar()` with negative sign (i.e. the literal
  `varFalse`) when building `clauseNodes`; if exactly one real node remains, return it directly
  instead of wrapping in `mkNode(Kind::OR, ...)` (a unary `OR` isn't how the rest of the codebase
  represents unit facts, and may not even be legal `Kind::OR` arity).

  **Verified**: `fuzz28.smtv1.smt2` now returns `unsat` cleanly under
  `--check-proofs --proof-check=lazy` (default CB options). Swept
  `regress0/bv/*.smt2{,v1.smt2}` (274 files) — the 9 remaining failures are all pre-existing/
  unrelated (missing `--incremental`, bv-to-int + higher-order, parser edge cases needing test
  metadata flags this ad hoc sweep didn't apply) — zero proof-scope failures. Also swept
  `regress0/arith/*.smt2` + `regress0/uf/*.smt2` (116 files) filtering specifically for
  `mkScope`/"not closed by the scope" failures: zero. The two open questions above (whether
  `reason()`'s identical padding trick was independently safe, and whether `lemma[0]`'s own
  justification is reachable without `explainPropagation()`) are moot for this fix, since it no
  longer matters *how* `lemma[0]` was justified — the padded clause's Node is now literally
  `lemma[0]`'s Node, so it inherits whatever already proves that literal, same as it would via any
  other route to the same Node.

## Key files

- `src/prop/minisat/core/Solver.h` / `.cc` — everything above.
- `src/prop/minisat/core/SolverTypes.h` — `CRef_LazyRoot`.
- `src/prop/minisat/sat_proof_manager.h` / `.cpp` — proof resolution-chain plumbing (no MLI-specific additions remain here).
