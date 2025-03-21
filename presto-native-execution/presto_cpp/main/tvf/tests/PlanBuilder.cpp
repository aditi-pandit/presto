/*
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
#include "presto_cpp/main/tvf/tests/PlanBuilder.h"

using namespace facebook::velox;
using namespace facebook::velox::core;

namespace facebook::presto::tvf {

std::function<
    velox::core::PlanNodePtr(std::string nodeId, velox::core::PlanNodePtr)>
addTvfNode(
    const std::string& name,
    const std::unordered_map<std::string, std::shared_ptr<Argument>>& args) {
  return [&name, &args](PlanNodeId nodeId, PlanNodePtr source) -> PlanNodePtr {
    QueryConfig c{{}};
    auto analysis = TableFunction::analyze(name, args, c);
    VELOX_CHECK(analysis);
    VELOX_CHECK(analysis->tableFunctionHandle());
    VELOX_CHECK(analysis->returnType());

    std::vector<std::string> names = analysis->returnType()->names();
    std::vector<velox::TypePtr> types = analysis->returnType()->types();
    auto outputType = velox::ROW(std::move(names), std::move(types));

    return std::make_shared<TableFunctionNode>(
        nodeId,
        name,
        analysis->tableFunctionHandle(),
        outputType,
        analysis->requiredColumns(),
        source);
  };
}
} // namespace facebook::presto::tvf
