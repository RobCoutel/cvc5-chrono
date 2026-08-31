/**
 * @file SentinelWrapper.h
 * @author Robin Coutelier
 * @brief This file is part of the MiniSat solver. It implements functions to convert between the
 * types used in MiniSat and the types used in the Sentinel API, the NOTIFY macro the solver
 * announces its steps with, and the bookkeeping that backs it.
 *
 * Everything here compiles away unless cvc5 was configured with --sat-sentinel; only then are the
 * SATSentinel headers pulled in.
 */

#pragma once

#include "cvc5_private.h"

#ifdef CVC5_USE_SATSENTINEL

#include "Sentinel-API.hpp"
#include "base/check.h"
#include "base/output.h"
#include "prop/minisat/core/SolverTypes.h"
#include "options/booleans_options.h"
#include "options/base_options.h"
#include "options/main_options.h"
#include "options/option_exception.h"
#include "options/prop_options.h"
#include "options/smt_options.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#endif /* CVC5_USE_SATSENTINEL */

//=================================================================================================
// SATSentinel notifications:
//
// NOTIFY(name, args...) forwards a notification to sentinel::wrapper::name, passing the sentinel
// state of the surrounding object -- which it obtains by calling sentinelState() -- as the first
// argument. It expands to nothing unless cvc5 was configured with --sat-sentinel, and is a no-op
// at runtime unless --sat-sentinel was passed, so it is safe to sprinkle over the solver.
#ifdef CVC5_USE_SATSENTINEL
#define NOTIFY(type, ...)                                                   \
  do                                                                        \
  {                                                                         \
    const sentinel::wrapper::SentinelState& state__ = sentinelState();      \
    if (state__)                                                            \
    {                                                                       \
      if (!sentinel::wrapper::type(state__ __VA_OPT__(, __VA_ARGS__)))      \
      {                                                                     \
        sentinel::wrapper::message(                                         \
            state__,                                                        \
            "An error occurred during application of " + std::string(#type) \
                + " notification");                                         \
        Assert(false) << "SATSentinel rejected the " #type " notification"; \
      }                                                                     \
    }                                                                       \
  } while (0)
#else
#define NOTIFY(type, ...) \
  do                      \
  {                       \
  } while (0)
#endif

// Watch list movements are by far the most frequent notifications, and only the watched literal
// invariants need them. Set this to 0 to compile them out when profiling something else.
#define NOTIFY_WATCH_CHANGES 1
#if NOTIFY_WATCH_CHANGES
#define NOTIFY_WATCH(type, ...) NOTIFY(type, __VA_ARGS__)
#else
#define NOTIFY_WATCH(type, ...) \
  do                            \
  {                             \
  } while (0)
#endif

#ifdef CVC5_USE_SATSENTINEL

namespace sentinel::wrapper
{
  using namespace cvc5::internal::Minisat;

  // MiniSat variables are 0-indexed and use var_Undef == -1 for "no variable", while Sentinel
  // variables are 1-indexed and use 0 for "no variable". Shifting by one maps both the regular
  // variables and the undefined variable (-1 + 1 wraps around to 0).
  inline sentinel::Tvar convert(Var var) {
    return sentinel::Tvar(static_cast<unsigned>(var));
  }
  inline Var convert(sentinel::Tvar var) {
    return static_cast<Var>(var.value);
  }

  // MiniSat encodes a literal as 2 * var + sign, where sign is 1 for a *negative* literal, while
  // Sentinel encodes it as 2 * var + pol, where pol is 1 for a *positive* literal. On top of the
  // shift on the variable, the polarity bit therefore has to be flipped.
  // MiniSat's lit_Undef and lit_Error are both built on var_Undef and would round-trip to a
  // literal over the (nonexistent) Sentinel variable 0, so they are mapped to LIT_UNDEF instead.
  inline sentinel::Tlit convert(Lit lit) {
    if (lit == lit_Undef || lit == lit_Error) {
      return sentinel::LIT_UNDEF;
    }
    return sentinel::Tlit(convert(var(lit)), sign(lit) ? 0 : 1);
  }
  inline Lit convert(sentinel::Tlit lit) {
    if (lit == sentinel::LIT_UNDEF) {
      return lit_Undef;
    }
    return mkLit(convert(lit.var()), !lit.pol());
  }

  // MiniSat decision levels are plain ints (the same type as Var), so they need a differently
  // named conversion to avoid an ambiguous overload with convert(Var). Levels are rooted at 0,
  // matching LEVEL_ROOT, and the level -1 that MiniSat gives to unassigned variables wraps around
  // to LEVEL_UNDEF.
  inline sentinel::Tlevel convertLevel(int level) {
    return sentinel::Tlevel(static_cast<unsigned>(level));
  }
  inline int convertLevel(sentinel::Tlevel level) {
    return static_cast<int>(level.value);
  }

  // Setting the sentinel up happens before there is any state to talk through, so these take the
  // sentinel directly. Everything that reports a step of the solver takes a SentinelState instead,
  // since it may have to translate a clause reference; see below.
  inline sentinel::SATSentinel* create_sentinel(const sentinel::Options& options) {
    return sentinel::create_sentinel(options);
  }

  inline void delete_sentinel(sentinel::SATSentinel* sentinel) {
    sentinel::delete_sentinel(sentinel);
  }

  inline void set_command_parser(sentinel::SATSentinel* sentinel, sentinel::Tparser* parser) {
    sentinel::set_command_parser(sentinel, parser);
  }

  inline void set_variable_detail_callback(sentinel::SATSentinel* sentinel, std::function<std::string(Var)> callback) {
    sentinel::set_variable_detail_callback(sentinel, [callback](sentinel::Tvar var) {
      return callback(convert(var));
    });
  }

  inline void add_invariant(sentinel::SATSentinel* sentinel, std::function<bool(std::string&)> checker, const std::string& name) {
    sentinel::add_invariant(sentinel, checker, name);
  }

  // The checker receives the two watched literals and the blocker of the first watcher.
  inline void add_watch_invariant(sentinel::SATSentinel* sentinel, std::function<bool(Lit, Lit, Lit, std::string&)> checker, const std::string& name) {
    sentinel::add_watch_invariant(sentinel, [checker](sentinel::Tlit c1, sentinel::Tlit c2, sentinel::Tlit blocker, std::string& error) {
      return checker(convert(c1), convert(c2), convert(blocker), error);
    }, name);
  }

  /**
   * Owns the SATSentinel monitoring a solver, together with the bookkeeping the solver needs in
   * order to talk to it.
   *
   * The sentinel indexes its clauses with a dense vector, while a MiniSat CRef is an offset into
   * the clause region, which is both sparse and rewritten by every garbage collection. This class
   * holds the translation between the two -- convert(CRef) below -- so that the solver only ever
   * handles its own clause references and has them turned into sentinel identifiers on the way
   * out.
   *
   * A default constructed state is disabled: it owns no sentinel, and every NOTIFY made on an
   * object holding it is a no-op until create() is called.
   */
  class SentinelState
  {
   public:
    SentinelState() = default;
    ~SentinelState() {
      if (d_sentinel != nullptr) {
        sentinel::wrapper::delete_sentinel(d_sentinel);
        d_sentinel = nullptr;
      }
    }
    SentinelState(const SentinelState&) = delete;
    SentinelState& operator=(const SentinelState&) = delete;

    /** The sentinel, or nullptr when monitoring is disabled. */
    sentinel::SATSentinel* sentinelPtr() const { return d_sentinel; }

    /** Whether the solver holding this state is being monitored. */
    explicit operator bool() const { return d_sentinel != nullptr; }

    /** The NOTIFY macro looks this up in the surrounding object; the state is its own. */
    const SentinelState& sentinelState() const { return *this; }

    /**
     * Creates the sentinel. Must be called at the very beginning of the construction of the
     * solver: notifications emitted before this point would be lost, and the sentinel needs to
     * see the whole history of the solver in order to mirror its state.
     *
     * args is tokenized and parsed by SATSentinel's own option parser (sentinel::Options), exactly
     * as the tokens of a `--sat-sentinel { ... }` group on the cvc5 command line, so e.g. passing
     * "--gui" turns on the graphical frontend.
     *
     * checkTrailMonotonicity and checkImpliedLevels are derived from cvc5 options
     * (chronologicalBacktracking and elevate respectively) by the caller, since SentinelState has
     * no access to the surrounding Env.
     *
     * varDetail and clauseDetail give the sentinel display the solver's view of a variable and of
     * a clause, so that it shows SMT atoms rather than plain indices. clauseDetail is called with
     * the clause reference the solver knows, not with the sentinel identifier.
     */
    void create(const std::string& args,
                bool checkTrailMonotonicity,
                bool checkImpliedLevels,
                std::function<std::string(Var)> varDetail,
                std::function<std::string(CRef)> clauseDetail);

    /**
     * Announces a variable of the solver. Solver::resizeVars() drops variables again on pop and
     * the sentinel has no way of forgetting one, so a variable is only announced the first time
     * its index is used.
     */
    void newVar(Var v);

    /** Whether cr already has an identifier, i.e. has been announced and not deleted since. */
    bool knowsClause(CRef cr) const {
      return d_clauseIds.find(cr) != d_clauseIds.end();
    }

    /**
     * The sentinel identifier of the clause the solver refers to by cr, allocated on first sight
     * and kept until deleteClause() gives it back. Identifiers are handed out densely and reused,
     * which is what the sentinel's clause vector wants and what the sparse, garbage collected
     * CRefs cannot provide.
     *
     * MiniSat has two references that do not denote a clause, and the sentinel has a
     * counterpart for each of them, so they are translated rather than given an identifier:
     *   CRef_Undef -> CLAUSE_UNDEF     no reason at all, i.e. a decision;
     *   CRef_Lazy  -> CLAUSE_LAZY      a theory propagation whose explanation was not asked for.
     * Both are "assigned without an available reason clause", which is what the sentinel needs in
     * order not to look for one. A third case, a literal implied at the root level by a clause
     * cvc5 does not keep around (e.g. a unit clause, which MiniSat never stores), is also reported
     * without a reason clause: it reaches the solver as CRef_Undef like a decision, so it is the
     * assign() notification below -- not this function -- that tells it apart by level and reports
     * it to the sentinel as CLAUSE_ROOT instead.
     */
    sentinel::Tclause convert(CRef cr) const {
      if (cr == CRef_Undef) {
        return sentinel::CLAUSE_UNDEF;
      }
      if (cr == CRef_Lazy) {
        return sentinel::CLAUSE_LAZY;
      }
      auto it = d_clauseIds.find(cr);
      if (it != d_clauseIds.end()) {
        return it->second;
      }
      sentinel::Tclause id = sentinel::Tclause(d_nextClauseId);
      if (d_freeClauseIds.empty()) {
        ++d_nextClauseId;
      } else {
        id = d_freeClauseIds.back();
        d_freeClauseIds.pop_back();
      }
      d_clauseIds.emplace(cr, id);
      return id;
    }

    /**
     * The clause reference behind a sentinel identifier, or CRef_Undef if the sentinel knows a
     * clause the solver does not. Only used to render a clause on the sentinel's side, hence the
     * linear scan.
     */
    CRef convert(sentinel::Tclause id) const {
      if (id == sentinel::CLAUSE_UNDEF) {
        return CRef_Undef;
      }
      if (id == sentinel::CLAUSE_LAZY) {
        return CRef_Lazy;
      }
      if (id == sentinel::CLAUSE_ROOT) {
        // CLAUSE_ROOT has no solver-side counterpart: it is a CRef_Undef assignment that
        // assign() reported as root-implied because it happened at level 0.
        return CRef_Undef;
      }
      auto it = std::find_if(
          d_clauseIds.begin(), d_clauseIds.end(),
          [id](const std::pair<const CRef, sentinel::Tclause>& e) {
            return e.second == id;
          });
      return it == d_clauseIds.end() ? CRef_Undef : it->first;
    }

    /** Announces the deletion of cr and recycles its identifier. */
    void deleteClause(CRef cr);

    /**
     * Rewrites the identifier map after a garbage collection has moved every clause. Solver::
     * relocAll() rewrites the clause lists in place, entry by entry, so pairing each list against
     * the copy taken before relocation gives the new reference of every clause without having to
     * ask the allocator. The identifiers themselves do not change: the sentinel never learns that
     * a garbage collection happened.
     */
    void relocate(const vec<CRef>& oldRemovable,
                  const vec<CRef>& newRemovable,
                  const vec<CRef>& oldPersistent,
                  const vec<CRef>& newPersistent);

   private:
    /** The sentinel this state owns, or nullptr while monitoring is disabled */
    sentinel::SATSentinel* d_sentinel = nullptr;

    /**
     * Maps a live clause reference of the solver onto the identifier the sentinel knows it by.
     * Filled in by convert(CRef), which is called from the const notification path, hence mutable.
     */
    mutable std::unordered_map<CRef, sentinel::Tclause> d_clauseIds;

    /** Identifiers of deleted clauses, reused so that the sentinel's clause vector stays compact */
    mutable std::vector<sentinel::Tclause> d_freeClauseIds;

    /** Next never-used sentinel clause identifier */
    mutable unsigned d_nextClauseId = 0;

    /** Number of variables the sentinel has been told about, see newVar() */
    int d_numVars = 0;
  };

  //===============================================================================================
  // The notifications themselves. Each takes the state of the solver making it, so that a clause
  // reference can be translated into the identifier the sentinel knows.

  inline bool add_variable(const SentinelState& state, Var var) {
    return sentinel::add_variable(state.sentinelPtr(), convert(var));
  }
  inline bool set_variable_alias(const SentinelState& state, Var var, std::string alias) {
    return sentinel::set_variable_alias(state.sentinelPtr(), convert(var), alias);
  }

  // Both vec<Lit> and Clause implicitly convert to a Lit pointer, so a clause held in either can
  // be passed directly, e.g. add_clause(state, cr, ca[cr], ca[cr].size()).
  inline bool add_clause(const SentinelState& state, CRef cl, const Lit* lits, unsigned int size, bool external = false) {
    std::vector<sentinel::Tlit> sentinel_lits;
    sentinel_lits.reserve(size);
    for (unsigned int i = 0; i < size; i++) {
      sentinel_lits.push_back(convert(lits[i]));
    }
    return sentinel::add_clause(
        state.sentinelPtr(), state.convert(cl), sentinel_lits.data(), size, external);
  }
  inline bool delete_clause(const SentinelState& state, CRef clause) {
    return sentinel::delete_clause(state.sentinelPtr(), state.convert(clause));
  }
  inline bool shrink_clause(const SentinelState& state, CRef clause, Lit removed_lit) {
    return sentinel::shrink_clause(
        state.sentinelPtr(), state.convert(clause), convert(removed_lit));
  }

  // A literal assigned with no reason clause is a decision, except at level 0: nothing opens a
  // decision level there, so a CRef_Undef assignment at level 0 is instead a literal implied by a
  // clause cvc5 does not keep around (e.g. a unit clause, which MiniSat never stores), which is
  // reported to the sentinel as CLAUSE_ROOT rather than CLAUSE_UNDEF.
  inline bool assign  (const SentinelState& state, Lit lit, int level, CRef reason = CRef_Undef) {
    sentinel::Tclause justification = (reason == CRef_Undef && level == 0)
                                           ? sentinel::CLAUSE_ROOT
                                           : state.convert(reason);
    return sentinel::assign(state.sentinelPtr(), convert(lit), justification)
        && sentinel::update_level(state.sentinelPtr(), convert(lit), convertLevel(level));
  }
  inline bool unassign(const SentinelState& state, Lit lit) {
    return sentinel::unassign(state.sentinelPtr(), convert(lit));
  }

  inline bool propagate  (const SentinelState& state, Lit lit) {
    return sentinel::propagate(state.sentinelPtr(), convert(lit));
  }
  inline bool unpropagate(const SentinelState& state, Lit lit) {
    return sentinel::unpropagate(state.sentinelPtr(), convert(lit));
  }

  inline bool update_level(const SentinelState& state, Lit lit, int level) {
    return sentinel::update_level(state.sentinelPtr(), convert(lit), convertLevel(level));
  }
  inline bool update_reason(const SentinelState& state, Lit lit, CRef reason) {
    return sentinel::update_reason(state.sentinelPtr(), convert(lit), state.convert(reason));
  }

  inline bool watch(const SentinelState& state, CRef clause, Lit lit) {
    return sentinel::watch(state.sentinelPtr(), state.convert(clause), convert(lit));
  }
  inline bool unwatch(const SentinelState& state, CRef clause, Lit lit) {
    return sentinel::unwatch(state.sentinelPtr(), state.convert(clause), convert(lit));
  }
  inline bool block(const SentinelState& state, CRef clause, Lit blocker, Lit watch = lit_Undef) {
    return sentinel::block(
        state.sentinelPtr(), state.convert(clause), convert(blocker), convert(watch));
  }
  inline bool lock_assumption(const SentinelState& state, Lit lit) {
    return sentinel::lock_assumption(state.sentinelPtr(), convert(lit));
  }
  inline bool unlock_assumption(const SentinelState& state, Lit lit) {
    return sentinel::unlock_assumption(state.sentinelPtr(), convert(lit));
  }

  inline bool check_invariants(const SentinelState& state) {
    return sentinel::check_invariants(state.sentinelPtr());
  }
  inline bool checkpoint(const SentinelState& state) {
    return sentinel::checkpoint(state.sentinelPtr());
  }
  inline bool message(const SentinelState& state, const std::string& msg, unsigned level = 0) {
    return sentinel::message(state.sentinelPtr(), msg, level);
  }

  inline bool save_execution(const SentinelState& state, const std::string& filename) {
    return sentinel::save_execution(state.sentinelPtr(), filename);
  }
  inline bool load_execution(const SentinelState& state, const std::string& filename) {
    return sentinel::load_execution(state.sentinelPtr(), filename);
  }

  //===============================================================================================
  // The members of SentinelState that notify, and therefore need the functions above.

  inline void SentinelState::create(const std::string& args,
                                    bool checkTrailMonotonicity,
                                    bool checkImpliedLevels,
                                    std::function<std::string(Var)> varDetail,
                                    std::function<std::string(CRef)> clauseDetail) {
    Assert(d_sentinel == nullptr) << "the sentinel has already been created";
    std::istringstream argStream(args);
    std::vector<std::string> tokens{
        std::istream_iterator<std::string>(argStream),
        std::istream_iterator<std::string>()};
    sentinel::Options opts(tokens);
    opts.check_no_missed_implications = true;
    opts.check_topological_order = true;
    opts.check_strong_watched_literals = true;
    // opts.check_repetition = true;
    opts.check_trail_monotonicity = checkTrailMonotonicity;
    opts.check_implied_levels = checkImpliedLevels;
    d_sentinel = sentinel::wrapper::create_sentinel(opts);
    sentinel::wrapper::set_variable_detail_callback(d_sentinel, std::move(varDetail));
    // The sentinel identifies a clause by the identifier it was given, so translate back.
    sentinel::set_clause_detail_callback(
        d_sentinel, [this, detail = std::move(clauseDetail)](sentinel::Tclause id) {
          CRef cr = convert(id);
          if (cr == CRef_Undef || cr == CRef_Lazy) {
            return std::string();
          }
          return detail(cr);
        });
  }

  inline void SentinelState::newVar(Var v) {
    if (v >= d_numVars) {
      d_numVars = v + 1;
      NOTIFY(add_variable, v);
    }
  }

  inline void SentinelState::deleteClause(CRef cr) {
    auto it = d_clauseIds.find(cr);
    if (it == d_clauseIds.end()) {
      return;
    }
    // Notified before the entry goes away, so that convert(cr) finds the identifier the sentinel
    // knows rather than handing out a fresh one.
    NOTIFY(delete_clause, cr);
    d_freeClauseIds.push_back(it->second);
    d_clauseIds.erase(it);
  }

  inline void SentinelState::relocate(const vec<CRef>& oldRemovable,
                                      const vec<CRef>& newRemovable,
                                      const vec<CRef>& oldPersistent,
                                      const vec<CRef>& newPersistent) {
    std::unordered_map<CRef, sentinel::Tclause> relocated;
    relocated.reserve(d_clauseIds.size());
    auto carryOver = [&](const vec<CRef>& before, const vec<CRef>& after) {
      Assert(before.size() == after.size());
      for (int i = 0; i < before.size(); ++i) {
        auto it = d_clauseIds.find(before[i]);
        if (it != d_clauseIds.end()) {
          relocated.emplace(after[i], it->second);
          d_clauseIds.erase(it);
        }
      }
    };
    carryOver(oldRemovable, newRemovable);
    carryOver(oldPersistent, newPersistent);

    // A clause the sentinel knows about that is in neither list is unreachable for MiniSat too,
    // so it did not survive the collection. Tell the sentinel it is gone rather than leaving it
    // with a clause that no longer exists.
    for (const auto& [cr, id] : d_clauseIds) {
      Trace("minisat") << "sentinel: clause " << cr
                       << " dropped by garbage collection\n";
      NOTIFY(delete_clause, cr);
      d_freeClauseIds.push_back(id);
    }
    d_clauseIds = std::move(relocated);
  }
}

#endif /* CVC5_USE_SATSENTINEL */
