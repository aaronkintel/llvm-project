//===- Shape.cpp - MLIR Shape Operations ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <utility>

#include "mlir/Dialect/Shape/IR/Shape.h"

#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/CommonFolders.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Traits.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/FunctionImplementation.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Transforms/InliningUtils.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace mlir::shape;

#include "mlir/Dialect/Shape/IR/ShapeOpsDialect.cpp.inc"

namespace {
#include "ShapeCanonicalization.inc"
} // namespace

RankedTensorType shape::getExtentTensorType(MLIRContext *ctx, int64_t rank) {
  return RankedTensorType::get({rank}, IndexType::get(ctx));
}

bool shape::isExtentTensorType(Type type) {
  auto ranked = type.dyn_cast<RankedTensorType>();
  return ranked && ranked.getRank() == 1 && ranked.getElementType().isIndex();
}

LogicalResult shape::getShapeVec(Value input,
                                 SmallVectorImpl<int64_t> &shapeValues) {
  if (auto inputOp = input.getDefiningOp<ShapeOfOp>()) {
    auto type = inputOp.getArg().getType().cast<ShapedType>();
    if (!type.hasRank())
      return failure();
    llvm::append_range(shapeValues, type.getShape());
    return success();
  }
  DenseIntElementsAttr attr;
  if (matchPattern(input, m_Constant(&attr))) {
    llvm::append_range(shapeValues, attr.getValues<int64_t>());
    return success();
  }
  return failure();
}

static bool isErrorPropagationPossible(TypeRange operandTypes) {
  return llvm::any_of(operandTypes, [](Type ty) {
    return ty.isa<SizeType, ShapeType, ValueShapeType>();
  });
}

static LogicalResult verifySizeOrIndexOp(Operation *op) {
  assert(op != nullptr && op->getNumResults() == 1);
  Type resultTy = op->getResultTypes().front();
  if (isErrorPropagationPossible(op->getOperandTypes())) {
    if (!resultTy.isa<SizeType>())
      return op->emitOpError()
             << "if at least one of the operands can hold error values then "
                "the result must be of type `size` to propagate them";
  }
  return success();
}

static LogicalResult verifyShapeOrExtentTensorOp(Operation *op) {
  assert(op != nullptr && op->getNumResults() == 1);
  Type resultTy = op->getResultTypes().front();
  if (isErrorPropagationPossible(op->getOperandTypes())) {
    if (!resultTy.isa<ShapeType>())
      return op->emitOpError()
             << "if at least one of the operands can hold error values then "
                "the result must be of type `shape` to propagate them";
  }
  return success();
}

template <typename... Ty>
static bool eachHasOnlyOneOfTypes(TypeRange typeRange) {
  return typeRange.size() == 1 && typeRange.front().isa<Ty...>();
}

template <typename... Ty, typename... ranges>
static bool eachHasOnlyOneOfTypes(TypeRange l, ranges... rs) {
  return eachHasOnlyOneOfTypes<Ty...>(l) && eachHasOnlyOneOfTypes<Ty...>(rs...);
}

//===----------------------------------------------------------------------===//
// InlinerInterface
//===----------------------------------------------------------------------===//

namespace {
/// This class defines the interface for inlining shape dialect ops.
struct ShapeInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;

  // Returns true if the given region 'src' can be inlined into the region
  // 'dest' that is attached to an operation registered to the current dialect.
  bool isLegalToInline(Region *dest, Region *src, bool wouldBeCloned,
                       BlockAndValueMapping &) const final {
    return true;
  }

  // Returns true if the given operation 'op', that is registered to this
  // dialect, can be inlined into the region 'dest' that is attached to an
  // operation registered to the current dialect.
  bool isLegalToInline(Operation *op, Region *dest, bool wouldBeCloned,
                       BlockAndValueMapping &) const final {
    return true;
  }
};
} // namespace

void ShapeDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "mlir/Dialect/Shape/IR/ShapeOps.cpp.inc"
      >();
  addTypes<
#define GET_TYPEDEF_LIST
#include "mlir/Dialect/Shape/IR/ShapeOpsTypes.cpp.inc"
      >();
  addInterfaces<ShapeInlinerInterface>();
  // Allow unknown operations during prototyping and testing. As the dialect is
  // still evolving it makes it simple to start with an unregistered ops and
  // try different variants before actually defining the op.
  allowUnknownOperations();
}

Operation *ShapeDialect::materializeConstant(OpBuilder &builder,
                                             Attribute value, Type type,
                                             Location loc) {
  if (type.isa<ShapeType>() || isExtentTensorType(type))
    return builder.create<ConstShapeOp>(loc, type,
                                        value.cast<DenseIntElementsAttr>());
  if (type.isa<SizeType>())
    return builder.create<ConstSizeOp>(loc, type, value.cast<IntegerAttr>());
  if (type.isa<WitnessType>())
    return builder.create<ConstWitnessOp>(loc, type, value.cast<BoolAttr>());
  if (arith::ConstantOp::isBuildableWith(value, type))
    return builder.create<arith::ConstantOp>(loc, type, value);
  return nullptr;
}

LogicalResult ShapeDialect::verifyOperationAttribute(Operation *op,
                                                     NamedAttribute attribute) {
  // Verify shape.lib attribute.
  if (attribute.getName() == "shape.lib") {
    if (!op->hasTrait<OpTrait::SymbolTable>())
      return op->emitError(
          "shape.lib attribute may only be on op implementing SymbolTable");

    if (auto symbolRef = attribute.getValue().dyn_cast<SymbolRefAttr>()) {
      auto *symbol = SymbolTable::lookupSymbolIn(op, symbolRef);
      if (!symbol)
        return op->emitError("shape function library ")
               << symbolRef << " not found";
      return isa<shape::FunctionLibraryOp>(symbol)
                 ? success()
                 : op->emitError()
                       << symbolRef << " required to be shape function library";
    }

    if (auto arr = attribute.getValue().dyn_cast<ArrayAttr>()) {
      // Verify all entries are function libraries and mappings in libraries
      // refer to unique ops.
      DenseSet<StringAttr> key;
      for (auto it : arr) {
        if (!it.isa<SymbolRefAttr>())
          return op->emitError(
              "only SymbolRefAttr allowed in shape.lib attribute array");

        auto shapeFnLib = dyn_cast<shape::FunctionLibraryOp>(
            SymbolTable::lookupSymbolIn(op, it.cast<SymbolRefAttr>()));
        if (!shapeFnLib)
          return op->emitError()
                 << it << " does not refer to FunctionLibraryOp";
        for (auto mapping : shapeFnLib.getMapping()) {
          if (!key.insert(mapping.getName()).second) {
            return op->emitError("only one op to shape mapping allowed, found "
                                 "multiple for `")
                   << mapping.getName() << "`";
          }
        }
      }
      return success();
    }

    return op->emitError("only SymbolRefAttr or array of SymbolRefAttrs "
                         "allowed as shape.lib attribute");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// AnyOp
//===----------------------------------------------------------------------===//

// TODO: Canonicalization should be implemented for shapes that can be
// determined through mixtures of the known dimensions of the inputs.
OpFoldResult AnyOp::fold(ArrayRef<Attribute> operands) {
  // Only the last operand is checked because AnyOp is commutative.
  if (operands.back())
    return operands.back();

  return nullptr;
}

//===----------------------------------------------------------------------===//
// AssumingOp
//===----------------------------------------------------------------------===//

ParseResult AssumingOp::parse(OpAsmParser &parser, OperationState &result) {
  result.regions.reserve(1);
  Region *doRegion = result.addRegion();

  auto &builder = parser.getBuilder();
  OpAsmParser::UnresolvedOperand cond;
  if (parser.parseOperand(cond) ||
      parser.resolveOperand(cond, builder.getType<WitnessType>(),
                            result.operands))
    return failure();

  // Parse optional results type list.
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Parse the region and add a terminator if elided.
  if (parser.parseRegion(*doRegion, /*arguments=*/{}, /*argTypes=*/{}))
    return failure();
  AssumingOp::ensureTerminator(*doRegion, parser.getBuilder(), result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

void AssumingOp::print(OpAsmPrinter &p) {
  bool yieldsResults = !getResults().empty();

  p << " " << getWitness();
  if (yieldsResults)
    p << " -> (" << getResultTypes() << ")";
  p << ' ';
  p.printRegion(getDoRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/yieldsResults);
  p.printOptionalAttrDict((*this)->getAttrs());
}

namespace {
// Removes AssumingOp with a passing witness and inlines the region.
struct AssumingWithTrue : public OpRewritePattern<AssumingOp> {
  using OpRewritePattern<AssumingOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AssumingOp op,
                                PatternRewriter &rewriter) const override {
    auto witness = op.getWitness().getDefiningOp<ConstWitnessOp>();
    if (!witness || !witness.getPassingAttr())
      return failure();

    AssumingOp::inlineRegionIntoParent(op, rewriter);
    return success();
  }
};

struct AssumingOpRemoveUnusedResults : public OpRewritePattern<AssumingOp> {
  using OpRewritePattern<AssumingOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AssumingOp op,
                                PatternRewriter &rewriter) const override {
    Block *body = op.getBody();
    auto yieldOp = llvm::cast<AssumingYieldOp>(body->getTerminator());

    // Find used values.
    SmallVector<Value, 4> newYieldOperands;
    for (auto [opResult, yieldOperand] :
         llvm::zip(op.getResults(), yieldOp.getOperands())) {
      if (!opResult.getUses().empty()) {
        newYieldOperands.push_back(yieldOperand);
      }
    }

    // Rewrite only if redundant results exist.
    if (newYieldOperands.size() == yieldOp->getNumOperands())
      return failure();

    // Replace yield op in the old assuming op's body and move the entire region
    // to the new assuming op.
    rewriter.setInsertionPointToEnd(body);
    auto newYieldOp =
        rewriter.replaceOpWithNewOp<AssumingYieldOp>(yieldOp, newYieldOperands);
    rewriter.setInsertionPoint(op);
    auto newOp = rewriter.create<AssumingOp>(
        op.getLoc(), newYieldOp->getOperandTypes(), op.getWitness());
    newOp.getDoRegion().takeBody(op.getDoRegion());

    // Use the new results to replace the previously used ones.
    SmallVector<Value, 4> replacementValues;
    auto src = newOp.getResults().begin();
    for (auto it : op.getResults()) {
      if (it.getUses().empty())
        replacementValues.push_back(nullptr);
      else
        replacementValues.push_back(*src++);
    }
    rewriter.replaceOp(op, replacementValues);
    return success();
  }
};
} // namespace

void AssumingOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                             MLIRContext *context) {
  patterns.add<AssumingOpRemoveUnusedResults, AssumingWithTrue>(context);
}

// See RegionBranchOpInterface in Interfaces/ControlFlowInterfaces.td
void AssumingOp::getSuccessorRegions(
    Optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  // AssumingOp has unconditional control flow into the region and back to the
  // parent, so return the correct RegionSuccessor purely based on the index
  // being None or 0.
  if (index) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }

  regions.push_back(RegionSuccessor(&getDoRegion()));
}

void AssumingOp::inlineRegionIntoParent(AssumingOp &op,
                                        PatternRewriter &rewriter) {
  auto *blockBeforeAssuming = rewriter.getInsertionBlock();
  auto *assumingBlock = op.getBody();
  auto initPosition = rewriter.getInsertionPoint();
  auto *blockAfterAssuming =
      rewriter.splitBlock(blockBeforeAssuming, initPosition);

  // Remove the AssumingOp and AssumingYieldOp.
  auto &yieldOp = assumingBlock->back();
  rewriter.inlineRegionBefore(op.getDoRegion(), blockAfterAssuming);
  rewriter.replaceOp(op, yieldOp.getOperands());
  rewriter.eraseOp(&yieldOp);

  // Merge blocks together as there was no branching behavior from the
  // AssumingOp.
  rewriter.mergeBlocks(assumingBlock, blockBeforeAssuming);
  rewriter.mergeBlocks(blockAfterAssuming, blockBeforeAssuming);
}

void AssumingOp::build(
    OpBuilder &builder, OperationState &result, Value witness,
    function_ref<SmallVector<Value, 2>(OpBuilder &, Location)> bodyBuilder) {

  result.addOperands(witness);
  Region *bodyRegion = result.addRegion();
  bodyRegion->push_back(new Block);
  Block &bodyBlock = bodyRegion->front();

  // Build body.
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(&bodyBlock);
  SmallVector<Value, 2> yieldValues = bodyBuilder(builder, result.location);
  builder.create<AssumingYieldOp>(result.location, yieldValues);

  SmallVector<Type, 2> assumingTypes;
  for (Value v : yieldValues)
    assumingTypes.push_back(v.getType());
  result.addTypes(assumingTypes);
}

//===----------------------------------------------------------------------===//
// AddOp
//===----------------------------------------------------------------------===//

LogicalResult mlir::shape::AddOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  if (operands[0].getType().isa<SizeType>() ||
      operands[1].getType().isa<SizeType>())
    inferredReturnTypes.assign({SizeType::get(context)});
  else
    inferredReturnTypes.assign({IndexType::get(context)});
  return success();
}

bool mlir::shape::AddOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  // SizeType is compatible with IndexType.
  return eachHasOnlyOneOfTypes<SizeType, IndexType>(l, r);
}

OpFoldResult mlir::shape::AddOp::fold(ArrayRef<Attribute> operands) {
  // add(x, 0) -> x
  if (matchPattern(getRhs(), m_Zero()))
    return getLhs();

  return constFoldBinaryOp<IntegerAttr>(
      operands, [](APInt a, const APInt &b) { return std::move(a) + b; });
}

LogicalResult shape::AddOp::verify() { return verifySizeOrIndexOp(*this); }

//===----------------------------------------------------------------------===//
// AssumingAllOp
//===----------------------------------------------------------------------===//

namespace {

// Merge multiple `shape.assuming_all` operations together.
//
//   %0 = shape.assuming_all %w0, %w1
//   %1 = shape.assuming_all %w2, %0
//
// to:
//
//   %0 = shape.assuming_all %w0, %w2, %w2
struct MergeAssumingAllOps : public OpRewritePattern<AssumingAllOp> {
  using OpRewritePattern<AssumingAllOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AssumingAllOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value> operands;

    for (Value operand : op.getInputs()) {
      if (auto assumeAll = operand.getDefiningOp<AssumingAllOp>())
        operands.append(assumeAll.operand_begin(), assumeAll->operand_end());
      else
        operands.push_back(operand);
    }

    // We didn't find any other `assuming_all` ops to merge with.
    if (operands.size() == op.getNumOperands())
      return failure();

    // Replace with a new `assuming_all` operation with merged constraints.
    rewriter.replaceOpWithNewOp<AssumingAllOp>(op, operands);
    return success();
  }
};

// Eliminate `cstr_broadcastable` operands from `assuming_all` operation that
// are subsumed by others.
//
//   %0 = shape.cstr_broadcastable %shape0, %shape1
//   %1 = shape.cstr_broadcastable %shape0, %shape1, %shape2
//
//   %2 = shape.cstr_broadcastable %shape3, %shape4
//   %3 = shape.cstr_broadcastable %shape3, %shape4, %shape5
//
//   %4 = shape.assuming_all %0, %1, %2, %3
//
// to:
//
//   %0 = shape.cstr_broadcastable %shape0, %shape1, %shape2
//   %1 = shape.cstr_broadcastable %shape3, %shape4, %shape5
//   %2 = shape.assuming_all %0, %1
//
// In this example if shapes [0, 1, 2] are broadcastable, then it means that
// shapes [0, 1] are broadcastable too, and can be removed from the list of
// constraints. If shapes [0, 1, 2] are not broadcastable, then it doesn't
// matter if shapes [0, 1] are broadcastable (same for shapes [3, 4, 5]).
struct AssumingAllOfCstrBroadcastable : public OpRewritePattern<AssumingAllOp> {
  using OpRewritePattern<AssumingAllOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AssumingAllOp op,
                                PatternRewriter &rewriter) const override {
    // Collect all `CstrBroadcastableOp` operands first.
    SetVector<CstrBroadcastableOp> operands;
    for (Value operand : op.getInputs()) {
      // TODO: Apply this optimization if some of the witnesses are not
      // produced by the `cstr_broadcastable`.
      auto broadcastable = operand.getDefiningOp<CstrBroadcastableOp>();
      if (!broadcastable)
        return failure();

      operands.insert(broadcastable);
    }

    // Skip trivial `assuming_all` operations.
    if (operands.size() <= 1)
      return failure();

    // Collect shapes checked by `cstr_broadcastable` operands.
    SmallVector<std::pair<CstrBroadcastableOp, DenseSet<Value>>> shapes;
    for (auto cstr : operands) {
      DenseSet<Value> shapesSet(cstr->operand_begin(), cstr->operand_end());
      shapes.emplace_back(cstr, std::move(shapesSet));
    }

    // Sort by the number of shape operands (larger to smaller).
    llvm::sort(shapes, [](auto a, auto b) {
      return a.first.getNumOperands() > b.first.getNumOperands();
    });

    // We start from the `cst_broadcastable` operations with largest number of
    // shape operands, and remove redundant `cst_broadcastable` operations. We
    // do this until we find a set of `cst_broadcastable` operations with
    // non-overlapping constraints.
    SmallVector<CstrBroadcastableOp> markedForErase;

    for (unsigned i = 0; i < shapes.size(); ++i) {
      auto isSubset = [&](auto pair) {
        return llvm::set_is_subset(pair.second, shapes[i].second);
      };

      // Keep redundant `cstr_broadcastable` operations to be erased.
      auto *it = std::remove_if(shapes.begin() + i + 1, shapes.end(), isSubset);
      for (auto *it0 = it; it0 < shapes.end(); ++it0)
        markedForErase.push_back(it0->first);
      shapes.erase(it, shapes.end());
    }

    // We didn't find any operands that could be removed.
    if (markedForErase.empty())
      return failure();

    // Collect non-overlapping `cst_broadcastable` constraints.
    SmallVector<Value> uniqueConstraints;
    for (auto &shape : shapes)
      uniqueConstraints.push_back(shape.first.getResult());

    // Replace with a new `assuming_all` operation ...
    rewriter.replaceOpWithNewOp<AssumingAllOp>(op, uniqueConstraints);

    // ... and maybe erase `cstr_broadcastable` ops without uses.
    for (auto &op : markedForErase)
      if (op->use_empty())
        rewriter.eraseOp(op);

    return success();
  }
};

struct AssumingAllToCstrEqCanonicalization
    : public OpRewritePattern<AssumingAllOp> {
  using OpRewritePattern<AssumingAllOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AssumingAllOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value, 8> shapes;
    for (Value w : op.getInputs()) {
      auto cstrEqOp = w.getDefiningOp<CstrEqOp>();
      if (!cstrEqOp)
        return failure();
      bool disjointShapes = llvm::none_of(cstrEqOp.getShapes(), [&](Value s) {
        return llvm::is_contained(shapes, s);
      });
      if (!shapes.empty() && !cstrEqOp.getShapes().empty() && disjointShapes)
        return failure();
      shapes.append(cstrEqOp.getShapes().begin(), cstrEqOp.getShapes().end());
    }
    rewriter.replaceOpWithNewOp<CstrEqOp>(op, shapes);
    return success();
  }
};

template <typename OpTy>
struct RemoveDuplicateOperandsPattern : public OpRewritePattern<OpTy> {
  using OpRewritePattern<OpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(OpTy op,
                                PatternRewriter &rewriter) const override {
    // Find unique operands.
    SetVector<Value> unique(op.operand_begin(), op.operand_end());

    // Reduce op to equivalent with unique operands.
    if (unique.size() < op.getNumOperands()) {
      rewriter.replaceOpWithNewOp<OpTy>(op, op->getResultTypes(),
                                        unique.takeVector(), op->getAttrs());
      return success();
    }

    return failure();
  }
};
} // namespace

void AssumingAllOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                                MLIRContext *context) {
  patterns
      .add<MergeAssumingAllOps, AssumingAllOneOp,
           AssumingAllOfCstrBroadcastable, AssumingAllToCstrEqCanonicalization,
           RemoveDuplicateOperandsPattern<AssumingAllOp>>(context);
}

OpFoldResult AssumingAllOp::fold(ArrayRef<Attribute> operands) {
  // Iterate in reverse to first handle all constant operands. They are
  // guaranteed to be the tail of the inputs because this is commutative.
  for (int idx = operands.size() - 1; idx >= 0; idx--) {
    Attribute a = operands[idx];
    // Cannot fold if any inputs are not constant;
    if (!a)
      return nullptr;

    // We do not need to keep statically known values after handling them in
    // this method.
    getOperation()->eraseOperand(idx);

    // Always false if any input is statically known false
    if (!a.cast<BoolAttr>().getValue())
      return a;
  }
  // If this is reached, all inputs were statically known passing.
  return BoolAttr::get(getContext(), true);
}

LogicalResult AssumingAllOp::verify() {
  // Ensure that AssumingAllOp contains at least one operand
  if (getNumOperands() == 0)
    return emitOpError("no operands specified");

  return success();
}

//===----------------------------------------------------------------------===//
// BroadcastOp
//===----------------------------------------------------------------------===//

OpFoldResult BroadcastOp::fold(ArrayRef<Attribute> operands) {
  if (getShapes().size() == 1) {
    // Otherwise, we need a cast which would be a canonicalization, not folding.
    if (getShapes().front().getType() != getType())
      return nullptr;
    return getShapes().front();
  }

  // TODO: Support folding with more than 2 input shapes
  if (getShapes().size() > 2)
    return nullptr;

  if (!operands[0] || !operands[1])
    return nullptr;
  auto lhsShape = llvm::to_vector<6>(
      operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
  auto rhsShape = llvm::to_vector<6>(
      operands[1].cast<DenseIntElementsAttr>().getValues<int64_t>());
  SmallVector<int64_t, 6> resultShape;

  // If the shapes are not compatible, we can't fold it.
  // TODO: Fold to an "error".
  if (!OpTrait::util::getBroadcastedShape(lhsShape, rhsShape, resultShape))
    return nullptr;

  Builder builder(getContext());
  return builder.getIndexTensorAttr(resultShape);
}

LogicalResult BroadcastOp::verify() {
  return verifyShapeOrExtentTensorOp(*this);
}

namespace {
template <typename OpTy>
struct RemoveEmptyShapeOperandsPattern : public OpRewritePattern<OpTy> {
  using OpRewritePattern<OpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(OpTy op,
                                PatternRewriter &rewriter) const override {
    auto isPotentiallyNonEmptyShape = [](Value shape) {
      if (auto extentTensorTy = shape.getType().dyn_cast<RankedTensorType>()) {
        if (extentTensorTy.getDimSize(0) == 0)
          return false;
      }
      if (auto constShape = shape.getDefiningOp<ConstShapeOp>()) {
        if (constShape.getShape().empty())
          return false;
      }
      return true;
    };
    auto newOperands = llvm::to_vector<8>(
        llvm::make_filter_range(op->getOperands(), isPotentiallyNonEmptyShape));

    // Reduce op to equivalent without empty shape operands.
    if (newOperands.size() < op.getNumOperands()) {
      rewriter.replaceOpWithNewOp<OpTy>(op, op->getResultTypes(), newOperands,
                                        op->getAttrs());
      return success();
    }

    return failure();
  }
};

struct BroadcastForwardSingleOperandPattern
    : public OpRewritePattern<BroadcastOp> {
  using OpRewritePattern<BroadcastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(BroadcastOp op,
                                PatternRewriter &rewriter) const override {
    if (op.getNumOperands() != 1)
      return failure();
    Value replacement = op.getShapes().front();

    // Insert cast if needed.
    if (replacement.getType() != op.getType()) {
      auto loc = op.getLoc();
      if (op.getType().isa<ShapeType>()) {
        replacement = rewriter.create<FromExtentTensorOp>(loc, replacement);
      } else {
        assert(!op.getType().isa<ShapeType>() &&
               !replacement.getType().isa<ShapeType>() &&
               "expect extent tensor cast");
        replacement =
            rewriter.create<tensor::CastOp>(loc, op.getType(), replacement);
      }
    }

    rewriter.replaceOp(op, replacement);
    return success();
  }
};

struct BroadcastFoldConstantOperandsPattern
    : public OpRewritePattern<BroadcastOp> {
  using OpRewritePattern<BroadcastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(BroadcastOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<int64_t, 8> foldedConstantShape;
    SmallVector<Value, 8> newShapeOperands;
    for (Value shape : op.getShapes()) {
      if (auto constShape = shape.getDefiningOp<ConstShapeOp>()) {
        SmallVector<int64_t, 8> newFoldedConstantShape;
        if (OpTrait::util::getBroadcastedShape(
                foldedConstantShape,
                llvm::to_vector<8>(constShape.getShape().getValues<int64_t>()),
                newFoldedConstantShape)) {
          foldedConstantShape = newFoldedConstantShape;
          continue;
        }
      }
      newShapeOperands.push_back(shape);
    }

    // Need at least two constant operands to fold anything.
    if (op.getNumOperands() - newShapeOperands.size() < 2)
      return failure();

    auto foldedConstantOperandsTy = RankedTensorType::get(
        {static_cast<int64_t>(foldedConstantShape.size())},
        rewriter.getIndexType());
    newShapeOperands.push_back(rewriter.create<ConstShapeOp>(
        op.getLoc(), foldedConstantOperandsTy,
        rewriter.getIndexTensorAttr(foldedConstantShape)));
    rewriter.replaceOpWithNewOp<BroadcastOp>(op, op.getType(),
                                             newShapeOperands);
    return success();
  }
};

template <typename OpTy>
struct CanonicalizeCastExtentTensorOperandsPattern
    : public OpRewritePattern<OpTy> {
  using OpRewritePattern<OpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(OpTy op,
                                PatternRewriter &rewriter) const override {
    // Canonicalize operands.
    bool anyChange = false;
    auto canonicalizeOperand = [&](Value operand) {
      if (auto castOp = operand.getDefiningOp<tensor::CastOp>()) {
        // Only eliminate the cast if it holds no shape information.
        bool isInformationLoosingCast =
            castOp.getType().cast<RankedTensorType>().isDynamicDim(0);
        if (isInformationLoosingCast) {
          anyChange = true;
          return castOp.getSource();
        }
      }
      return operand;
    };
    auto newOperands = llvm::to_vector<8>(
        llvm::map_range(op.getOperands(), canonicalizeOperand));

    // Rewrite op if any change required.
    if (!anyChange)
      return failure();
    rewriter.replaceOpWithNewOp<OpTy>(op, op->getResultTypes(), newOperands);
    return success();
  }
};

struct BroadcastConcretizeResultTypePattern
    : public OpRewritePattern<BroadcastOp> {
  using OpRewritePattern<BroadcastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(BroadcastOp op,
                                PatternRewriter &rewriter) const override {
    // Only concretize dynamic extent tensor result types.
    auto resultTy = op.getType().dyn_cast<RankedTensorType>();
    if (!resultTy || !resultTy.isDynamicDim(0))
      return failure();

    // Infer resulting shape rank if possible.
    int64_t maxRank = 0;
    for (Value shape : op.getShapes()) {
      if (auto extentTensorTy = shape.getType().dyn_cast<RankedTensorType>()) {
        // Cannot infer resulting shape rank if any operand is dynamically
        // ranked.
        if (extentTensorTy.isDynamicDim(0))
          return failure();
        maxRank = std::max(maxRank, extentTensorTy.getDimSize(0));
      }
    }

    auto newOp = rewriter.create<BroadcastOp>(
        op.getLoc(), getExtentTensorType(getContext(), maxRank),
        op.getShapes());
    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, op.getType(), newOp);
    return success();
  }
};
} // namespace

void BroadcastOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                              MLIRContext *context) {
  patterns.add<BroadcastConcretizeResultTypePattern,
               BroadcastFoldConstantOperandsPattern,
               BroadcastForwardSingleOperandPattern,
               CanonicalizeCastExtentTensorOperandsPattern<BroadcastOp>,
               RemoveDuplicateOperandsPattern<BroadcastOp>,
               RemoveEmptyShapeOperandsPattern<BroadcastOp>>(context);
}

//===----------------------------------------------------------------------===//
// ConcatOp
//===----------------------------------------------------------------------===//

OpFoldResult ConcatOp::fold(ArrayRef<Attribute> operands) {
  if (!operands[0] || !operands[1])
    return nullptr;
  auto lhsShape = llvm::to_vector<6>(
      operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
  auto rhsShape = llvm::to_vector<6>(
      operands[1].cast<DenseIntElementsAttr>().getValues<int64_t>());
  SmallVector<int64_t, 6> resultShape;
  resultShape.append(lhsShape.begin(), lhsShape.end());
  resultShape.append(rhsShape.begin(), rhsShape.end());
  Builder builder(getContext());
  return builder.getIndexTensorAttr(resultShape);
}

//===----------------------------------------------------------------------===//
// ConstShapeOp
//===----------------------------------------------------------------------===//

void ConstShapeOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printOptionalAttrDict((*this)->getAttrs(), /*elidedAttrs=*/{"shape"});
  p << "[";
  interleaveComma(getShape().getValues<int64_t>(), p);
  p << "] : ";
  p.printType(getType());
}

ParseResult ConstShapeOp::parse(OpAsmParser &parser, OperationState &result) {
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  // We piggy-back on ArrayAttr parsing, though we don't internally store the
  // shape as an ArrayAttr.
  // TODO: Implement custom parser and maybe make syntax a bit more concise.
  Attribute extentsRaw;
  NamedAttrList dummy;
  if (parser.parseAttribute(extentsRaw, "dummy", dummy))
    return failure();
  auto extentsArray = extentsRaw.dyn_cast<ArrayAttr>();
  if (!extentsArray)
    return failure();
  SmallVector<int64_t, 6> ints;
  for (Attribute extent : extentsArray) {
    IntegerAttr attr = extent.dyn_cast<IntegerAttr>();
    if (!attr)
      return failure();
    ints.push_back(attr.getInt());
  }
  Builder &builder = parser.getBuilder();
  result.addAttribute("shape", builder.getIndexTensorAttr(ints));
  Type resultTy;
  if (parser.parseColonType(resultTy))
    return failure();
  result.types.push_back(resultTy);
  return success();
}

OpFoldResult ConstShapeOp::fold(ArrayRef<Attribute>) { return getShapeAttr(); }

void ConstShapeOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                               MLIRContext *context) {
  patterns.add<TensorCastConstShape>(context);
}

LogicalResult mlir::shape::ConstShapeOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  Builder b(context);
  auto shape = attributes.getAs<DenseIntElementsAttr>("shape");
  if (!shape)
    return emitOptionalError(location, "missing shape attribute");
  inferredReturnTypes.assign({RankedTensorType::get(
      {static_cast<int64_t>(shape.size())}, b.getIndexType())});
  return success();
}

bool mlir::shape::ConstShapeOp::isCompatibleReturnTypes(TypeRange l,
                                                        TypeRange r) {
  if (l.size() != 1 || r.size() != 1)
    return false;

  Type lhs = l.front();
  Type rhs = r.front();

  if (lhs.isa<ShapeType>() || rhs.isa<ShapeType>())
    // Shape type is compatible with all other valid return types.
    return true;
  return lhs == rhs;
}

//===----------------------------------------------------------------------===//
// CstrBroadcastableOp
//===----------------------------------------------------------------------===//

void CstrBroadcastableOp::getCanonicalizationPatterns(
    RewritePatternSet &patterns, MLIRContext *context) {
  // Canonicalization patterns have overlap with the considerations during
  // folding in case additional shape information is inferred at some point that
  // does not result in folding.
  patterns.add<CanonicalizeCastExtentTensorOperandsPattern<CstrBroadcastableOp>,
               CstrBroadcastableEqOps,
               RemoveDuplicateOperandsPattern<CstrBroadcastableOp>,
               RemoveEmptyShapeOperandsPattern<CstrBroadcastableOp>>(context);
}

// Return true if there is exactly one attribute not representing a scalar
// broadcast.
static bool hasAtMostSingleNonScalar(ArrayRef<Attribute> attributes) {
  bool nonScalarSeen = false;
  for (Attribute a : attributes) {
    if (!a || a.cast<DenseIntElementsAttr>().getNumElements() != 0) {
      if (nonScalarSeen)
        return false;
      nonScalarSeen = true;
    }
  }
  return true;
}

OpFoldResult CstrBroadcastableOp::fold(ArrayRef<Attribute> operands) {
  // No broadcasting is needed if all operands but one are scalar.
  if (hasAtMostSingleNonScalar(operands))
    return BoolAttr::get(getContext(), true);

  if ([&] {
        SmallVector<SmallVector<int64_t, 6>, 6> extents;
        for (const auto &operand : operands) {
          if (!operand)
            return false;
          extents.push_back(llvm::to_vector<6>(
              operand.cast<DenseIntElementsAttr>().getValues<int64_t>()));
        }
        return OpTrait::util::staticallyKnownBroadcastable(extents);
      }())
    return BoolAttr::get(getContext(), true);

  // Lastly, see if folding can be completed based on what constraints are known
  // on the input shapes.
  if ([&] {
        SmallVector<SmallVector<int64_t, 6>, 6> extents;
        for (auto shapeValue : getShapes()) {
          extents.emplace_back();
          if (failed(getShapeVec(shapeValue, extents.back())))
            return false;
        }
        return OpTrait::util::staticallyKnownBroadcastable(extents);
      }())
    return BoolAttr::get(getContext(), true);

  // Because a failing witness result here represents an eventual assertion
  // failure, we do not replace it with a constant witness.
  return nullptr;
}

LogicalResult CstrBroadcastableOp::verify() {
  // Ensure that CstrBroadcastableOp contains at least two operands
  if (getNumOperands() < 2)
    return emitOpError("required at least 2 input shapes");
  return success();
}

//===----------------------------------------------------------------------===//
// CstrEqOp
//===----------------------------------------------------------------------===//

void CstrEqOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                           MLIRContext *context) {
  // If inputs are equal, return passing witness
  patterns.add<CstrEqEqOps>(context);
}

OpFoldResult CstrEqOp::fold(ArrayRef<Attribute> operands) {
  if (llvm::all_of(operands,
                   [&](Attribute a) { return a && a == operands[0]; }))
    return BoolAttr::get(getContext(), true);

  // Because a failing witness result here represents an eventual assertion
  // failure, we do not try to replace it with a constant witness. Similarly, we
  // cannot if there are any non-const inputs.
  return nullptr;
}

//===----------------------------------------------------------------------===//
// ConstSizeOp
//===----------------------------------------------------------------------===//

void ConstSizeOp::build(OpBuilder &builder, OperationState &result,
                        int64_t value) {
  build(builder, result, builder.getIndexAttr(value));
}

OpFoldResult ConstSizeOp::fold(ArrayRef<Attribute>) { return getValueAttr(); }

void ConstSizeOp::getAsmResultNames(
    llvm::function_ref<void(Value, StringRef)> setNameFn) {
  SmallString<4> buffer;
  llvm::raw_svector_ostream os(buffer);
  os << "c" << getValue();
  setNameFn(getResult(), os.str());
}

//===----------------------------------------------------------------------===//
// ConstWitnessOp
//===----------------------------------------------------------------------===//

OpFoldResult ConstWitnessOp::fold(ArrayRef<Attribute>) {
  return getPassingAttr();
}

//===----------------------------------------------------------------------===//
// CstrRequireOp
//===----------------------------------------------------------------------===//

OpFoldResult CstrRequireOp::fold(ArrayRef<Attribute> operands) {
  return operands[0];
}

//===----------------------------------------------------------------------===//
// DivOp
//===----------------------------------------------------------------------===//

OpFoldResult DivOp::fold(ArrayRef<Attribute> operands) {
  auto lhs = operands[0].dyn_cast_or_null<IntegerAttr>();
  if (!lhs)
    return nullptr;
  auto rhs = operands[1].dyn_cast_or_null<IntegerAttr>();
  if (!rhs)
    return nullptr;

  // Division in APInt does not follow floor(lhs, rhs) when the result is
  // negative. Rather, APInt rounds toward zero.
  APInt quotient, remainder;
  APInt::sdivrem(lhs.getValue(), rhs.getValue(), quotient, remainder);
  if (quotient.isNegative() && !remainder.isNullValue()) {
    quotient -= 1;
  }

  Type indexTy = IndexType::get(getContext());
  return IntegerAttr::get(indexTy, quotient);
}

LogicalResult mlir::shape::DivOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  if (operands[0].getType().isa<SizeType>() ||
      operands[1].getType().isa<SizeType>())
    inferredReturnTypes.assign({SizeType::get(context)});
  else
    inferredReturnTypes.assign({IndexType::get(context)});
  return success();
}

bool mlir::shape::DivOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  // SizeType is compatible with IndexType.
  return eachHasOnlyOneOfTypes<SizeType, IndexType>(l, r);
}

LogicalResult DivOp::verify() { return verifySizeOrIndexOp(*this); }

//===----------------------------------------------------------------------===//
// ShapeEqOp
//===----------------------------------------------------------------------===//

OpFoldResult ShapeEqOp::fold(ArrayRef<Attribute> operands) {
  bool allSame = true;
  if (!operands.empty() && !operands[0])
    return {};
  for (Attribute operand : operands.drop_front(1)) {
    if (!operand)
      return {};
    allSame = allSame && operand == operands[0];
  }
  return BoolAttr::get(getContext(), allSame);
}

//===----------------------------------------------------------------------===//
// IndexToSizeOp
//===----------------------------------------------------------------------===//

OpFoldResult IndexToSizeOp::fold(ArrayRef<Attribute> operands) {
  // Constant values of both types, `shape.size` and `index`, are represented as
  // `IntegerAttr`s which makes constant folding simple.
  if (Attribute arg = operands[0])
    return arg;
  return {};
}

void IndexToSizeOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                                MLIRContext *context) {
  patterns.add<SizeToIndexToSizeCanonicalization>(context);
}

//===----------------------------------------------------------------------===//
// FromExtentsOp
//===----------------------------------------------------------------------===//

OpFoldResult FromExtentsOp::fold(ArrayRef<Attribute> operands) {
  if (llvm::any_of(operands, [](Attribute a) { return !a; }))
    return nullptr;
  SmallVector<int64_t, 6> extents;
  for (auto attr : operands)
    extents.push_back(attr.cast<IntegerAttr>().getInt());
  Builder builder(getContext());
  return builder.getIndexTensorAttr(extents);
}

//===----------------------------------------------------------------------===//
// FunctionLibraryOp
//===----------------------------------------------------------------------===//

void FunctionLibraryOp::build(OpBuilder &builder, OperationState &result,
                              StringRef name) {
  result.attributes.push_back(builder.getNamedAttr(
      ::mlir::SymbolTable::getSymbolAttrName(), builder.getStringAttr(name)));
}

FuncOp FunctionLibraryOp::getShapeFunction(Operation *op) {
  auto attr = getMapping()
                  .get(op->getName().getIdentifier())
                  .dyn_cast_or_null<FlatSymbolRefAttr>();
  if (!attr)
    return nullptr;
  return lookupSymbol<FuncOp>(attr);
}

ParseResult FunctionLibraryOp::parse(OpAsmParser &parser,
                                     OperationState &result) {
  // Parse the op name.
  StringAttr nameAttr;
  if (parser.parseSymbolName(nameAttr, ::mlir::SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  if (parser.parseOptionalAttrDictWithKeyword(result.attributes))
    return failure();

  auto *bodyRegion = result.addRegion();
  if (parser.parseRegion(*bodyRegion))
    return failure();

  if (parser.parseKeyword("mapping"))
    return failure();

  DictionaryAttr mappingAttr;
  if (parser.parseAttribute(mappingAttr,
                            parser.getBuilder().getType<NoneType>(), "mapping",
                            result.attributes))
    return failure();
  return success();
}

void FunctionLibraryOp::print(OpAsmPrinter &p) {
  p << ' ';
  p.printSymbolName(getName());
  p.printOptionalAttrDictWithKeyword(
      (*this)->getAttrs(), {mlir::SymbolTable::getSymbolAttrName(), "mapping"});
  p << ' ';
  p.printRegion(getRegion(), /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/false);
  p << " mapping ";
  p.printAttributeWithoutType(getMappingAttr());
}

//===----------------------------------------------------------------------===//
// FuncOp
//===----------------------------------------------------------------------===//

ParseResult FuncOp::parse(OpAsmParser &parser, OperationState &result) {
  auto buildFuncType =
      [](Builder &builder, ArrayRef<Type> argTypes, ArrayRef<Type> results,
         function_interface_impl::VariadicFlag,
         std::string &) { return builder.getFunctionType(argTypes, results); };

  return function_interface_impl::parseFunctionOp(
      parser, result, /*allowVariadic=*/false, buildFuncType);
}

void FuncOp::print(OpAsmPrinter &p) {
  function_interface_impl::printFunctionOp(p, *this, /*isVariadic=*/false);
}

//===----------------------------------------------------------------------===//
// GetExtentOp
//===----------------------------------------------------------------------===//

Optional<int64_t> GetExtentOp::getConstantDim() {
  if (auto constSizeOp = getDim().getDefiningOp<ConstSizeOp>())
    return constSizeOp.getValue().getLimitedValue();
  if (auto constantOp = getDim().getDefiningOp<arith::ConstantOp>())
    return constantOp.getValue().cast<IntegerAttr>().getInt();
  return llvm::None;
}

OpFoldResult GetExtentOp::fold(ArrayRef<Attribute> operands) {
  auto elements = operands[0].dyn_cast_or_null<DenseIntElementsAttr>();
  if (!elements)
    return nullptr;
  Optional<int64_t> dim = getConstantDim();
  if (!dim.has_value())
    return nullptr;
  if (dim.value() >= elements.getNumElements())
    return nullptr;
  return elements.getValues<Attribute>()[(uint64_t)dim.value()];
}

void GetExtentOp::build(OpBuilder &builder, OperationState &result, Value shape,
                        int64_t dim) {
  auto loc = result.location;
  auto dimAttr = builder.getIndexAttr(dim);
  if (shape.getType().isa<ShapeType>()) {
    Value dim = builder.create<ConstSizeOp>(loc, dimAttr);
    build(builder, result, builder.getType<SizeType>(), shape, dim);
  } else {
    Value dim =
        builder.create<arith::ConstantOp>(loc, builder.getIndexType(), dimAttr);
    build(builder, result, builder.getIndexType(), shape, dim);
  }
}

LogicalResult mlir::shape::GetExtentOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  inferredReturnTypes.assign({IndexType::get(context)});
  return success();
}

bool mlir::shape::GetExtentOp::isCompatibleReturnTypes(TypeRange l,
                                                       TypeRange r) {
  // SizeType is compatible with IndexType.
  return eachHasOnlyOneOfTypes<SizeType, IndexType>(l, r);
}

LogicalResult GetExtentOp::verify() { return verifySizeOrIndexOp(*this); }

//===----------------------------------------------------------------------===//
// IsBroadcastableOp
//===----------------------------------------------------------------------===//

void IsBroadcastableOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                                    MLIRContext *context) {
  patterns.add<RemoveDuplicateOperandsPattern<IsBroadcastableOp>>(context);
}

OpFoldResult IsBroadcastableOp::fold(ArrayRef<Attribute> operands) {
  // Can always broadcast fewer than two shapes.
  if (operands.size() < 2) {
    return BoolAttr::get(getContext(), true);
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// MeetOp
//===----------------------------------------------------------------------===//

LogicalResult mlir::shape::MeetOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  inferredReturnTypes.assign({operands[0].getType()});
  return success();
}

bool mlir::shape::MeetOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  if (l.size() != 1 || r.size() != 1)
    return false;
  if (l == r)
    return true;

  Type lhs = l.front();
  Type rhs = r.front();

  if (lhs != rhs)
    return false;

  if (lhs.isa<SizeType>() || lhs.isa<ShapeType>())
    return true;

  if (succeeded(verifyCompatibleShapes({lhs, rhs})))
    return true;
  return false;
}

//===----------------------------------------------------------------------===//
// RankOp
//===----------------------------------------------------------------------===//

OpFoldResult shape::RankOp::fold(ArrayRef<Attribute> operands) {
  auto shape = operands[0].dyn_cast_or_null<DenseIntElementsAttr>();
  if (!shape)
    return {};
  int64_t rank = shape.getNumElements();
  Builder builder(getContext());
  return builder.getIndexAttr(rank);
}

/// Evaluate the `rank` operation for shapes of ranked tensors at compile time.
/// Constant folding fails in cases where only the rank is constant, not the
/// shape itself.
/// This canonicalization matches `shape.rank(shape.shape_of(%ranked_tensor))`.
///
/// Example:
///
/// %shape = shape.shape_of %ranked_tensor : tensor<1x2x?xf32>
/// %rank = shape.rank %shape
///
/// becomes
///
/// %rank = shape.const_size 3

namespace {
struct RankShapeOfCanonicalizationPattern
    : public OpRewritePattern<shape::RankOp> {
  using OpRewritePattern<shape::RankOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(shape::RankOp op,
                                PatternRewriter &rewriter) const override {
    auto shapeOfOp = op.getShape().getDefiningOp<ShapeOfOp>();
    if (!shapeOfOp)
      return failure();
    auto rankedTensorType =
        shapeOfOp.getArg().getType().dyn_cast<RankedTensorType>();
    if (!rankedTensorType)
      return failure();
    int64_t rank = rankedTensorType.getRank();
    if (op.getType().isa<IndexType>()) {
      rewriter.replaceOpWithNewOp<arith::ConstantIndexOp>(op.getOperation(),
                                                          rank);
    } else if (op.getType().isa<shape::SizeType>()) {
      rewriter.replaceOpWithNewOp<shape::ConstSizeOp>(op.getOperation(), rank);
    } else {
      return failure();
    }
    return success();
  }
};
} // namespace

void shape::RankOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                                MLIRContext *context) {
  patterns.add<RankShapeOfCanonicalizationPattern>(context);
}

LogicalResult mlir::shape::RankOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  if (operands[0].getType().isa<ShapeType>())
    inferredReturnTypes.assign({SizeType::get(context)});
  else
    inferredReturnTypes.assign({IndexType::get(context)});
  return success();
}

bool mlir::shape::RankOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  // SizeType is compatible with IndexType.
  return eachHasOnlyOneOfTypes<SizeType, IndexType>(l, r);
}

LogicalResult shape::RankOp::verify() { return verifySizeOrIndexOp(*this); }

//===----------------------------------------------------------------------===//
// NumElementsOp
//===----------------------------------------------------------------------===//

OpFoldResult NumElementsOp::fold(ArrayRef<Attribute> operands) {

  // Fold only when argument constant.
  Attribute shape = operands[0];
  if (!shape)
    return {};

  APInt product(64, 1);
  for (auto value : shape.cast<DenseIntElementsAttr>())
    product *= value;
  Builder builder(getContext());
  return builder.getIndexAttr(product.getLimitedValue());
}

LogicalResult mlir::shape::NumElementsOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  if (operands[0].getType().isa<ShapeType>())
    inferredReturnTypes.assign({SizeType::get(context)});
  else
    inferredReturnTypes.assign({IndexType::get(context)});
  return success();
}

bool mlir::shape::NumElementsOp::isCompatibleReturnTypes(TypeRange l,
                                                         TypeRange r) {
  // SizeType is compatible with IndexType.
  return eachHasOnlyOneOfTypes<SizeType, IndexType>(l, r);
}

LogicalResult shape::NumElementsOp::verify() {
  return verifySizeOrIndexOp(*this);
}

//===----------------------------------------------------------------------===//
// MaxOp
//===----------------------------------------------------------------------===//

OpFoldResult MaxOp::fold(llvm::ArrayRef<mlir::Attribute> operands) {
  // If operands are equal, just propagate one.
  if (getLhs() == getRhs())
    return getLhs();
  return nullptr;
}

LogicalResult mlir::shape::MaxOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  if (operands[0].getType() == operands[1].getType())
    inferredReturnTypes.assign({operands[0].getType()});
  else
    inferredReturnTypes.assign({SizeType::get(context)});
  return success();
}

bool mlir::shape::MaxOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  if (l.size() != 1 || r.size() != 1)
    return false;
  if (l.front().isa<ShapeType>() && r.front().isa<ShapeType>())
    return true;
  if (l.front().isa<SizeType>() && r.front().isa<SizeType>())
    return true;
  return false;
}

//===----------------------------------------------------------------------===//
// MinOp
//===----------------------------------------------------------------------===//

OpFoldResult MinOp::fold(llvm::ArrayRef<mlir::Attribute> operands) {
  // If operands are equal, just propagate one.
  if (getLhs() == getRhs())
    return getLhs();
  return nullptr;
}

LogicalResult mlir::shape::MinOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  if (operands[0].getType() == operands[1].getType())
    inferredReturnTypes.assign({operands[0].getType()});
  else
    inferredReturnTypes.assign({SizeType::get(context)});
  return success();
}

bool mlir::shape::MinOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  if (l.size() != 1 || r.size() != 1)
    return false;
  if (l.front().isa<ShapeType>() && r.front().isa<ShapeType>())
    return true;
  if (l.front().isa<SizeType>() && r.front().isa<SizeType>())
    return true;
  return false;
}

//===----------------------------------------------------------------------===//
// MulOp
//===----------------------------------------------------------------------===//

OpFoldResult MulOp::fold(ArrayRef<Attribute> operands) {
  auto lhs = operands[0].dyn_cast_or_null<IntegerAttr>();
  if (!lhs)
    return nullptr;
  auto rhs = operands[1].dyn_cast_or_null<IntegerAttr>();
  if (!rhs)
    return nullptr;
  APInt folded = lhs.getValue() * rhs.getValue();
  Type indexTy = IndexType::get(getContext());
  return IntegerAttr::get(indexTy, folded);
}

LogicalResult mlir::shape::MulOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  if (operands[0].getType().isa<SizeType>() ||
      operands[1].getType().isa<SizeType>())
    inferredReturnTypes.assign({SizeType::get(context)});
  else
    inferredReturnTypes.assign({IndexType::get(context)});
  return success();
}

bool mlir::shape::MulOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  // SizeType is compatible with IndexType.
  return eachHasOnlyOneOfTypes<SizeType, IndexType>(l, r);
}

LogicalResult shape::MulOp::verify() { return verifySizeOrIndexOp(*this); }

//===----------------------------------------------------------------------===//
// ShapeOfOp
//===----------------------------------------------------------------------===//

OpFoldResult ShapeOfOp::fold(ArrayRef<Attribute>) {
  auto type = getOperand().getType().dyn_cast<ShapedType>();
  if (!type || !type.hasStaticShape())
    return nullptr;
  Builder builder(getContext());
  return builder.getIndexTensorAttr(type.getShape());
}

namespace {
struct ShapeOfWithTensor : public OpRewritePattern<shape::ShapeOfOp> {
  using OpRewritePattern<shape::ShapeOfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(shape::ShapeOfOp op,
                                PatternRewriter &rewriter) const override {
    if (!op.getArg().getType().isa<ShapedType>())
      return failure();
    if (op.getType().isa<ShapedType>())
      return failure();

    rewriter.replaceOpWithNewOp<shape::ShapeOfOp>(op.getOperation(),
                                                  op.getArg());
    return success();
  }
};

// Canonicalize
// ```
// %0 = shape.shape_of %arg : tensor<?x?x?xf32> -> tensor<3xindex>
// %1 = tensor.cast %0 : tensor<3xindex> to tensor<?xindex>
// ```
// to
// ```
// %1 = shape.shape_of %arg : tensor<?x?x?xf32> -> tensor<?xindex>
// ```
struct ShapeOfCastExtentTensor : public OpRewritePattern<tensor::CastOp> {
  using OpRewritePattern<tensor::CastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::CastOp op,
                                PatternRewriter &rewriter) const override {
    auto ty = op.getType().dyn_cast<RankedTensorType>();
    if (!ty || ty.getRank() != 1)
      return failure();

    auto shapeOfOp = op.getSource().getDefiningOp<ShapeOfOp>();
    if (!shapeOfOp)
      return failure();

    // Argument type must be ranked and must not conflict.
    auto argTy = shapeOfOp.getArg().getType().dyn_cast<RankedTensorType>();
    if (!argTy || (!ty.isDynamicDim(0) && ty.getDimSize(0) != argTy.getRank()))
      return failure();

    rewriter.replaceOpWithNewOp<ShapeOfOp>(op, ty, shapeOfOp.getArg());
    return success();
  }
};
} // namespace

void ShapeOfOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                            MLIRContext *context) {
  patterns.add<ShapeOfCastExtentTensor, ShapeOfWithTensor,
               ExtractFromShapeOfExtentTensor>(context);
}

LogicalResult mlir::shape::ShapeOfOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  if (operands[0].getType().isa<ValueShapeType>())
    inferredReturnTypes.assign({ShapeType::get(context)});
  else {
    auto shapedTy = operands[0].getType().cast<ShapedType>();
    int64_t rank =
        shapedTy.hasRank() ? shapedTy.getRank() : ShapedType::kDynamicSize;
    Type indexTy = IndexType::get(context);
    Type extentTensorTy = RankedTensorType::get({rank}, indexTy);
    inferredReturnTypes.assign({extentTensorTy});
  }
  return success();
}

bool mlir::shape::ShapeOfOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  if (l.size() != 1 || r.size() != 1)
    return false;
  if (l == r)
    return true;

  Type lhs = l.front();
  Type rhs = r.front();

  if (!lhs.isa<ShapeType, ShapedType>() || !rhs.isa<ShapeType, ShapedType>())
    return false;

  if (lhs.isa<ShapeType>() || rhs.isa<ShapeType>())
    // Shape type is compatible with all other valid return types.
    return true;

  if (succeeded(verifyCompatibleShapes({lhs, rhs})))
    return true;
  return false;
}

LogicalResult shape::ShapeOfOp::verify() {
  return verifyShapeOrExtentTensorOp(*this);
}

//===----------------------------------------------------------------------===//
// SizeToIndexOp
//===----------------------------------------------------------------------===//

OpFoldResult SizeToIndexOp::fold(ArrayRef<Attribute> operands) {
  // Constant values of both types, `shape.size` and `index`, are represented as
  // `IntegerAttr`s which makes constant folding simple.
  if (Attribute arg = operands[0])
    return arg;
  return OpFoldResult();
}

void SizeToIndexOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                                MLIRContext *context) {
  patterns.add<IndexToSizeToIndexCanonicalization>(context);
}

bool SizeToIndexOp::areCastCompatible(TypeRange inputs, TypeRange outputs) {
  if (inputs.size() != 1 || outputs.size() != 1)
    return false;
  return inputs[0].isa<IndexType, SizeType>() && outputs[0].isa<IndexType>();
}

//===----------------------------------------------------------------------===//
// YieldOp
//===----------------------------------------------------------------------===//

LogicalResult shape::YieldOp::verify() {
  auto *parentOp = (*this)->getParentOp();
  auto results = parentOp->getResults();
  auto operands = getOperands();

  if (parentOp->getNumResults() != getNumOperands())
    return emitOpError() << "number of operands does not match number of "
                            "results of its parent";
  for (auto e : llvm::zip(results, operands))
    if (std::get<0>(e).getType() != std::get<1>(e).getType())
      return emitOpError() << "types mismatch between yield op and its parent";

  return success();
}

//===----------------------------------------------------------------------===//
// SplitAtOp
//===----------------------------------------------------------------------===//

LogicalResult SplitAtOp::fold(ArrayRef<Attribute> operands,
                              SmallVectorImpl<OpFoldResult> &results) {
  if (!operands[0] || !operands[1])
    return failure();
  auto shapeVec = llvm::to_vector<6>(
      operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
  auto shape = llvm::makeArrayRef(shapeVec);
  auto splitPoint = operands[1].cast<IntegerAttr>().getInt();
  // Verify that the split point is in the correct range.
  // TODO: Constant fold to an "error".
  int64_t rank = shape.size();
  if (-rank > splitPoint || splitPoint > rank)
    return failure();
  if (splitPoint < 0)
    splitPoint += shape.size();
  Builder builder(operands[0].getContext());
  results.push_back(builder.getIndexTensorAttr(shape.take_front(splitPoint)));
  results.push_back(builder.getIndexTensorAttr(shape.drop_front(splitPoint)));
  return success();
}

//===----------------------------------------------------------------------===//
// ToExtentTensorOp
//===----------------------------------------------------------------------===//

OpFoldResult ToExtentTensorOp::fold(ArrayRef<Attribute> operands) {
  if (!operands[0])
    return OpFoldResult();
  Builder builder(getContext());
  auto shape = llvm::to_vector<6>(
      operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
  auto type = RankedTensorType::get({static_cast<int64_t>(shape.size())},
                                    builder.getIndexType());
  return DenseIntElementsAttr::get(type, shape);
}

bool ToExtentTensorOp::areCastCompatible(TypeRange inputs, TypeRange outputs) {
  if (inputs.size() != 1 || outputs.size() != 1)
    return false;
  if (auto inputTensor = inputs[0].dyn_cast<RankedTensorType>()) {
    if (!inputTensor.getElementType().isa<IndexType>() ||
        inputTensor.getRank() != 1)
      return false;
  } else if (!inputs[0].isa<ShapeType>()) {
    return false;
  }

  TensorType outputTensor = outputs[0].dyn_cast<TensorType>();
  return outputTensor && outputTensor.getElementType().isa<IndexType>();
}

//===----------------------------------------------------------------------===//
// ReduceOp
//===----------------------------------------------------------------------===//

void ReduceOp::build(OpBuilder &builder, OperationState &result, Value shape,
                     ValueRange initVals) {
  result.addOperands(shape);
  result.addOperands(initVals);

  Region *bodyRegion = result.addRegion();
  bodyRegion->push_back(new Block);
  Block &bodyBlock = bodyRegion->front();
  bodyBlock.addArgument(builder.getIndexType(), result.location);

  Type elementType;
  if (auto tensorType = shape.getType().dyn_cast<TensorType>())
    elementType = tensorType.getElementType();
  else
    elementType = SizeType::get(builder.getContext());
  bodyBlock.addArgument(elementType, shape.getLoc());

  for (Value initVal : initVals) {
    bodyBlock.addArgument(initVal.getType(), initVal.getLoc());
    result.addTypes(initVal.getType());
  }
}

LogicalResult ReduceOp::verify() {
  // Verify block arg types.
  Block &block = getRegion().front();

  // The block takes index, extent, and aggregated values as arguments.
  auto blockArgsCount = getInitVals().size() + 2;
  if (block.getNumArguments() != blockArgsCount)
    return emitOpError() << "ReduceOp body is expected to have "
                         << blockArgsCount << " arguments";

  // The first block argument is the index and must always be of type `index`.
  if (!block.getArgument(0).getType().isa<IndexType>())
    return emitOpError(
        "argument 0 of ReduceOp body is expected to be of IndexType");

  // The second block argument is the extent and must be of type `size` or
  // `index`, depending on whether the reduce operation is applied to a shape or
  // to an extent tensor.
  Type extentTy = block.getArgument(1).getType();
  if (getShape().getType().isa<ShapeType>()) {
    if (!extentTy.isa<SizeType>())
      return emitOpError("argument 1 of ReduceOp body is expected to be of "
                         "SizeType if the ReduceOp operates on a ShapeType");
  } else {
    if (!extentTy.isa<IndexType>())
      return emitOpError(
          "argument 1 of ReduceOp body is expected to be of IndexType if the "
          "ReduceOp operates on an extent tensor");
  }

  for (const auto &type : llvm::enumerate(getInitVals()))
    if (block.getArgument(type.index() + 2).getType() != type.value().getType())
      return emitOpError() << "type mismatch between argument "
                           << type.index() + 2
                           << " of ReduceOp body and initial value "
                           << type.index();
  return success();
}

ParseResult ReduceOp::parse(OpAsmParser &parser, OperationState &result) {
  // Parse operands.
  SmallVector<OpAsmParser::UnresolvedOperand, 3> operands;
  Type shapeOrExtentTensorType;
  if (parser.parseOperandList(operands, /*requiredOperandCount=*/-1,
                              OpAsmParser::Delimiter::Paren) ||
      parser.parseColonType(shapeOrExtentTensorType) ||
      parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Resolve operands.
  auto initVals = llvm::makeArrayRef(operands).drop_front();
  if (parser.resolveOperand(operands.front(), shapeOrExtentTensorType,
                            result.operands) ||
      parser.resolveOperands(initVals, result.types, parser.getNameLoc(),
                             result.operands))
    return failure();

  // Parse the body.
  Region *body = result.addRegion();
  if (parser.parseRegion(*body, /*args=*/{}, /*argTypes=*/{}))
    return failure();

  // Parse attributes.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

void ReduceOp::print(OpAsmPrinter &p) {
  p << '(' << getShape() << ", " << getInitVals()
    << ") : " << getShape().getType();
  p.printOptionalArrowTypeList(getResultTypes());
  p << ' ';
  p.printRegion(getRegion());
  p.printOptionalAttrDict((*this)->getAttrs());
}

#define GET_OP_CLASSES
#include "mlir/Dialect/Shape/IR/ShapeOps.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "mlir/Dialect/Shape/IR/ShapeOpsTypes.cpp.inc"
