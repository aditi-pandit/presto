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
#include "presto_cpp/main/tvf/exec/TableFunctionOperator.h"

#include "velox/common/memory/MemoryArbitrator.h"

namespace facebook::presto::tvf {

using namespace facebook::velox;
using namespace facebook::velox::exec;

TableFunctionOperator::TableFunctionOperator(
    int32_t operatorId,
    DriverCtx* driverCtx,
    const std::shared_ptr<const TableFunctionNode>& tableFunctionNode)
    : Operator(
          driverCtx,
          tableFunctionNode->outputType(),
          operatorId,
          tableFunctionNode->id(),
          "TableFunctionOperator",
          tableFunctionNode->canSpill(driverCtx->queryConfig())
              ? driverCtx->makeSpillConfig(operatorId)
              : std::nullopt),
      stringAllocator_(pool()) {
  const auto& inputType = tableFunctionNode->sources()[0]->outputType();
  inputType_ = tableFunctionNode->sources()[0]->outputType();

  createTableFunction(tableFunctionNode);
}

void TableFunctionOperator::createTableFunction(
    const std::shared_ptr<const TableFunctionNode>& node) {
  function_ = TableFunction::create(
      node->functionName(),
      node->handle(),
      operatorCtx_->pool(),
      &stringAllocator_,
      operatorCtx_->driverCtx()->queryConfig());
  VELOX_CHECK(function_);
}

void TableFunctionOperator::addInput(RowVectorPtr input) {
  input_ = std::move(input);
}

void TableFunctionOperator::noMoreInput() {
  Operator::noMoreInput();
}

RowVectorPtr TableFunctionOperator::getOutput() {
  if (input_ == nullptr) {
    return nullptr;
  }

  auto results = input_;
  input_ = nullptr;
  return results;
}

void TableFunctionOperator::reclaim(
    uint64_t /*targetBytes*/,
    memory::MemoryReclaimer::Stats& stats) {
  VELOX_NYI("TableFunctionOperator::reclaim not implemented");
}

} // namespace facebook::presto::tvf
