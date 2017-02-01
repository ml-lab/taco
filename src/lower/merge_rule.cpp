#include "merge_rule.h"

#include <map>
#include <set>
#include <vector>
#include <stack>

#include "format.h"
#include "expr.h"
#include "expr_visitor.h"
#include "expr_nodes.h"
#include "lower/iteration_schedule.h"
#include "util/collections.h"

using namespace std;

namespace taco {
namespace lower {

// class MergeRuleNode
MergeRuleNode::~MergeRuleNode() {
}

std::ostream& operator<<(std::ostream& os, const MergeRuleNode& node) {
  class MergeRulePrinter : public MergeRuleVisitor {
  public:
    MergeRulePrinter(std::ostream& os) : os(os) {
    }
    std::ostream& os;
    void visit(const Step* rule) {
      os << rule->step;
    }
    void visit(const And* rule) {
      rule->a.accept(this);
      os << " \u2227 ";
      rule->b.accept(this);
    }
    void visit(const Or* rule) {
      rule->a.accept(this);
      os << " \u2228 ";
      rule->b.accept(this);
    }
  };
  MergeRulePrinter printer(os);
  node.accept(&printer);
  return os;
}


// class MergeRule
MergeRule::MergeRule(const MergeRuleNode* n)
    : util::IntrusivePtr<const MergeRuleNode>(n) {
}

MergeRule::MergeRule() : util::IntrusivePtr<const MergeRuleNode>() {
}

MergeRule MergeRule::make(const Expr& indexExpr, const Var& indexVar,
                          const IterationSchedule& schedule) {
  struct ComputeMergeRule : public internal::ExprVisitor {
    using ExprVisitor::visit;

    ComputeMergeRule(const Var& indexVar, const IterationSchedule& schedule)
        : indexVar(indexVar), schedule(schedule) {
    }

    const Var& indexVar;
    const IterationSchedule& schedule;

    MergeRule mergeRule;
    MergeRule computeMergeRule(const Expr& expr) {
      expr.accept(this);
      MergeRule mr = mergeRule;
      mergeRule = MergeRule();
      return mr;
    }

    void visit(const internal::Read* node) {
      // Throw away expressions `var` does not contribute to
      if (!util::contains(node->indexVars, indexVar)) {
        mergeRule = MergeRule();
        return;
      }

      TensorPath path = schedule.getTensorPath(node);
      size_t i = util::locate(path.getVariables(), indexVar);
      mergeRule = Step::make(path.getStep(i), node);
    }

    void createOrRule(const internal::BinaryExpr* node) {
      MergeRule a = computeMergeRule(node->a);
      MergeRule b = computeMergeRule(node->b);
      if (a.defined() && b.defined()) {
        mergeRule = Or::make(a, b, node);
      }
      // The merge rules are undefined iff one of the operands is a scalar
      else if (a.defined()) {
        mergeRule = a;
      }
      else if (b.defined()) {
        mergeRule = b;
      }
    }

    void createAndRule(const internal::BinaryExpr* node) {
      MergeRule a = computeMergeRule(node->a);
      MergeRule b = computeMergeRule(node->b);
      if (a.defined() && b.defined()) {
        mergeRule = And::make(a, b, node);
      }
      // The merge rules are undefined iff one of the operands is a scalar
      else if (a.defined()) {
        mergeRule = a;
      }
      else if (b.defined()) {
        mergeRule = b;
      }
    }

    void visit(const internal::Add* op) {
      createOrRule(op);
    }

    void visit(const internal::Sub* op) {
      createOrRule(op);
    }

    void visit(const internal::Mul* op) {
      createAndRule(op);
    }

    void visit(const internal::Div* op) {
      createAndRule(op);
    }
  };

  return ComputeMergeRule(indexVar,schedule).computeMergeRule(indexExpr);
}

std::vector<TensorPathStep> MergeRule::getSteps() const {
  struct GetPathsVisitor : public lower::MergeRuleVisitor {
    using MergeRuleVisitor::visit;
    vector<lower::TensorPathStep> steps;
    void visit(const lower::Step* rule) {
      steps.push_back(rule->step);
    }
  };
  GetPathsVisitor getPathsVisitor;
  this->accept(&getPathsVisitor);
  return getPathsVisitor.steps;
}

const Expr& MergeRule::getExpr() const {
  return ptr->expr;
}

void MergeRule::accept(MergeRuleVisitor* v) const {
  ptr->accept(v);
}

std::ostream& operator<<(std::ostream& os, const MergeRule& mergeRule) {
  if (!mergeRule.defined()) return os << "MergeRule()";
  return os << *(mergeRule.ptr);
}


// class Path
MergeRule Step::make(const TensorPathStep& step, Expr expr) {
  auto* node = new Step;
  node->step = step;
  node->expr = expr;
  return node;
}

void Step::accept(MergeRuleVisitor* v) const {
  v->visit(this);
}


// class And
MergeRule And::make(MergeRule a, MergeRule b, Expr expr) {
  auto node = new And;
  node->a = a;
  node->b = b;
  node->expr = expr;
  return node;
}

void And::accept(MergeRuleVisitor* v) const {
  v->visit(this);
}


// class Or
MergeRule Or::make(MergeRule a, MergeRule b, Expr expr) {
  auto* node = new Or;
  node->a = a;
  node->b = b;
  node->expr = expr;
  return node;
}

void Or::accept(MergeRuleVisitor* v) const {
  v->visit(this);
}


// class MergeRuleVisitor
MergeRuleVisitor::~MergeRuleVisitor() {
}

void MergeRuleVisitor::visit(const Step* rule) {
}

void MergeRuleVisitor::visit(const And* rule) {
  rule->a.accept(this);
  rule->b.accept(this);
}

void MergeRuleVisitor::visit(const Or* rule) {
  rule->a.accept(this);
  rule->b.accept(this);
}

MergeRule simplify(const MergeRule& rule) {
  class SimplifyVisitor : public MergeRuleVisitor {
  public:
    set<MergeRule> denseRules;

    MergeRule mergeRule;
    MergeRule simplify(const MergeRule& rule) {
      rule.accept(this);
      MergeRule r = mergeRule;
      mergeRule = MergeRule();
      return r;
    }

    void visit(const Step* rule) {
      Format format = rule->step.getPath().getTensor().getFormat();

      if (format.getLevels()[rule->step.getStep()].getType()==LevelType::Dense){
        denseRules.insert(rule);
      }
      mergeRule = rule;
    }

    void visit(const And* rule) {
      MergeRule a = simplify(rule->a);
      MergeRule b = simplify(rule->b);

      if (util::contains(denseRules, a) && util::contains(denseRules, b)) {
        mergeRule = And::make(a, b, rule->expr);
      }
      else if (util::contains(denseRules, b)) {
        mergeRule = a;
      }
      else {
        mergeRule = b;
      }
    }

    void visit(const Or* rule) {
      // TODO: Handle this case
      mergeRule = rule;
    }
  };
  return SimplifyVisitor().simplify(rule);
}

}}
