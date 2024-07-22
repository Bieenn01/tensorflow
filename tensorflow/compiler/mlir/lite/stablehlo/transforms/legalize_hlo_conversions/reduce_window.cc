/* Copyright 2024 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/compiler/mlir/lite/stablehlo/transforms/legalize_hlo_conversions/reduce_window.h"

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/IRMapping.h"  // from @llvm-project
#include "mlir/IR/Matchers.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"  // IWYU pragma: keep
#include "tensorflow/compiler/mlir/lite/stablehlo/transforms/legalize_hlo_conversions/op_util_common.h"
#include "tensorflow/compiler/mlir/lite/stablehlo/transforms/legalize_hlo_conversions/reduce_window_util.h"
#include "tensorflow/compiler/mlir/lite/stablehlo/transforms/legalize_hlo_conversions/util.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"

namespace mlir::odml {
namespace {

bool AreDilationsSupported(const ReduceWindowView& op) {
  auto is_one = [](int64_t v) { return v == 1; };
  return llvm::all_of(op.BaseDilations(), is_one) &&
         llvm::all_of(op.WindowDilations(), is_one);
}

bool IsRankSupported(const ReduceWindowView& op) { return op.Rank() == 4; }

std::optional<std::tuple<ReduceWindowView, Layout>> GetViewIfAttrsSupported(
    mhlo::ReduceWindowOp op) {
  const ReduceWindowView view(op);

  if (!IsRankSupported(view)) {
    return std::nullopt;
  }

  if (!AreDilationsSupported(view)) {
    return std::nullopt;
  }

  auto opt_layout = view.GuessLayout();
  if (!opt_layout.has_value()) {
    return std::nullopt;
  }
  auto layout = opt_layout.value();

  const int64_t batch = layout.SpecialDim1();
  if (!view.Paddings()[batch].Trivial()) {
    return std::nullopt;
  }

  const int64_t chan = layout.SpecialDim2();
  if (!view.Paddings()[chan].Trivial()) {
    return std::nullopt;
  }

  return std::tuple(view, layout);
}

std::optional<bool> IsReduceWindowLegal(mhlo::ReduceWindowOp op) {
  return std::nullopt;
}

std::optional<bool> IsDivideLegal(mhlo::DivOp op) { return std::nullopt; }

Layout TFLNativePoolingLayout(int64_t rank) {
  return Layout(0, rank - 1, llvm::to_vector(llvm::seq<int64_t>(1, rank - 1)));
}

bool IsCstFloatZero(Value val) {
  DenseFPElementsAttr initial_value;
  return matchPattern(val, m_Constant(&initial_value)) &&
         initial_value.getNumElements() == 1 &&
         initial_value.getValues<APFloat>()[0].isZero();
}

llvm::SmallVector<int64_t> Permute(llvm::ArrayRef<int64_t> data,
                                   llvm::ArrayRef<int64_t> perm) {
  llvm::SmallVector<int64_t> res(data.size());
  for (int i = 0; i < data.size(); ++i) {
    res[i] = data[perm[i]];
  }
  return res;
}

Value TransposeTensor(OpBuilder& b, Value tensor,
                      llvm::SmallVector<int64_t> perm) {
  const int64_t perm_size = perm.size();
  auto perm_attr_type = RankedTensorType::get({perm_size}, b.getI64Type());
  auto perm_attr = DenseIntElementsAttr::get(perm_attr_type, perm);
  return b.create<mhlo::TransposeOp>(tensor.getLoc(), tensor, perm_attr);
}

DenseIntElementsAttr BuildDenseI64(OpBuilder& b, ArrayRef<int64_t> shape,
                                   ArrayRef<int64_t> data) {
  return DenseIntElementsAttr::get(RankedTensorType::get(shape, b.getI64Type()),
                                   data);
}

DenseIntElementsAttr BuildDenseI64(OpBuilder& b, ArrayRef<int64_t> data) {
  const int64_t dim = data.size();
  return BuildDenseI64(b, {dim}, data);
}

std::optional<std::tuple<Value, Value>> GetInputAndInitIfValid(
    mhlo::ReduceWindowOp op) {
  if (op->getNumResults() != 1) {
    return std::nullopt;
  }
  if (op.getInputs().size() > 1) {
    return std::nullopt;
  }
  if (op.getInitValues().size() > 1) {
    return std::nullopt;
  }
  auto init_val = op.getInitValues().front();
  if (llvm::dyn_cast<ShapedType>(init_val.getType()).getNumElements() != 1) {
    return std::nullopt;
  }
  return std::tuple(op.getInputs().front(), op.getInitValues().front());
}

//===------------------------------------------------------------------------===
// relayout reduce_window to channel last
//===------------------------------------------------------------------------===

class RelayoutReduceWindow : public OpRewritePattern<mhlo::ReduceWindowOp> {
 public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(mhlo::ReduceWindowOp op,
                                PatternRewriter& rewriter) const final;
};

LogicalResult RelayoutReduceWindow::matchAndRewrite(
    mhlo::ReduceWindowOp op, PatternRewriter& rewriter) const {
  //
  // check and parse attributes
  //=-----

  auto opt_view = GetViewIfAttrsSupported(op);
  if (!opt_view.has_value()) {
    return rewriter.notifyMatchFailure(
        op, "Reduce window attributes not supported.");
  }
  const auto [view, layout] = opt_view.value();

  //
  // get inputs and inits if there are only one
  //=-----

  auto opt_input_and_init = GetInputAndInitIfValid(op);
  if (!opt_input_and_init.has_value()) {
    return rewriter.notifyMatchFailure(
        op, "Reduce window has wrong number of inputs or init values.");
  }
  auto [input, init_val] = opt_input_and_init.value();

  //
  // figure out permutations for layout change
  //=-----

  const auto target_layout = TFLNativePoolingLayout(view.Rank());
  if (layout == target_layout) {
    return rewriter.notifyMatchFailure(
        op, "Reduce window does not need layout change");
  }

  llvm::SmallVector<int64_t> perm_for_inputs =
      layout.GetPermForReLayout(target_layout);

  //
  // permute layout sensitive attrs
  //=-----

  // permute paddings
  auto paddings = view.Paddings();
  llvm::SmallVector<int64_t> new_paddings(paddings.size() * 2);
  for (int i = 0; i < new_paddings.size() / 2; ++i) {
    const auto& dim_pad = paddings[perm_for_inputs[i]];
    new_paddings[2 * i] = dim_pad.Lo();
    new_paddings[2 * i + 1] = dim_pad.Hi();
  }
  const int64_t new_paddings_size = paddings.size();
  auto new_paddings_type =
      RankedTensorType::get({new_paddings_size, 2}, rewriter.getI64Type());
  auto new_paddings_attr =
      DenseIntElementsAttr::get(new_paddings_type, new_paddings);

  // permute window dims
  llvm::SmallVector<int64_t> new_window_dims =
      Permute(view.WindowDims(), perm_for_inputs);
  auto new_window_dims_attr = BuildDenseI64(rewriter, new_window_dims);

  // permute window strides
  llvm::SmallVector<int64_t> new_window_strides =
      Permute(view.WindowStrides(), perm_for_inputs);
  auto new_window_strides_attr = BuildDenseI64(rewriter, new_window_strides);

  //
  // permute params and build new op
  //=-----

  // figure out permuted result type
  llvm::SmallVector<int64_t> perm_for_outputs =
      target_layout.GetPermForReLayout(layout);
  auto cur_out_type = llvm::dyn_cast<ShapedType>(op.getResult(0).getType());
  llvm::SmallVector<int64_t> new_rw_out_shape =
      layout.PermuteShape(target_layout, cur_out_type.getShape());
  auto new_out_type = cur_out_type.clone(new_rw_out_shape);

  // transpose input and build new reduce_window
  auto new_input = TransposeTensor(rewriter, input, perm_for_inputs);
  auto new_rw = rewriter.create<mhlo::ReduceWindowOp>(
      op.getLoc(), new_out_type, new_input, init_val, new_window_dims_attr,
      new_window_strides_attr, BuildDenseI64(rewriter, view.BaseDilations()),
      BuildDenseI64(rewriter, view.WindowDilations()), new_paddings_attr);
  IRMapping ir_map;
  op.getBody().cloneInto(&new_rw.getBody(), ir_map);

  // transpose output and update ir
  auto new_output =
      TransposeTensor(rewriter, new_rw.getResult(0), perm_for_outputs);
  rewriter.replaceOp(op, new_output);

  return success();
}

//===------------------------------------------------------------------------===
// mhlo.reduce_window -> tfl.max_pool
//===------------------------------------------------------------------------===

class LegalizeReduceWindowMax
    : public OpConversionPattern<mhlo::ReduceWindowOp> {
 public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::ReduceWindowOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final;
};

// TODO: b/352960217 - Implement.
LogicalResult LegalizeReduceWindowMax::matchAndRewrite(
    mhlo::ReduceWindowOp op, OpAdaptor adaptor,
    ConversionPatternRewriter& rewriter) const {
  return failure();
}

//===------------------------------------------------------------------------===
// mhlo.div(mhlo.reduce_window, cst | mhlo.reduce_window) -> tfl.avg_pool
//===------------------------------------------------------------------------===

void ReplaceWithAvgPool(mhlo::DivOp op, Value rw_lhs_input,
                        const ReduceWindowView& lhs_view,
                        llvm::StringRef padding, PatternRewriter& rewriter,
                        mhlo::TransposeOp opt_final_tpose) {
  // TODO build the average pool with the div same input. Clone the tpose and
  // chain it to the new average pool, replace div op with transpose
  auto tfl_padding_attr = rewriter.getStringAttr(padding);
  auto tfl_faf_attr = rewriter.getStringAttr("NONE");

  Type out_type =
      opt_final_tpose ? opt_final_tpose.getOperand().getType() : op.getType();

  const int32_t tfl_filter_h = lhs_view.WindowDims()[1];
  auto tfl_filter_h_attr = rewriter.getI32IntegerAttr(tfl_filter_h);

  const int32_t tfl_filter_w = lhs_view.WindowDims()[2];
  auto tfl_filter_w_attr = rewriter.getI32IntegerAttr(tfl_filter_w);

  const int32_t tfl_stride_h = lhs_view.WindowStrides()[1];
  auto tfl_stride_h_attr = rewriter.getI32IntegerAttr(tfl_stride_h);

  const int32_t tfl_stride_w = lhs_view.WindowStrides()[2];
  auto tfl_stride_w_attr = rewriter.getI32IntegerAttr(tfl_stride_w);

  Value final_op = rewriter.create<TFL::AveragePool2DOp>(
      op->getLoc(), out_type, rw_lhs_input, tfl_filter_h_attr,
      tfl_filter_w_attr, tfl_padding_attr, tfl_stride_h_attr, tfl_stride_w_attr,
      tfl_faf_attr);

  if (opt_final_tpose) {
    final_op = rewriter
                   .create<mhlo::TransposeOp>(final_op.getLoc(), final_op,
                                              opt_final_tpose.getPermutation())
                   .getResult();
  }

  rewriter.replaceOp(op, final_op);
}

// Walks up the op and ignore all precedding ops of type Tys.
// Returns the first producer op whose type is not in Tys.
template <typename... Tys>
Value RecursivelyWalkUp(Value op) {
  while (llvm::isa_and_nonnull<Tys...>(op.getDefiningOp())) {
    Operation* producer = op.getDefiningOp();
    op = producer->getOperand(/*idx=*/0);
  }

  return op;
}

class LegalizeAvgPool : public OpConversionPattern<mhlo::DivOp> {
 public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::DivOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final;
};

LogicalResult LegalizeAvgPool::matchAndRewrite(
    mhlo::DivOp div_op, OpAdaptor adaptor,
    ConversionPatternRewriter& rewriter) const {
  //
  // parse and validate reduce window lhs
  //=-----

  auto div_lhs = div_op.getLhs();
  // If div's input is transposed, save it to chain on the new pool op.
  mhlo::TransposeOp opt_final_tpose;
  if (auto div_lhs_op = div_lhs.getDefiningOp()) {
    opt_final_tpose = llvm::dyn_cast_or_null<mhlo::TransposeOp>(div_lhs_op);
  }

  auto rw_lhs_val = RecursivelyWalkUp<mhlo::TransposeOp>(div_lhs);
  auto rw_lhs =
      llvm::dyn_cast_or_null<mhlo::ReduceWindowOp>(rw_lhs_val.getDefiningOp());
  if (!rw_lhs) {
    return rewriter.notifyMatchFailure(
        div_op, "Could not match lhs of div on reduce window.");
  }

  const auto opt_rw_lhs_view = GetViewIfAttrsSupported(rw_lhs);
  if (!opt_rw_lhs_view.has_value()) {
    return rewriter.notifyMatchFailure(div_op, "Lhs rw is not valid.");
  }
  const auto [rw_lhs_view, rw_lhs_layout] = opt_rw_lhs_view.value();
  if (rw_lhs_view.Rank() != 4) {
    return rewriter.notifyMatchFailure(div_op, "Not a 2d pooling operator.");
  }
  if (rw_lhs_layout != TFLNativePoolingLayout(rw_lhs_layout.Rank())) {
    return rewriter.notifyMatchFailure(
        div_op, "Lhs reduce window not tfl standard layout.");
  }

  auto opt_rw_lhs_input_and_init = GetInputAndInitIfValid(rw_lhs);
  if (!opt_rw_lhs_input_and_init.has_value()) {
    return rewriter.notifyMatchFailure(
        div_op, "Lhs reduce window has wrong number of inputs or init values.");
  }
  auto [rw_lhs_input, rw_lhs_init_val] = opt_rw_lhs_input_and_init.value();
  auto rw_lhs_input_type = llvm::dyn_cast<ShapedType>(rw_lhs_input.getType());

  // Check that the reduce-window is a sum-reduce-window.
  if (failed(MatchBinaryReduceFunction<mhlo::AddOp>(rw_lhs.getBody()))) {
    return rewriter.notifyMatchFailure(div_op,
                                       "Failed to match rw lhs binary func.");
  }

  // Check that this is a floating point reduce window with a rank of 4 or 5.
  auto rw_lhs_type =
      mlir::dyn_cast<RankedTensorType>(rw_lhs.getResult(0).getType());
  if (!mlir::isa<FloatType>(rw_lhs_type.getElementType())) {
    return rewriter.notifyMatchFailure(div_op,
                                       "Reduce window lhs most be float type.");
  }

  // If the init value isn't zero then it can't be an average pool.
  if (!IsCstFloatZero(rw_lhs_init_val)) {
    return rewriter.notifyMatchFailure(
        div_op, "Reduce window lhs init value is not zero.");
  }

  std::string tfl_padding = "VALID";
  for (int i = 1; i < rw_lhs_view.Rank() - 1; ++i) {
    const auto& dim_pad = rw_lhs_view.Paddings()[i];
    const int64_t dim_stride = rw_lhs_view.WindowStrides()[i];
    const int64_t in_dim = rw_lhs_input_type.getShape()[i];
    const int64_t out_dim = rw_lhs_type.getShape()[i];
    if (dim_pad.Trivial()) {
      continue;
    }
    if (!IsSamePaddingOnDim(dim_pad, out_dim, in_dim, dim_stride)) {
      return rewriter.notifyMatchFailure(div_op,
                                         "Padding is not same or valid.");
    }
    tfl_padding = "SAME";
  }

  //
  // case 1: rhs is splat const with val == window_size
  //=-----

  {
    DenseFPElementsAttr divisor;
    auto div_rhs = RecursivelyWalkUp<mhlo::BroadcastInDimOp, mhlo::TransposeOp>(
        div_op.getRhs());
    if (matchPattern(div_rhs, m_Constant(&divisor))) {
      if (!divisor.isSplat()) {
        return failure();
      }

      if (!divisor.getSplatValue<APFloat>().isExactlyValue(
              rw_lhs_view.WindowSize())) {
        return rewriter.notifyMatchFailure(
            div_op, "Rhs splat const is not equal to window size.");
      }

      if (tfl_padding != "VALID") {
        return rewriter.notifyMatchFailure(div_op,
                                           "Matching on rhs splat const where "
                                           "rw lhs has non-trivial padding.");
      }

      ReplaceWithAvgPool(div_op, rw_lhs_input, rw_lhs_view, tfl_padding,
                         rewriter, opt_final_tpose);
      return success();
    }
  }

  //
  // case 2: rhs is another reduce window over 1's with same config as lhs
  //=-----

  {
    Value divisor = RecursivelyWalkUp<mhlo::BroadcastInDimOp, mhlo::ReshapeOp,
                                      mhlo::TransposeOp>(div_op.getRhs());
    auto rw_rhs =
        dyn_cast_or_null<mhlo::ReduceWindowOp>(divisor.getDefiningOp());
    if (!rw_rhs) {
      return rewriter.notifyMatchFailure(
          div_op, "Rhs of div op is not a reduce window.");
    }

    const auto opt_rw_rhs_view = GetViewIfAttrsSupported(rw_rhs);
    if (!opt_rw_rhs_view.has_value()) {
      return rewriter.notifyMatchFailure(div_op, "Rhs rw is not valid.");
    }
    const auto [rw_rhs_view, rw_rhs_layout] = opt_rw_rhs_view.value();
    if (rw_rhs_layout != TFLNativePoolingLayout(rw_rhs_layout.Rank())) {
      return rewriter.notifyMatchFailure(
          div_op, "Rhs reduce window not tfl standard layout.");
    }

    // Check that RHS is a sum-reduce-window.
    if (failed(MatchBinaryReduceFunction<mhlo::AddOp>(rw_rhs.getBody()))) {
      return rewriter.notifyMatchFailure(
          div_op, "Rhs rw body function is not an add op.");
    }

    auto opt_rw_rhs_input_and_init = GetInputAndInitIfValid(rw_rhs);
    if (!opt_rw_rhs_input_and_init.has_value()) {
      return rewriter.notifyMatchFailure(
          div_op,
          "Rhs reduce window has wrong number of inputs or init values.");
    }
    auto [rw_rhs_input, rw_rhs_init_val] = opt_rw_rhs_input_and_init.value();

    if (!IsCstFloatZero(rw_rhs_init_val)) {
      return rewriter.notifyMatchFailure(div_op,
                                         "Rhs rw init vals is not zero.");
    }

    rw_rhs_input = RecursivelyWalkUp<mhlo::BroadcastInDimOp, mhlo::TransposeOp>(
        rw_rhs_input);
    DenseFPElementsAttr rhs_input_data;
    if (!matchPattern(rw_rhs_input, m_Constant(&rhs_input_data)) ||
        !rhs_input_data.isSplat() ||
        !rhs_input_data.getSplatValue<APFloat>().isExactlyValue(1.0)) {
      return rewriter.notifyMatchFailure(div_op,
                                         "Rw rhs input is not splat of 1.0.");
    }

    // Check that the two reduce window have the same window configuration.
    if (rw_lhs.getWindowDimensions() != rw_rhs.getWindowDimensions() ||
        rw_lhs.getWindowStrides() != rw_rhs.getWindowStrides() ||
        rw_lhs.getPadding() != rw_rhs.getPadding()) {
      return rewriter.notifyMatchFailure(
          div_op, "Lhs rw and Rhs rw do not have the same config.");
    }

    ReplaceWithAvgPool(div_op, rw_lhs_input, rw_lhs_view, tfl_padding, rewriter,
                       opt_final_tpose);
    return success();
  }

  return failure();
}

}  // namespace

void PopulateLegalizeReduceWindowPatterns(MLIRContext* ctx,
                                          RewritePatternSet& patterns,
                                          ConversionTarget& target) {
  patterns.add<LegalizeAvgPool>(ctx);
  target.addDynamicallyLegalOp<mhlo::ReduceWindowOp>(IsReduceWindowLegal);
  target.addDynamicallyLegalOp<mhlo::DivOp>(IsDivideLegal);
}

void PopulatePrepareReduceWindowPatterns(MLIRContext* ctx,
                                         RewritePatternSet& patterns) {
  patterns.add<RelayoutReduceWindow>(ctx);
}

}  // namespace mlir::odml
