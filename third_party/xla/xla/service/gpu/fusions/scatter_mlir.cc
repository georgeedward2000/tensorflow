/* Copyright 2024 The OpenXLA Authors.

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
#include "xla/service/gpu/fusions/scatter_mlir.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/SCF/IR/SCF.h"  // from @llvm-project
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // from @llvm-project
#include "mlir/IR/AffineExpr.h"  // from @llvm-project
#include "mlir/IR/AffineMap.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/gpu/fusions/mlir/computation_partitioner.h"
#include "xla/service/gpu/fusions/mlir/elemental_hlo_to_mlir.h"
#include "xla/service/gpu/fusions/mlir/ir/xla_gpu_ops.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/model/indexing_map.h"
#include "xla/shape.h"
#include "xla/xla_data.pb.h"

namespace xla {
namespace gpu {
namespace {

using llvm::SmallVector;
using mlir::Location;
using mlir::OpBuilder;
using mlir::Value;
using mlir::ValueRange;
using mlir::arith::AddIOp;
using mlir::arith::AndIOp;
using mlir::arith::CmpIOp;
using mlir::arith::CmpIPredicate;
using mlir::arith::ConstantIndexOp;
using mlir::func::ReturnOp;
using mlir::tensor::InsertOp;
using mlir_converter::ApplyAffineMap;
using mlir_converter::CallTargetProvider;
using mlir_converter::PartitionedComputations;
using mlir_converter::ProvideParameter;

namespace scf = ::mlir::scf;

}  // namespace

bool MlirScatterFusion::IsSupported(const HloFusionAnalysis& analysis) {
  auto* scatter = Cast<HloScatterInstruction>(analysis.fusion_heroes().front());
  if (scatter->scatter_operand_count() != 1) {
    LOG(ERROR) << "Variadic scatter is not supported like in the legacy "
                  "emitter, although it is possible to make it work when the "
                  "indices are unique.";
    return false;
  }
  return true;
}

std::optional<IndexingMap> MlirScatterFusion::ComputeThreadIdToOutputIndexing(
    int64_t root_index, mlir::MLIRContext* ctx) const {
  return std::nullopt;
}

std::optional<IndexingMap> MlirScatterFusion::ComputeThreadIdToInputIndexing(
    int64_t root_index, int64_t hero_operand_index,
    mlir::MLIRContext* ctx) const {
  auto* scatter =
      DynCast<HloScatterInstruction>(analysis_.fusion_heroes().front());
  int64_t scatter_operand_count = scatter->scatter_operand_count();
  // Scatter operands a packed in the following way:
  // Operand IDs [0, scatter_operand_count - 1] for `scatter operands`.
  // Operand ID  scatter_operand_count for `scatter indices`.
  // Operand IDs [scatter_operand_count + 1, 2 * scatter_operand_count] for
  // `scatter updates`.

  // For scatter operands we do not know the thread ID indexing.
  if (hero_operand_index < scatter_operand_count) {
    return std::nullopt;
  }
  // Compute thread id mapping based on the first update operand.
  Shape scatter_update_shape = scatter->scatter_updates().front()->shape();
  IndexingMap scatter_update_map = GetDefaultThreadIdToOutputIndexingMap(
      launch_dimensions(), config_.unroll_factor, scatter_update_shape, ctx);

  // For scatter indices we project indexing for scatter updates and take the
  // first result of the affine map only, because they coincide.
  if (hero_operand_index == scatter_operand_count) {
    Shape scatter_indices_shape = scatter->scatter_indices()->shape();
    CHECK_EQ(scatter_indices_shape.rank(), 2) << scatter->ToString();
    // Create a map from scatter update to scatter indices.
    IndexingMap updates_to_indices_map{
        mlir::AffineMap::get(
            /*dimCount=*/scatter_update_shape.rank(), /*symbolCount=*/1,
            {mlir::getAffineDimExpr(0, ctx), mlir::getAffineSymbolExpr(0, ctx)},
            ctx),
        /*dim_ranges=*/RangesFromTensorSizes(scatter_update_shape.dimensions()),
        /*symbol_ranges=*/
        RangesFromTensorSizes({scatter_indices_shape.dimensions(1)})};
    auto scatter_indices_map = scatter_update_map * updates_to_indices_map;
    scatter_indices_map.Simplify();
    return scatter_indices_map;
  }
  return scatter_update_map;
}

LaunchDimensions MlirScatterFusion::launch_dimensions() const {
  auto* scatter = analysis_.fusion_heroes().front();
  // Compute thread id mapping based on the shape of update operand.
  auto& shape = scatter->operands().back()->shape();
  return CalculateLaunchDimensions(shape, analysis_.device_info());
}

absl::flat_hash_set<const HloInstruction*>
MlirScatterFusion::GetInstructionsWithCustomCodegen(
    const HloFusionInstruction& fusion) const {
  return {analysis_.fusion_heroes()[0]};
}

mlir::Value EmitScatterComputation(
    const HloInstruction* scatter, ValueRange indices, Value update_elem,
    Value output_tensor,
    const mlir_converter::PartitionedComputation& root_computation,
    const mlir_converter::CallTargetProvider& call_targets,
    mlir::ImplicitLocOpBuilder& b) {
  constexpr int kScatterOperandIndex = 0;
  auto reducer =
      call_targets(scatter->called_computations()[0]->root_instruction());
  if (scatter->unique_indices()) {
    auto operand_elem =
        ProvideParameter(root_computation, scatter, kScatterOperandIndex,
                         indices, call_targets, b)[0];
    auto reduced_val = mlir_converter::InlineBlock(
        b, reducer.getBody().front(), {operand_elem, update_elem})[0];

    return b.create<InsertOp>(reduced_val, output_tensor, indices);
  }
  auto atomic_rmw = b.create<AtomicRMWOp>(output_tensor, indices);
  mlir::OpBuilder body_builder = atomic_rmw.getBodyBuilder();
  auto reduced_val = mlir_converter::InlineBlock(
      body_builder, reducer.getBody().front(),
      {atomic_rmw.getCurrentValue(), update_elem})[0];
  body_builder.create<xla::gpu::YieldOp>(reducer->getLoc(), reduced_val);
  return atomic_rmw->getResult(0);
}

// The scatter has to be canonicalized with `scatter_simplifier` pass.
absl::Status MlirScatterFusion::EmitEntryFunction(
    const PartitionedComputations& computations,
    const CallTargetProvider& call_targets, mlir::func::FuncOp entry_function,
    const HloFusionInstruction& fusion) const {
  constexpr int kScatterOperandIndex = 0;
  constexpr int kScatterIndicesIndex = 1;
  constexpr int kScatterUpdateIndex = 2;
  const auto* scatter = analysis_.fusion_heroes()[0];
  const HloInstruction* scatter_operand =
      scatter->operand(kScatterOperandIndex);
  const HloInstruction* scatter_indices =
      scatter->operand(kScatterIndicesIndex);
  const HloInstruction* scatter_update = scatter->operand(kScatterUpdateIndex);

  mlir::MLIRContext* mlir_context = entry_function.getContext();
  auto thread_id_to_update_map =
      ComputeThreadIdToInputIndexing(
          /*root_index=*/0, /*hero_operand_index=*/kScatterUpdateIndex,
          mlir_context)
          .value();
  thread_id_to_update_map.Simplify();
  thread_id_to_update_map.RemoveUnusedSymbols();

  const auto& root_computation = computations.FindPartitionedComputation(
      fusion.fused_instructions_computation());
  mlir::ImplicitLocOpBuilder b(entry_function.getLoc(), entry_function);
  b.setInsertionPointToStart(entry_function.addEntryBlock());

  SmallVector<Value> result_tensors{entry_function.getArguments().back()};
  auto c0 = b.create<ConstantIndexOp>(0);

  auto scatter_result = EmitThreadLoopNest(
      b, result_tensors, thread_id_to_update_map,
      [&](ValueRange output_tensors, ValueRange dim_values,
          ValueRange symbol_values) -> SmallVector<Value> {
        // Extract input element.
        auto update_tensor_indices =
            ApplyAffineMap(thread_id_to_update_map.GetAffineMap(), dim_values,
                           symbol_values, b);
        auto update_elem =
            ProvideParameter(root_computation, scatter, kScatterUpdateIndex,
                             update_tensor_indices, call_targets, b)
                .front();

        // Extract slice offsets from scatter_indices operand, compute if the
        // whole slice of scatter_update operand will fit into the output.
        mlir::Value is_in_bounds =
            b.create<mlir::arith::ConstantIntOp>(1, b.getI1Type());
        SmallVector<Value, 4> indices{
            llvm::ArrayRef(update_tensor_indices).drop_front()};
        for (int i = 0; i < scatter_operand->shape().rank(); ++i) {
          Value extracted_index = c0;
          if (i < scatter_indices->shape().dimensions(1)) {
            SmallVector<Value, 4> indices_tensor_indices = {
                update_tensor_indices.front(), b.create<ConstantIndexOp>(i)};
            extracted_index = ProvideParameter(
                root_computation, scatter, kScatterIndicesIndex,
                indices_tensor_indices, call_targets, b)[0];
            if (extracted_index.getType() != b.getIndexType()) {
              extracted_index = b.create<mlir::arith::IndexCastOp>(
                  b.getIndexType(), extracted_index);
            }
          }
          is_in_bounds = b.create<AndIOp>(
              is_in_bounds,
              b.create<CmpIOp>(CmpIPredicate::sge, extracted_index, c0));
          Value ub = b.create<ConstantIndexOp>(
              scatter_operand->shape().dimensions(i) -
              scatter_update->shape().dimensions(i + 1));
          is_in_bounds = b.create<AndIOp>(
              is_in_bounds,
              b.create<CmpIOp>(CmpIPredicate::sle, extracted_index, ub));
          indices[i] = b.create<AddIOp>(extracted_index, indices[i]);
        }
        // Call scatter's computation if is_in_bounds.
        Value output_tensor = output_tensors.front();
        Value predicated_update =
            b.create<scf::IfOp>(
                 is_in_bounds,
                 [&](OpBuilder& then_builder, Location then_loc) -> void {
                   Value updated_output = EmitScatterComputation(
                       scatter, indices, update_elem, output_tensor,
                       root_computation, call_targets, b);
                   b.create<scf::YieldOp>(updated_output);
                 },
                 [&](OpBuilder& else_b, Location else_loc) {
                   b.create<scf::YieldOp>(output_tensor);
                 })
                .getResult(0);
        return {predicated_update};
      });
  b.create<ReturnOp>(scatter_result);
  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
