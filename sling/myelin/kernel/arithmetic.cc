// Copyright 2017 Google Inc.
//
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

#include "sling/myelin/kernel/arithmetic.h"

#include <math.h>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sling/base/logging.h"
#include "sling/base/types.h"
#include "sling/myelin/compute.h"
#include "sling/myelin/express.h"
#include "sling/myelin/macro-assembler.h"
#include "sling/myelin/generator/elementwise.h"
#include "sling/myelin/generator/expression.h"

#define __ masm->

namespace sling {
namespace myelin {

using namespace jit;

// Generic instruction model for complexity calculation.
static Express::Model generic_model;

// Mapping from flow variables to expression variables.
typedef std::map<Flow::Variable *, Express::Var *> VarMap;

// Convert operation type to expression op.
static Express::OpType OpType(const string &op) {
  // Operations that can be fused into Calculate operations.
  static std::unordered_map<string, Express::OpType> ops {
    {"Add", Express::ADD},
    {"Sub", Express::SUB},
    {"Mul", Express::MUL},
    {"Div", Express::DIV},
    {"RealDiv", Express::DIV},
    {"Minimum", Express::MINIMUM},
    {"Maximum", Express::MAXIMUM},

    {"Log", Express::LOG},
    {"Exp", Express::EXP},
    {"Sigmoid", Express::SIGMOID},
    {"Erf", Express::ERF},

    {"Sin", Express::SIN},
    {"Cos", Express::COS},
    {"Tan", Express::TAN},
    {"Cot", Express::COT},
    {"Sec", Express::SEC},
    {"Csc", Express::CSC},

    {"Asin", Express::ASIN},
    {"Acos", Express::ACOS},
    {"Atan", Express::ATAN},
    {"Acot", Express::ACOT},
    {"Asec", Express::ASEC},
    {"Acsc", Express::ACSC},

    {"Sinh", Express::SINH},
    {"Cosh", Express::COSH},
    {"Tanh", Express::TANH},
    {"Coth", Express::COTH},
    {"Sech", Express::SECH},
    {"Csch", Express::CSCH},

    {"Asinh", Express::ASINH},
    {"Acosh", Express::ACOSH},
    {"Atanh", Express::ATANH},
    {"Acoth", Express::ACOTH},
    {"Asech", Express::ASECH},
    {"Acsch", Express::ACSCH},

    {"Neg", Express::NEG},
    {"Abs", Express::ABS},
    {"Sign", Express::SIGN},
    {"Relu", Express::RELU},
    {"Softsign", Express::SOFTSIGN},
    {"Softplus", Express::SOFTPLUS},
    {"LogSigmoid", Express::LOGSIGMOID},
    {"Reciprocal", Express::RECIPROCAL},
    {"Square", Express::SQUARE},
    {"Sqrt", Express::SQRT},
    {"Rsqrt", Express::RSQRT},

    {"Equal", Express::CMPEQOQ},
    {"NotEqual", Express::CMPNEUQ},
    {"Less", Express::CMPLTOQ},
    {"LessEqual", Express::CMPLEOQ},
    {"Greater", Express::CMPGTOQ},
    {"GreaterEqual", Express::CMPGEOQ},

    {"Cond", Express::COND},
    {"Select", Express::SELECT},

    {"And", Express::AND},
    {"Or", Express::OR},
    {"Xor", Express::XOR},
    {"AndNot", Express::ANDNOT},
    {"Not", Express::NOT},

    {"Sum", Express::SUM},
    {"Product", Express::PRODUCT},
    {"Min", Express::MIN},
    {"Max", Express::MAX},
    {"All", Express::ALL},
    {"Any", Express::ANY},
    {"Count", Express::COUNT},

    {"Identity", Express::MOV},
  };

  auto f = ops.find(op);
  return f == ops.end() ? Express::INVALID : f->second;
}

// Check if operation is a candidate for Calculate ops.
static bool IsCalculateOp(Flow::Operation *op) {
  return op->type == "Calculate" || OpType(op->type) != Express::INVALID;
}

// Check if operation is an assignment op.
static bool IsAssignmentOp(Flow::Operation *op) {
  return op->type == "Assign";
}

// Initialize expression for flow operation.
static void InitExpression(Flow::Operation *op, Express *expr) {
  if (op->type == "Calculate") {
    // Build expression from expression recipe attribute on op.
    const string &recipe = op->GetAttr("expr");
    if (!recipe.empty()) expr->Parse(recipe);
  } else if (op->type == "Assign") {
    const string &recipe = op->GetAttr("expr");
    expr->Parse(recipe.empty() ? "@0=Id(%1)" : recipe);
  } else {
    // Add op with inputs and output.
    CHECK_EQ(op->outdegree(), 1);
    std::vector<Express::Var *> args(op->indegree());
    for (int i = 0; i < op->indegree(); ++i) {
      args[i] = expr->Variable(Express::INPUT, i);
    }
    Express::Op *func = expr->Function(OpType(op->type), args);
    func->Assign(expr->Variable(Express::OUTPUT, 0));
    expr->CompactTempVars();
  }

  // Mark constant and scalar inputs.
  for (int i = 0; i < op->indegree(); ++i) {
    auto *input = op->inputs[i];
    if (input->elements() == 1) {
      int const_id = -1;
      if (input->constant()) {
        if (input->type == DT_FLOAT || input->type == DT_DOUBLE) {
          double value;
          if (input->type == DT_FLOAT) {
             value = *reinterpret_cast<const float *>(input->data);
          } else {
             value = *reinterpret_cast<const double *>(input->data);
          }
          if (value == 0.0) {
            const_id = Express::ZERO;
          } else if (value == 1.0) {
            const_id = Express::ONE;
          } else if (value == 0.5) {
            const_id = Express::HALF;
          } else if (value == 2.0) {
            const_id = Express::TWO;
          } else if (value == -1.0) {
            const_id = Express::N1;
          }
        }
      }
      auto *var = expr->Variable(Express::INPUT, i);
      if (const_id != -1) {
        var->type = Express::NUMBER;
        var->id = const_id;
      } else if (input->constant()) {
        var->type = Express::CONST;
      } else {
        var->single = true;
      }
    }
  }
}

// Initialize expression for step.
void InitExpression(const Step *step, Express *expr) {
  if (step->type() == "Calculate") {
    // Build expression from expression recipe attribute on op.
    const string &recipe = step->GetAttr("expr");
    if (!recipe.empty()) expr->Parse(recipe);
  } else if (step->type() == "Assign") {
    const string &recipe = step->GetAttr("expr");
    expr->Parse(recipe.empty() ? "@0=Id(%1)" : recipe);
  } else {
    // Add op with inputs and output.
    CHECK_EQ(step->outdegree(), 1);
    std::vector<Express::Var *> args(step->indegree());
    for (int i = 0; i < step->indegree(); ++i) {
      args[i] = expr->Variable(Express::INPUT, i);
    }
    Express::Op *func = expr->Function(OpType(step->type()), args);
    func->Assign(expr->Variable(Express::OUTPUT, 0));
    expr->CompactTempVars();
  }

  // Mark scalar and constant inputs.
  for (int i = 0; i < step->indegree(); ++i) {
    if (step->input(i)->elements() == 1) {
      Express::Var *var = expr->Variable(Express::INPUT, i);
      if (step->input(i)->constant()) {
        var->type = Express::CONST;
      } else {
        var->single = true;
      }
    }
  }
}

// Expression code generator for element-wise operations.
struct Expression {
  // Initialize expression.
  Expression(const Step *step, MacroAssembler *masm, int spare_regs = 0)
      : index(step, masm) {
    // Determine output type and shape from the prototype.
    prototype = step->GetPrototype();
    Type type = prototype->type();

    // Compute the maximum common size between inputs and outputs. Scalars are
    // not used for computing the maximum size since these can be broadcast to
    // the vector size.
    int elements = prototype->elements();
    for (auto *input : step->inputs()) {
      if (input->elements() == 1) continue;
      int common = prototype->shape().CommonSize(input->shape());
      if (common < elements) elements = common;
    }

    // Compile expression to be computed.
    InitExpression(step, &expr);

    // Clear single flag for scalar ops since broadcasting and hoisting is not
    // needed in this case.
    if (elements == 1) {
      for (auto *v : expr.vars()) v->single = false;
    }

    // Select expression generator.
    generator = ExpressionGenerator::Select(expr, type, elements);
    CHECK(generator != nullptr);
    if (masm != nullptr) generator->set_approx(masm->options().fast_math);

    // Initialize expression and index generators.
    generator->Initialize(expr, type, spare_regs, &index);
  }

  ~Expression() { delete generator; }

  // Allocate registers.
  bool AllocateRegisters() {
    return index.AllocateRegisters();
  }

  // Generate code for expression loop.
  void Generate(MacroAssembler *masm) {
    index.GenerateInit();
    generator->GenerateInit(masm);
    index.GenerateLoopBegin();
    generator->GenerateBody(masm);
    index.GenerateLoopEnd();
    generator->GenerateEnd(masm);
  }

  // Compute complexity.
  int64 Complexity() {
    Express basic(&generic_model);
    expr.Translate(&basic);
    return prototype->shape().elements() * basic.Complexity();
  }

  // Compute how many spare register we have for hoisting constant out of the
  // loop body. This is only done for floating-point operations to avoid
  // register pressure on the regular x64 integer registers which are also
  // used for the loop indexing.
  static int SpareRegs(const Step *step, const Options &options) {
    int spare_regs = 0;
    Type type = step->GetPrototype()->type();
    if (type == DT_FLOAT || type == DT_DOUBLE) {
      // Perform dry-run to estimate the number of SIMD registers needed.
      MacroAssembler masm(nullptr, 0, options);
      Expression expr(step, &masm, 0);
      CHECK(expr.AllocateRegisters()) << "Register overflow";

      // Count the number of spare SIMD registers.
      bool extended = expr.index.extended_regs();
      while (masm.mm().try_alloc(extended) != -1) spare_regs++;
    }
    return spare_regs;
  }

  // Representative output (or input) from expression.
  Tensor *prototype;

  // Expression to be compiled.
  Express expr;

  // Index generator for element-wise operation.
  ElementwiseIndexGenerator index;

  // Code generator for expression.
  ExpressionGenerator *generator;
};

// Convert division with constant c to multiplication with constant 1/c to
// take advantage of mul being much faster than div. Also transforms div(1,x)
// to rcp(x) and rcp(sqrt(x)) to rsqrt(x).
class DivTransformer : public Transformer {
 public:
  string Name() override { return "DivTransformer"; }

  bool Transform(Flow *flow) override {
    int updates = 0;
    for (Flow::Operation *op : flow->ops()) {
      if (op->type != "Div" && op->type != "RealDiv") continue;
      if (op->indegree() != 2) continue;

      Flow::Variable *first = op->inputs[0];
      Flow::Variable *second = op->inputs[1];

      if (second->type == DT_FLOAT && second->elements() == 1 &&
          second->constant() && second->usages() == 1) {
        // Change Div(x,c) to Mul(x,1/c).
        CHECK_EQ(second->size, sizeof(float));
        op->type = "Mul";
        float multiplier = 1.0 / *reinterpret_cast<const float *>(second->data);
        char *buffer = flow->AllocateMemory(sizeof(float));
        *reinterpret_cast<float *>(buffer) = multiplier;
        second->data = buffer;
        updates++;
      } else if (first->type == DT_FLOAT && first->elements() == 1 &&
                 first->constant()) {
        float value;
        if (first->GetData<float>(&value) && value == 1.0) {
          // Change Div(1,x) to Reciprocal(x).
          op->type = "Reciprocal";
          op->RemoveInput(first);
          updates++;
        }
      }
    }

    for (Flow::Operation *op : flow->Find("Sqrt|Reciprocal")) {
      Flow::Operation *rcp = op;
      Flow::Operation *sqrt = rcp->inputs[0]->producer;
      if (sqrt->outputs[0]->usages() > 1) continue;
      if (sqrt->outputs[0]->out()) continue;

      // Convert Reciprocal(Sqrt(x)) to Rsqrt(x).
      flow->Eliminate(sqrt);
      rcp->type = "Rsqrt";
      updates++;
    }

    return updates > 0;
  }
};

// Convert addition where last term is negated to subtraction.
class AddNegToSubTransformer : public Transformer {
 public:
  string Name() override { return "AddNegToSubTransformer"; }

  bool Transform(Flow *flow) override {
    int updates = 0;
    for (Flow::Operation *op : flow->Find("Neg|1:Add")) {
      Flow::Operation *add = op;
      Flow::Operation *neg = add->inputs[1]->producer;
      if (neg->outputs[0]->usages() == 1 && !neg->outputs[0]->out()) {
        flow->Eliminate(neg);
        add->type = "Sub";
        updates++;
      }
    }
    return updates > 0;
  }
};

// Combine arithmetic operators into expressions that can be computed by a
// Calculate kernel.
class ExpressionTransformer : public Transformer {
 public:
  string Name() override { return "ExpressionTransformer"; }

  bool Transform(Flow *flow) override {
    // Make list of ops that can potentially be included in Calculate or
    // Assign op merging.
    std::vector<Flow::Operation *> candidates;
    for (Flow::Operation *op : flow->ops()) {
      if (IsCalculateOp(op) || IsAssignmentOp(op)) {
        if (!op->GetAttr("strict", false)) {
          candidates.push_back(op);
        }
      }
    }

    // Merge calculate ops into assignment.
    int num_combines = 0;
    bool again = true;
    while (again) {
      again = false;
      for (int i = 0; i < candidates.size(); ++i) {
        Flow::Operation *op = candidates[i];
        if (op == nullptr) continue;
        if (!IsAssignmentOp(op)) continue;

        // Check if producer of one of the inputs is a calculate op.
        for (auto *input : op->inputs) {
          Flow::Operation *producer = input->producer;
          if (producer == nullptr) continue;
          if (!IsCalculateOp(producer)) continue;
          if (producer->GetAttr("strict", false)) continue;

          // Assignment must be the sole consumer of all the outputs from the
          // producer.
          bool contained = true;
          for (auto *v : producer->outputs) {
            if (v->usages() != 1 ||v->consumers[0] != op || v->out()) {
              contained = false;
              break;
            }
          }
          if (!contained) continue;

          // Try to combine op with producer.
          if (Combine(flow, producer, op)) {
            // Remove op from candidate list and try again.
            candidates[i] = nullptr;
            num_combines++;
            again = true;
            break;
          }
        }
      }
    }

    // Merge calculate ops.
    again = true;
    while (again) {
      again = false;
      // Merge calculate ops.
      for (int i = 0; i < candidates.size(); ++i) {
        Flow::Operation *op = candidates[i];
        if (op == nullptr) continue;
        if (!IsCalculateOp(op)) continue;

        // Check if producer of one of the inputs is also a candidate.
        for (auto *input : op->inputs) {
          Flow::Operation *producer = input->producer;
          if (producer == nullptr) continue;
          if (!IsCalculateOp(producer)) continue;
          if (producer->GetAttr("strict", false)) continue;

          // Try to combine op with producer.
          if (Combine(flow, producer, op)) {
            // Remove op from candidate list and try again.
            candidates[i] = nullptr;
            num_combines++;
            again = true;
            break;
          }
        }
      }
    }

    // Merge calculate ops sharing a non-trivial input.
    again = true;
    while (again) {
      again = false;
      // Try to find variable that is used in two different calculate ops.
      for (Flow::Variable *var : flow->vars()) {
        // Find a pair of ops that share a non-trivial input.
        if (var->usages() < 2) continue;
        if (var->elements() < 2) continue;
        Flow::Operation *first = nullptr;
        Flow::Operation *second = nullptr;
        for (Flow::Operation *op : var->consumers) {
          if (!IsCalculateOp(op)) continue;
          if (op->GetAttr("strict", false)) continue;
          if (first == nullptr) {
            first = op;
          } else {
            second = op;
            break;
          }
        }

        if (first != nullptr && second != nullptr) {
          // Try to combine ops.
          if (Combine(flow, first, second)) {
            num_combines++;
            again = true;
            break;
          }
        }
      }
    }

    return num_combines > 0;
  }

  bool Combine(Flow *flow, Flow::Operation *first, Flow::Operation *second) {
    // Check if merging has been disabled.
    if (first->GetAttr("nomerge", false)) return false;
    if (second->GetAttr("nomerge", false)) return false;

    // Check that ops have the same types and output shapes.
    bool assign = IsAssignmentOp(second);
    if (first->indegree() < 1) return false;
    if (first->outdegree() < 1) return false;
    if (second->indegree() < 1) return false;
    if (!assign && second->outdegree() < 1) return false;
    Flow::Variable *prototype = first->GetPrototype();
    Type type = prototype->type;
    const Shape &shape = prototype->shape;
    for (auto *input : first->inputs) {
      if (input->type != type) return false;
      if (!input->shape.defined()) return false;
      if (!input->shape.IsCompatible(shape)) return false;
    }
    for (auto *input : second->inputs) {
      if (input->type != type) return false;
      if (!input->shape.defined()) return false;
      if (!input->shape.IsCompatible(shape)) return false;
    }
    for (auto *output : first->outputs) {
      if (output->type != type) return false;
      if (!output->shape.defined()) return false;
      if (output->shape != shape && output->rank() != 0) return false;
    }
    for (auto *output : second->outputs) {
      if (output->type != type) return false;
      if (!output->shape.defined()) return false;
      if (output->shape != shape && output->rank() != 0) return false;
    }

    // Check for indirect dependencies between ops.
    for (auto *v : first->inputs) {
      if (v->producer != second && v->DependsOn(second)) return false;
    }
    for (auto *v : second->inputs) {
      if (v->producer != first && v->DependsOn(first)) return false;
    }

    // Compute fused expression.
    string fused_recipe = FuseExpressions(first, second);
    if (fused_recipe.empty()) return false;

    // Fuse the two ops and set expression recipe for the fused op.
    Flow::Variable *target = assign ? second->inputs[0] : nullptr;
    Flow::Operation *fused = flow->Fuse(first, second,
                                        assign ? "Assign" : "Calculate",
                                        true);

    // Make sure that the assignment target is still the first input to the
    // combined op.
    if (assign && fused->inputs[0] != target) {
      // Get the input index of the target variable.
      int target_index = fused->InputIndex(target);
      CHECK(target_index != -1);

      // Swap target variable with first input.
      Express expr;
      expr.Parse(fused_recipe);
      auto *vt = expr.Variable(Express::INPUT, target_index);
      auto *v0 = expr.Variable(Express::INPUT, 0);
      vt->id = 0;
      v0->id = target_index;
      fused_recipe = expr.AsRecipe();
      std::swap(fused->inputs[0], fused->inputs[target_index]);
    }

    // Set fused expression for combined op.
    fused->SetAttr("expr", fused_recipe);

    return true;
  }

  string FuseExpressions(Flow::Operation *first, Flow::Operation *second) {
    // Build first expression.
    Express expr1;
    InitExpression(first, &expr1);
    VarMap vars1;
    MapVars(first, &expr1, &vars1);

    // Build second expression.
    bool assign = IsAssignmentOp(second);
    Express expr2;
    InitExpression(second, &expr2);
    VarMap vars2;
    MapVars(second, &expr2, &vars2);

    // Build expression variable mapping for mapping variables in the second
    // expression to variables in the first expression.
    Express::Map mapping;
    int next_input = first->inputs.size();
    int next_output = first->outputs.size();
    if (assign && second->outdegree() == 0) {
      // Add implicit output for assignment target.
      Express::Var *v2 = expr2.Variable(Express::OUTPUT, 0);
      Express::Var *v1 = expr1.Variable(Express::OUTPUT, next_output++);
      mapping[v2] = v1;
    }
    for (Flow::Variable *v : second->inputs) {
      if (first->IsInput(v)) {
        // Map input from second op to input from first op.
        mapping[vars2[v]] = vars1[v];
      } else if (first->IsOutput(v)) {
        if (v->usages() == 1 && !v->out()) {
          // Second op is the only consumer of the output from the first op,
          // so the input can be turned into a temporary variable.
          int id = vars1[v]->id;
          vars1[v]->type = Express::TEMP;
          vars1[v]->id = -1;

          // Adjust numbering of output variables from the first op.
          next_output--;
          for (auto *o : expr1.vars()) {
            if (o->type == Express::OUTPUT && o->id > id) {
              o->id--;
            }
          }
        }

        // Map input from second op to output from first op.
        mapping[vars2[v]] = vars1[v];
      } else {
        // Map input from second op to a new input in the merged expression.
        mapping[vars2[v]] = expr1.Variable(InputType(v), next_input++);
      }
    }
    for (Flow::Variable *v : second->outputs) {
      if (first->IsInput(v)) {
        if (v->usages() == 1 && !v->out()) {
          // First op is the only consumer of the output from the second op,
          // so the output can be turned into a temporary variable.
          int id = vars1[v]->id;
          vars1[v]->type = Express::TEMP;
          vars1[v]->id = -1;

          // Adjust numbering of output variables from the second op.
          next_output--;
          for (auto *o : expr2.vars()) {
            if (o->type == Express::OUTPUT && o->id > id) {
              o->id--;
            }
          }
        }

        // Map output from second op to input to first op.
        mapping[vars2[v]] = vars1[v];
      } else {
        // Map output from second op to a new output in the merged expression.
        mapping[vars2[v]] = expr1.Variable(Express::OUTPUT, next_output++);
      }
    }
    expr1.CompactTempVars();
    expr2.CompactTempVars();

    // Merge second expression into the first one.
    expr1.Merge(&expr2, mapping);

    // Make sure that no reductions are used as inputs to ops in the merged
    // expression.
    for (Express::Op *op : expr1.ops()) {
      if (op->reduction() && op->result->usages() > 0) return "";
    }

    // Return merged recipe.
    expr1.EliminateRedundantMoves();
    return expr1.AsRecipe();
  }

  // Build mapping from flow variables to expression variables.
  static void MapVars(Flow::Operation *op, Express *expr, VarMap *varmap) {
    // Map input variables.
    for (int i = 0; i < op->indegree(); ++i) {
      (*varmap)[op->inputs[i]] = expr->Variable(InputType(op->inputs[i]), i);
    }

    // Map output variables.
    for (int i = 0; i < op->outdegree(); ++i) {
      (*varmap)[op->outputs[i]] = expr->Variable(Express::OUTPUT, i);
    }
  }

  // Determine input variable type.
  static Express::VarType InputType(Flow::Variable *var) {
    if (var->constant() && var->elements() == 1) {
      return Express::CONST;
    } else {
      return Express::INPUT;
    }
  }
};

// Eliminate unused inputs to calculate ops. These are usually constants that
// have been replaced with system constants.
class RemoveUnusedInputs : public Transformer {
 public:
  string Name() override { return "RemoveUnusedInputs"; }

  bool Transform(Flow *flow) override {
    int num_eliminates = 0;
    for (Flow::Operation *op : flow->ops()) {
      bool calculate = op->type == "Calculate";
      bool assign = op->type == "Assign";
      if (calculate || assign) {
        Express expr;
        InitExpression(op, &expr);
        for (int i = 0; i < op->inputs.size(); ++i) {
          if (expr.Lookup(Express::INPUT, i) == nullptr &&
              expr.Lookup(Express::CONST, i) == nullptr) {
            if (assign && i == 0) continue;
            expr.EliminateInput(i);
            op->RemoveInput(op->inputs[i]);
            op->SetAttr("expr", expr.AsRecipe());
            num_eliminates++;
            break;
          }
        }
      }
    }

    return num_eliminates > 0;
  }
};

// Apply transformations to logic operations.
class LogicTransformer : public Transformer {
 public:
  string Name() override { return "LogicTransformer"; }

  bool Transform(Flow *flow) override {
    int num_updates = 0;

    // Fold logical negations into comparison ops.
    bool again = true;
    while (again) {
      again = false;
      for (Flow::Operation *op : flow->ops()) {
        if (op->type != "Not" || op->indegree() != 1) continue;
        Flow::Operation *producer = op->inputs[0]->producer;
        if (producer == nullptr) continue;

        if (producer->type == "Not") {
          // Transform Not(Not(x)) to x.
          again = EliminateDoubleNegation(flow, producer, op);
        } else if (producer->type == "Equal") {
          // Transform Not(Equal(x,y)) to NotEqual(x,y).
          again = FoldNotCompare(flow, producer, op, "NotEqual");
        } else if (producer->type == "NotEqual") {
          // Transform Not(NotEqual(x,y)) to Equal(x,y).
          again = FoldNotCompare(flow, producer, op, "Equal");
        } else if (producer->type == "Less") {
          // Transform Not(Less(x,y)) to GreaterEqual(x,y).
          again = FoldNotCompare(flow, producer, op, "GreaterEqual");
        } else if (producer->type == "LessEqual") {
          // Transform Not(LessEqual(x,y)) to Greater(x,y).
          again = FoldNotCompare(flow, producer, op, "Greater");
        } else if (producer->type == "Greater") {
          // Transform Not(Greater(x,y)) to LessEqual(x,y).
          again = FoldNotCompare(flow, producer, op, "LessEqual");
        } else if (producer->type == "GreaterEqual") {
          // Transform Not(GreaterEqual(x,y)) to Less(x,y).
          again = FoldNotCompare(flow, producer, op, "Less");
        }

        if (again) {
          num_updates++;
          break;
        }
      }
    }

    // Merge negation into logical and.
    for (Flow::Operation *op : flow->Find("Not|0:And")) {
      Flow::Operation *logand = op;
      Flow::Operation *logneg = logand->inputs[0]->producer;
      if (logneg->outputs[0]->usages() == 1 && !logneg->outputs[0]->out()) {
        flow->Eliminate(logneg);
        logand->type = "AndNot";
        num_updates++;
      }
    }
    for (Flow::Operation *op : flow->Find("Not|1:And")) {
      Flow::Operation *logand = op;
      Flow::Operation *logneg = logand->inputs[1]->producer;
      if (logneg->outputs[0]->usages() == 1 && !logneg->outputs[0]->out()) {
        flow->Eliminate(logneg);
        logand->type = "AndNot";
        std::swap(logand->inputs[0], logand->inputs[1]);
        num_updates++;
      }
    }

    return num_updates > 0;
  }

  bool FoldNotCompare(Flow *flow, Flow::Operation *cmp, Flow::Operation *neg,
                      const string &replacement) {
    // Check that negation is the only consumer of the comparison.
    if (cmp->outputs[0]->usages() != 1) return false;
    if (cmp->outputs[0]->out()) return false;

    // Remove negation and invert comparison condition.
    flow->Eliminate(neg);
    cmp->type = replacement;
    return true;
  }

  bool EliminateDoubleNegation(Flow *flow, Flow::Operation *neg1,
                               Flow::Operation *neg2) {
    // Bypass double negation.
    Flow::Variable *result = neg2->outputs[0];
    for (Flow::Operation *op : result->consumers) {
      op->ReplaceInput(result, neg1->inputs[0]);
    }

    // Remove unused negations.
    if (neg2->outputs[0]->usages() == 0 && !neg2->outputs[0]->out()) {
      flow->RemoveOperation(neg2);
    }
    if (neg1->outputs[0]->usages() == 0 && !neg1->outputs[0]->out()) {
      flow->RemoveOperation(neg1);
    }
    return true;
  }
};

// Kernel for computing arithmetic expressions.
class Calculate : public Kernel {
 public:
  Calculate(const string &name, const string &operation, int arity = -1)
      : name_(name), operation_(operation), arity_(arity) {}

  string Name() override { return name_; }
  string Operation() override { return operation_; }

  bool Supports(Step *step) override {
    // Check that operation is compatible.
    if (step->type() != operation_) return false;
    if (arity_ != -1 && step->indegree() != arity_) return false;

    // Check that inputs and outputs have compatible types and shapes.
    bool assign = step->type() == "Assign";
    if (step->indegree() < 1) return false;
    if (!assign && step->outdegree() < 1) return false;
    Tensor *prototype = step->GetPrototype();
    Type type = prototype->type();
    const Shape &shape = prototype->shape();
    for (auto *input : step->inputs()) {
      if (input->type() != type) return false;
      if (!input->Compatible(prototype)) return false;
    }
    for (auto *output : step->outputs()) {
      if (output->type() != type) return false;
      if (output->shape() != shape && output->rank() != 0) return false;
    }

    // Strict math not supported.
    if (step->GetAttr("strict", false)) return false;

    return true;
  }

  void Adjust(Step *step) override {
    Expression expression(step, nullptr);
    step->set_variant(expression.generator->Name());

    // Set alignment.
    int alignment = expression.generator->VectorSize();
    for (auto *input : step->inputs()) {
      if (input->rank() > 0) input->SetMiniumAlignment(alignment);
      input->RequireDense();
      input->RequireStandardOrder();
    }
    for (auto *output : step->outputs()) {
      if (output->rank() > 0) output->SetMiniumAlignment(alignment);
      output->RequireDense();
      output->RequireStandardOrder();
    }

    if (step->type() == "Assign") {
      // Link output reference to assignment target.
      if (step->outdegree() == 1) {
        step->input(0)->Link(step->output(0));
      }
    } else {
      // Enable sharing of inputs and outputs.
      expression.expr.ComputeLiveRanges();
      for (int i = 0; i < step->indegree(); ++i) {
        Tensor *input = step->input(i);
        Express::Var *ivar = expression.expr.Lookup(Express::INPUT, i);
        if (ivar == nullptr) continue;

        for (int j = 0; j < step->outdegree(); ++j) {
          Tensor *output = step->output(j);
          Express::Var *ovar = expression.expr.Lookup(Express::OUTPUT, j);
          if (ovar == nullptr) continue;

          // The input and output can be shared if they have the same format and
          // their live ranges do not overlap.
          if (input->shape() == output->shape() && !ivar->overlaps(ovar)) {
            if (step->AllowInPlace(i, j)) {
              break;
            }
          }
        }
      }
    }
  }

  void Generate(Step *step, MacroAssembler *masm) override {
    // Generate code for element-wise expression evaluation.
    int spare_regs = Expression::SpareRegs(step, masm->options());
    Expression expression(step, masm, spare_regs);
    CHECK(expression.AllocateRegisters()) << "Register overflow";
    expression.Generate(masm);
  }

  int64 Complexity(const Step *step) override {
    Expression expression(step, nullptr);
    return expression.Complexity();
  }

 private:
  string name_;       // kernel name
  string operation_;  // kernel operation
  int arity_;         // number of inputs
};

// Register arithmetic library.
void RegisterArithmeticLibrary(Library *library) {
  generic_model.instruction_set({
    Express::MOV, Express::ADD, Express::SUB, Express::MUL, Express::DIV,
    Express::MINIMUM, Express::MAXIMUM, Express::NEG, Express::ABS,
    Express::SIGN, Express::SQUARE, Express::SQRT,
    Express::MULADD132, Express::MULADD213, Express::MULADD231,
    Express::MULSUB132, Express::MULSUB213, Express::MULSUB231,
    Express::CMPEQOQ, Express::CMPNEUQ, Express::CMPLTOQ,
    Express::CMPLEOQ, Express::CMPGTOQ, Express::CMPGEOQ,
    Express::AND, Express::OR, Express::XOR, Express::ANDNOT, Express::NOT,
    Express::COND, Express::SELECT,
    Express::BITAND, Express::BITOR, Express::BITXOR, Express::BITANDNOT,
    Express::BITEQ, Express::FLOOR,
    Express::CVTFLTINT, Express::CVTINTFLT,
    Express::CVTEXPINT, Express::CVTINTEXP,
    Express::QUADSIGN, Express::ADDINT, Express::SUBINT,
    Express::SUM, Express::PRODUCT, Express::MIN, Express::MAX,
    Express::ALL, Express::ANY,
  });

  library->Register(new Calculate("AddExpr", "Add", 2));
  library->Register(new Calculate("SubExpr", "Sub", 2));
  library->Register(new Calculate("MulExpr", "Mul", 2));
  library->Register(new Calculate("DivExpr", "Div", 2));
  library->Register(new Calculate("MaximumExpr", "Maximum", 2));
  library->Register(new Calculate("MinimumExpr", "Minimum", 2));

  library->Register(new Calculate("LogExpr", "Log", 1));
  library->Register(new Calculate("ExpExpr", "Exp", 1));
  library->Register(new Calculate("SigmoidExpr", "Sigmoid", 1));
  library->Register(new Calculate("ErfExpr", "Erf", 1));
  library->Register(new Calculate("Calculate", "Calculate"));
  library->Register(new Calculate("Assign", "Assign"));

  library->Register(new Calculate("SinExpr", "Sin", 1));
  library->Register(new Calculate("CosExpr", "Cos", 1));
  library->Register(new Calculate("TanExpr", "Tan", 1));
  library->Register(new Calculate("CotExpr", "Cot", 1));
  library->Register(new Calculate("SecExpr", "Sec", 1));
  library->Register(new Calculate("CscExpr", "Csc", 1));

  library->Register(new Calculate("AsinExpr", "Asin", 1));
  library->Register(new Calculate("AcosExpr", "Acos", 1));
  library->Register(new Calculate("AtanExpr", "Atan", 1));
  library->Register(new Calculate("AcotExpr", "Acot", 1));
  library->Register(new Calculate("AsecExpr", "Asec", 1));
  library->Register(new Calculate("AcscExpr", "Acsc", 1));

  library->Register(new Calculate("SinhExpr", "Sinh", 1));
  library->Register(new Calculate("CoshExpr", "Cosh", 1));
  library->Register(new Calculate("TanhExpr", "Tanh", 1));
  library->Register(new Calculate("CothExpr", "Coth", 1));
  library->Register(new Calculate("SechExpr", "Sech", 1));
  library->Register(new Calculate("CschExpr", "Csch", 1));

  library->Register(new Calculate("AsinhExpr", "Asinh", 1));
  library->Register(new Calculate("AcoshExpr", "Acosh", 1));
  library->Register(new Calculate("AtanhExpr", "Atanh", 1));
  library->Register(new Calculate("AcothExpr", "Acoth", 1));
  library->Register(new Calculate("AsechExpr", "Asech", 1));
  library->Register(new Calculate("AcschExpr", "Acsch", 1));

  library->Register(new Calculate("NegExpr", "Neg", 1));
  library->Register(new Calculate("AbsExpr", "Abs", 1));
  library->Register(new Calculate("SignExpr", "Sign", 1));
  library->Register(new Calculate("ReluExpr", "Relu", 1));
  library->Register(new Calculate("SoftsignExpr", "Softsign", 1));
  library->Register(new Calculate("SoftplusExpr", "Softplus", 1));
  library->Register(new Calculate("LogSigmoidExpr", "LogSigmoid", 1));
  library->Register(new Calculate("ReciprocalExpr", "Reciprocal", 1));
  library->Register(new Calculate("SquareExpr", "Square", 1));
  library->Register(new Calculate("SqrtExpr", "Sqrt", 1));
  library->Register(new Calculate("RsqrtExpr", "Rsqrt", 1));

  library->Register(new Calculate("EqualExpr", "Equal", 2));
  library->Register(new Calculate("NotEqualExpr", "NotEqual", 2));
  library->Register(new Calculate("LessExpr", "Less", 2));
  library->Register(new Calculate("LessEqualExpr", "LessEqual", 2));
  library->Register(new Calculate("GreaterExpr", "Greater", 2));
  library->Register(new Calculate("GreaterEqualExpr", "GreaterEqual", 2));

  library->Register(new Calculate("CondExpr", "Cond", 3));
  library->Register(new Calculate("SelectExpr", "Select", 2));

  library->Register(new Calculate("AndExpr", "And", 2));
  library->Register(new Calculate("OrExpr", "Or", 2));
  library->Register(new Calculate("XorExpr", "Xor", 2));
  library->Register(new Calculate("AndNotExpr", "AndNot", 2));
  library->Register(new Calculate("NotExpr", "Not", 1));

  library->Register(new Calculate("SumExpr", "Sum", 1));
  library->Register(new Calculate("ProductExpr", "Product", 1));
  library->Register(new Calculate("MaxExpr", "Max", 1));
  library->Register(new Calculate("MinExpr", "Min", 1));
  library->Register(new Calculate("AllExpr", "All", 1));
  library->Register(new Calculate("AnyExpr", "Any", 1));
  library->Register(new Calculate("CountExpr", "Count", 1));

  library->Register(new Calculate("IdExpr", "Identity", 1));
}

// Register arithmetic transforms.
void RegisterArithmeticTransforms(Library *library) {
  library->RegisterTransformer(new ExpressionTransformer());
  library->RegisterTransformer(new RemoveUnusedInputs());
  library->RegisterTransformer(new DivTransformer());
  library->RegisterTransformer(new AddNegToSubTransformer());
  library->RegisterTransformer(new LogicTransformer());
}

}  // namespace myelin
}  // namespace sling

