// Copyright 2010-2012 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <math.h>
#include <string.h>
#include <algorithm>
#include "base/hash.h"
#include <string>
#include <utility>
#include <vector>

#include "base/commandlineflags.h"
#include "base/hash.h"
#include "base/int-type-indexed-vector.h"
#include "base/int-type.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/map-util.h"
#include "base/scoped_ptr.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"
#include "constraint_solver/constraint_solver.h"
#include "constraint_solver/constraint_solveri.h"
#include "util/bitset.h"
#include "util/const_int_array.h"

#include "core/Solver.cc"

namespace operations_research {
class SatPropagator : public Constraint {
 public:
  SatPropagator(Solver* const solver)
  : Constraint(solver),
    num_bound_literals_(0) {}

  ~SatPropagator() {}

  bool Check(IntExpr* const expr) const {
    IntVar* expr_var = NULL;
    bool expr_negated = false;
    return solver()->IsBooleanVar(expr, &expr_var, &expr_negated);
  }

  bool Check(const std::vector<IntVar*>& vars) const {
    for (int i = 0; i < vars.size(); ++i) {
      if (!Check(vars[i])) {
        return false;
      }
    }
    return true;
  }

  Minisat::Lit Literal(IntExpr* const expr) {
    IntVar* expr_var = NULL;
    bool expr_negated = false;
    if (!solver()->IsBooleanVar(expr, &expr_var, &expr_negated)) {
      return Minisat::lit_Error;
    }
    VLOG(1) << "SAT: Parse " << expr->DebugString() << " to "
            << expr_var->DebugString() << "/" << expr_negated;
    if (ContainsKey(indices_, expr_var)) {
      return Minisat::mkLit(indices_[expr_var], expr_negated);
    } else {
      const Minisat::Var var = minisat_.newVar(true, true);
      vars_.push_back(expr_var);
      indices_[expr_var] = var;
      Minisat::Lit lit = Minisat::mkLit(var, expr_negated);
      VLOG(1) << "  - created var = " << Minisat::toInt(var)
              << ", lit = " << Minisat::toInt(lit);
      return lit;
    }
  }

  void VariableBound(int index) {
    if (num_bound_literals_.Value() <= minisat_.decisionLevel()) {
      minisat_.cancelUntil(num_bound_literals_.Value());
    }
    Minisat::Var var(index);
    Minisat::lbool internal_value = minisat_.value(var);
    int64 var_value = vars_[index]->Value();
    if (toInt(internal_value) != 2) {  // not undefined.
      const bool b_value = (toInt(internal_value) == 1);  // == l_True
      if (var_value != b_value) {
        solver()->Fail();
      } else {
        return;
      }
    }
    Minisat::Lit lit = Minisat::mkLit(var, var_value);
    VLOG(1) << "Assign " << vars_[index]->DebugString()
            << ", enqueue lit = " << Minisat::toInt(lit);
    if (!minisat_.propagateOneLiteral(lit)) {
      VLOG(1) << "  - failure detected";
      solver()->Fail();
    } else {
      num_bound_literals_.Incr(solver());
      for (int i = 0; i < minisat_.touched_variables_.size(); ++i) {
        const int var = minisat_.touched_variables_[i];
        Minisat::lbool assigned_value = minisat_.value(var);
        CHECK_NE(2, toInt(assigned_value));
        const bool assigned_bool = (toInt(assigned_value) == 1);  // == l_True
        VLOG(1) << "  - var " << var << " was assigned to " << assigned_bool;
        vars_[var]->SetValue(assigned_bool);
      }
    }
  }

  virtual void Post() {
    for (int i = 0; i < vars_.size(); ++i) {
      Demon* const d = MakeConstraintDemon1(solver(),
                                            this,
                                            &SatPropagator::VariableBound,
                                            "VariableBound",
                                            indices_[vars_[i]]);
      vars_[i]->WhenDomain(d);
    }
  }

  virtual void InitialPropagate() {
    minisat_.initPropagator();
    for (int i = 0; i < vars_.size(); ++i) {
      IntVar* const var = vars_[i];
      if (var->Bound()) {
        VariableBound(i);
      }
    }
  }

  // Add a clause to the solver.
  bool AddClause (const std::vector<Minisat::Lit>& lits) {
    return minisat_.addClause(lits);
  }

  // Add the empty clause, making the solver contradictory.
  bool AddEmptyClause() {
    return minisat_.addEmptyClause();
  }

  // Add a unit clause to the solver.
  bool AddClause (Minisat::Lit p) {
    return minisat_.addClause(p);
  }

  // Add a binary clause to the solver.
  bool AddClause (Minisat::Lit p, Minisat:: Lit q) {
    return minisat_.addClause(p, q);
  }

  // Add a ternary clause to the solver.
  bool AddClause (Minisat::Lit p, Minisat::Lit q, Minisat::Lit r) {
    return minisat_.addClause(p, q, r);
  }

 private:
  Minisat::Solver minisat_;
  std::vector<IntVar*> vars_;
  hash_map<IntVar*, Minisat::Var> indices_;
  std::vector<Minisat::Lit> bound_literals_;
  NumericalRev<int> num_bound_literals_;
};

bool AddBoolEq(SatPropagator* const sat,
               IntExpr* const left,
               IntExpr* const right) {
  if (!sat->Check(left) || !sat->Check(right)) {
    return false;
  }
  Minisat::Lit left_lit = sat->Literal(left);
  Minisat::Lit right_lit = sat->Literal(right);
  sat->AddClause(~left_lit, right_lit);
  sat->AddClause(left_lit, ~right_lit);
  return true;
}

bool AddBoolLe(SatPropagator* const sat,
               IntExpr* const left,
               IntExpr* const right) {
  if (!sat->Check(left) || !sat->Check(right)) {
    return false;
  }
  Minisat::Lit left_lit = sat->Literal(left);
  Minisat::Lit right_lit = sat->Literal(right);
  sat->AddClause(~left_lit, right_lit);
  return true;
}

bool AddBoolNot(SatPropagator* const sat,
                IntExpr* const left,
                IntExpr* const right) {
  if (!sat->Check(left) || !sat->Check(right)) {
    return false;
  }
  Minisat::Lit left_lit = sat->Literal(left);
  Minisat::Lit right_lit = sat->Literal(right);
  sat->AddClause(~left_lit, ~right_lit);
  sat->AddClause(left_lit, right_lit);
  return true;
}

bool AddBoolAndArrayEqVar(SatPropagator* const sat,
                          const std::vector<IntVar*>& vars,
                          IntVar* const target) {
  return false;
  if (!sat->Check(vars) || !sat->Check(target)) {
    return false;
  }
  Minisat::Lit target_lit = sat->Literal(target);
  std::vector<Minisat::Lit> lits(vars.size() + 1);
  for (int i = 0; i < vars.size(); ++i) {
    lits[i] = sat->Literal(vars[i]);
  }
  lits[vars.size()] = ~target_lit;
  sat->AddClause(lits);
  for (int i = 0; i < vars.size(); ++i) {
    sat->AddClause(target_lit, ~lits[i]);
  }
}

bool AddBoolOrArrayEqVar(SatPropagator* const sat,
                         const std::vector<IntVar*>& vars,
                         IntVar* const target) {
  return false;
  if (!sat->Check(vars) || !sat->Check(target)) {
    return false;
  }
  Minisat::Lit target_lit = sat->Literal(target);
  std::vector<Minisat::Lit> lits(vars.size() + 1);
  for (int i = 0; i < vars.size(); ++i) {
    lits[i] = ~sat->Literal(vars[i]);
  }
  lits[vars.size()] = target_lit;
  sat->AddClause(lits);
  for (int i = 0; i < vars.size(); ++i) {
    sat->AddClause(~target_lit, ~lits[i]);
  }
}

bool AddBoolAndEqVar(SatPropagator* const sat,
                     IntVar* const left,
                     IntVar* const right,
                     IntVar* const target) {
  if (!sat->Check(left) || !sat->Check(right) || !sat->Check(target)) {
    return false;
  }
  Minisat::Lit left_lit = sat->Literal(left);
  Minisat::Lit right_lit = sat->Literal(right);
  Minisat::Lit target_lit = sat->Literal(target);
  sat->AddClause(left_lit, right_lit, ~target_lit);
  sat->AddClause(~left_lit, target_lit);
  sat->AddClause(~right_lit, target_lit);
  return true;
}

bool AddBoolOrEqVar(SatPropagator* const sat,
                    IntVar* const left,
                    IntVar* const right,
                    IntVar* const target) {
  if (!sat->Check(left) || !sat->Check(right) || !sat->Check(target)) {
    return false;
  }
  Minisat::Lit left_lit = sat->Literal(left);
  Minisat::Lit right_lit = sat->Literal(right);
  Minisat::Lit target_lit = sat->Literal(target);
  sat->AddClause(~left_lit, ~right_lit, target_lit);
  sat->AddClause(left_lit, ~target_lit);
  sat->AddClause(right_lit, ~target_lit);
  return true;
}

bool AddBoolIsNEqVar(SatPropagator* const sat,
                     IntVar* const left,
                     IntVar* const right,
                     IntVar* const target) {
  if (!sat->Check(left) || !sat->Check(right) || !sat->Check(target)) {
    return false;
  }
  Minisat::Lit left_lit = sat->Literal(left);
  Minisat::Lit right_lit = sat->Literal(right);
  Minisat::Lit target_lit = sat->Literal(target);
  sat->AddClause(~left_lit, right_lit, ~target_lit);
  sat->AddClause(left_lit, ~right_lit, ~target_lit);
  sat->AddClause(left_lit, right_lit, target_lit);
  sat->AddClause(~left_lit, ~right_lit, target_lit);
  return true;
}

bool AddBoolIsEqVar(SatPropagator* const sat,
                    IntVar* const left,
                    IntVar* const right,
                    IntVar* const target) {
  if (!sat->Check(left) || !sat->Check(right) || !sat->Check(target)) {
    return false;
  }
  Minisat::Lit left_lit = sat->Literal(left);
  Minisat::Lit right_lit = sat->Literal(right);
  Minisat::Lit target_lit = sat->Literal(target);
  sat->AddClause(~left_lit, right_lit, target_lit);
  sat->AddClause(left_lit, ~right_lit, target_lit);
  sat->AddClause(left_lit, right_lit, ~target_lit);
  sat->AddClause(~left_lit, ~right_lit, ~target_lit);
  return true;
}

bool AddBoolOrArrayEqualTrue(SatPropagator* const sat,
                             const std::vector<IntVar*>& vars) {
  if (!sat->Check(vars)) {
    return false;
  }
  std::vector<Minisat::Lit> lits(vars.size());
  for (int i = 0; i < vars.size(); ++i) {
    lits[i] = sat->Literal(vars[i]);
  }
  sat->AddClause(lits);
  return false;
}

bool AddBoolAndArrayEqualFalse(SatPropagator* const sat,
                               const std::vector<IntVar*>& vars) {
  if (!sat->Check(vars)) {
    return false;
  }
  std::vector<Minisat::Lit> lits(vars.size());
  for (int i = 0; i < vars.size(); ++i) {
    lits[i] = ~sat->Literal(vars[i]);
  }
  sat->AddClause(lits);
  return false;
}

SatPropagator* MakeSatPropagator(Solver* const solver) {
  return solver->RevAlloc(new SatPropagator(solver));
}
}  // namespace operations_research
