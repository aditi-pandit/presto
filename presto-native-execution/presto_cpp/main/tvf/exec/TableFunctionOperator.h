/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "presto_cpp/main/tvf/core/TableFunctionNode.h"

#include "velox/common/memory/HashStringAllocator.h"
#include "velox/exec/Operator.h"

namespace facebook::presto::tvf {

class TableFunctionOperator : public velox::exec::Operator {
 public:
  TableFunctionOperator(
      int32_t operatorId,
      velox::exec::DriverCtx* driverCtx,
      const std::shared_ptr<const TableFunctionNode>& tableFunctionNode);

  void addInput(velox::RowVectorPtr input) override;

  void noMoreInput() override;

  velox::RowVectorPtr getOutput() override;

  bool needsInput() const override {
    return !noMoreInput_;
  }

  velox::exec::BlockingReason isBlocked(
      velox::ContinueFuture* /* unused */) override {
    return velox::exec::BlockingReason::kNotBlocked;
  }

  bool isFinished() override {
    return (noMoreInput_ && input_ == nullptr);
  }

  void reclaim(
      uint64_t targetBytes,
      velox::memory::MemoryReclaimer::Stats& stats) override;

 private:
  bool spillEnabled() const {
    return spillConfig_.has_value();
  }

  void createTableFunction(
      const std::shared_ptr<const TableFunctionNode>& tableFunctionNode);

  // HashStringAllocator required by functions that allocate out of line
  // buffers.
  velox::HashStringAllocator stringAllocator_;

  std::unique_ptr<TableFunction> function_;

  velox::RowTypePtr inputType_;
};

// Custom translation logic to hook into Velox Driver.
class TableFunctionTranslator
    : public velox::exec::Operator::PlanNodeTranslator {
  std::unique_ptr<velox::exec::Operator> toOperator(
      velox::exec::DriverCtx* ctx,
      int32_t id,
      const velox::core::PlanNodePtr& node) {
    if (auto tableFunctionNodeNode =
            std::dynamic_pointer_cast<const TableFunctionNode>(node)) {
      return std::make_unique<TableFunctionOperator>(
          id, ctx, tableFunctionNodeNode);
    }
    return nullptr;
  }
};
} // namespace facebook::presto::tvf
