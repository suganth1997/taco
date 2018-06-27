#include "test.h"
#include "test_tensors.h"

#include "taco/lower/lower.h"
#include "taco/ir/ir.h"
#include "taco/index_notation/index_notation_rewriter.h"
#include "taco/index_notation/index_notation_nodes.h"
#include "taco/index_notation/kernel.h"
#include "taco/codegen/module.h"
#include "taco/storage/storage.h"
#include "taco/storage/pack.h"

using namespace taco;
using namespace taco::lower;
using namespace taco::storage;

static const Dimension n, m, o;
static const Type vectype(Float64, {n});
static const Type mattype(Float64, {n,m});
static const Type tentype(Float64, {n,m,o});

static TensorVar alpha("alpha", Float64);
static TensorVar beta("beta",   Float64);
static TensorVar delta("delta", Float64);
static TensorVar zeta("zeta",   Float64);

static TensorVar a("a", vectype, Format());
static TensorVar b("b", vectype, Format());
static TensorVar c("c", vectype, Format());
static TensorVar d("d", vectype, Format());

static TensorVar w("w", vectype, dense);

static TensorVar A("A", mattype, Format());
static TensorVar B("B", mattype, Format());
static TensorVar C("C", mattype, Format());
static TensorVar D("D", mattype, Format());

static TensorVar S("S", tentype, Format());
static TensorVar T("T", tentype, Format());
static TensorVar U("U", tentype, Format());
static TensorVar V("V", tentype, Format());

const IndexVar i("i"), iw("iw");
const IndexVar j("j"), jw("jw");
const IndexVar k("k"), kw("kw");

struct TestCase {
  TestCase(const map<TensorVar, vector<pair<vector<int>,double>>>& inputs,
           const vector<pair<vector<int>,double>>& expected,
           const map<TensorVar, vector<int>>& dimensions = {})
      : inputs(inputs), expected(expected), dimensions(dimensions) {}

  map<TensorVar, vector<pair<vector<int>,double>>> inputs;
  vector<pair<vector<int>,double>> expected;
  map<TensorVar, vector<int>> dimensions;  // Shapes default to 5x5x...

  vector<int> getDimensions(TensorVar var) const {
    vector<int> dims;
    if (util::contains(dimensions, var)) {
      dims = dimensions.at(var);
    }
    else {
      for (int i=0; i < var.getOrder(); ++i) {
        dims.push_back(5);
      }
    }
    return dims;
  }

  TensorStorage packResult(TensorVar var, Format format) const {
    return TensorStorage(type<double>(), getDimensions(var), format);
  }

  TensorStorage packArgument(TensorVar var, Format format) const {
    taco_iassert(util::contains(inputs, var)) << var;
    int order = var.getOrder();
    const vector<pair<vector<int>,double>>& input = inputs.at(var);
    size_t num = input.size();
    
    if (order == 0) {
          TensorStorage storage = TensorStorage(type<double>(), {}, format);
      Array array = makeArray(type<double>(), 1);
      *((double*)array.getData()) = input[0].second;
      storage.setValues(array);
      return storage;
    }
    else {
      vector<int> dims = getDimensions(var);
      vector<TypedIndexVector> coords;
      for (int i=0; i < order; ++i) {
        coords.push_back(TypedIndexVector(format.getCoordinateTypeIdx(i), num));
      }
      vector<double> values(num);
      for (size_t i=0; i < input.size(); ++i) {
        auto& coordinates = input[i].first;
        for (size_t j=0; j < coordinates.size(); ++j) {
          coords[j][i] = coordinates[j];
        }
        values[i] = input[i].second;
      }

      return storage::pack(type<double>(), dims, format, coords,
                           values.data(), num);
    }
  }
};

struct Test {
  Test() {}
  IndexStmt stmt;
  vector<TestCase> testCases;
  Test(IndexStmt stmt, const vector<TestCase>& testCases) : stmt(stmt),
      testCases(testCases) {}
};


ostream& operator<<(ostream& os, const Test& stmt) {
  os << endl;
  return os << "  " << stmt.stmt;
}

struct Formats {
  Formats() {}
  Formats(map<TensorVar, Format> formats) : formats(formats) {}
  map<TensorVar, Format> formats;
};

ostream& operator<<(ostream& os, const Formats& formats) {
  for (auto& format : formats.formats) {
    os << endl << "  " << format.first.getName() << " : " << format.second;
  }
  return os << endl;
}

struct stmt : public TestWithParam<::testing::tuple<Test,Formats>> {};

static
map<TensorVar,TensorVar> formatVars(const std::vector<TensorVar>& vars,
                                    const map<TensorVar,Format>& formats) {
  map<TensorVar,TensorVar> formatted;
  for (auto& var : vars) {
    Format format;
    if (util::contains(formats, var)) {
      format = formats.at(var);
    }
    else {
      // Default format is dense in all dimensions
      format = Format(vector<ModeTypePack>(var.getOrder(), dense));
    }
    formatted.insert({var, TensorVar(var.getName(), var.getType(), format)});
  }
  return formatted;
}

TEST_P(stmt, lower) {
  map<TensorVar,TensorVar> varsFormatted =
      formatVars(getTensorVars(get<0>(GetParam()).stmt),
                 get<1>(GetParam()).formats);
  IndexStmt stmt = replace(get<0>(GetParam()).stmt, varsFormatted);

  ASSERT_TRUE(isLowerable(stmt));

  ir::Stmt compute = lower::lower(stmt, "compute", false, true);
  ASSERT_TRUE(compute.defined())
      << "The call to lower returned an undefined IR function.";

  ir::Stmt assemble = lower::lower(stmt, "assemble", true, false);
  ASSERT_TRUE(assemble.defined())
      << "The call to lower returned an undefined IR function.";

  ir::Stmt evaluate = lower::lower(stmt, "evaluate", true, true);
  ASSERT_TRUE(evaluate.defined())
      << "The call to lower returned an undefined IR function.";

  for (auto& testCase : get<0>(GetParam()).testCases) {
    // TODO print test case
//    SCOPED_TRACE("Test case: " + testCase);

    vector<TensorStorage> arguments;

    // Result tensors
    for (auto& var : getResultTensorVars(get<0>(GetParam()).stmt)) {
      Format format = varsFormatted.at(var).getFormat();
      TensorStorage storage = testCase.packResult(var, format);
      arguments.push_back(storage);
    }

    // Input tensors
    for (auto& var : getInputTensorVars(get<0>(GetParam()).stmt)) {
      Format format = varsFormatted.at(var).getFormat();
      TensorStorage storage = testCase.packArgument(var, format);
      arguments.push_back(storage);
    }

    Kernel kernel = compile(stmt);
    std::cout << kernel << std::endl;
    ASSERT_TRUE(kernel(arguments));
    std::cout << util::join(arguments) << std::endl;
    ASSERT_DOUBLE_EQ(-42.0, ((double*)arguments[0].getValues().getData())[0]);
  }
}

#define TEST_STMT(name, statement, formats, testcases) \
INSTANTIATE_TEST_CASE_P(name, stmt,                    \
Combine(Values(Test(statement, testcases)), formats));

TEST_STMT(DISABLED_scalar_neg,
  alpha = -beta,
  Values(Formats()),
  {
    TestCase({{beta, {{{}, 42.0}}}}, {{{}, -42.0}})
  }
)

TEST_STMT(DISABLED_vector_neg,
  forall(i,
         a(i) = -b(i)
         ),
  Values(
         Formats({{a,dense},  {b,dense}}),
         Formats({{a,dense},  {b,sparse}}),
         Formats({{a,sparse}, {b,dense}}),
         Formats({{a,sparse}, {b,sparse}})
         ),
  {
    TestCase({{{b, {{{0},  42.0}, {{3},  4.0}}}},
                   {{{0}, -42.0}, {{3}, -4.0}}})
  }
)

TEST(DISABLED_lower, transpose) {
  TensorVar A(mattype, Format({Sparse,Sparse}, {0,1}));
  TensorVar B(mattype, Format({Sparse,Sparse}, {0,1}));
  TensorVar C(mattype, Format({Sparse,Sparse}, {1,0}));
  string reason;
  ASSERT_FALSE(isLowerable(forall(i,
                                  forall(j,
                                         A(i,j) = B(i,j) + C(i,j)
                                         )),
                           &reason));
  ASSERT_EQ(error::expr_transposition, reason);
}

TEST(DISABLED_lower, transpose2) {
  TensorVar A(mattype, Format({Sparse,Sparse}, {0,1}));
  TensorVar B(mattype, Format({Sparse,Sparse}, {0,1}));
  TensorVar C(mattype, Format({Sparse,Sparse}, {0,1}));
  string reason;
  ASSERT_FALSE(isLowerable(forall(i,
                                  forall(j,
                                         A(i,j) = B(i,j) + C(j,i)
                                         )),
                           &reason));
  ASSERT_EQ(error::expr_transposition, reason);
}

TEST(DISABLED_lower, transpose3) {
  TensorVar A(tentype, Format({Sparse,Sparse,Sparse}, {0,1,2}));
  TensorVar B(tentype, Format({Sparse,Sparse,Sparse}, {0,1,2}));
  TensorVar C(tentype, Format({Sparse,Sparse,Sparse}, {0,1,2}));
  string reason;
  ASSERT_FALSE(isLowerable(forall(i,
                                  forall(j,
                                         forall(k,
                                                A(i,j,k) = B(i,j,k) + C(k,i,j)
                                                ))),
                           &reason));
  ASSERT_EQ(error::expr_transposition, reason);
}
