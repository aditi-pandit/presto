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

#include "presto_cpp/main/tvf/spi/Argument.h"

#include "velox/core/Expressions.h"

namespace facebook::presto::tvf {

class Descriptor : public Argument {
  Descriptor(
      std::vector<std::string> names,
      std::vector<velox::TypePtr> types) {
    VELOX_CHECK_EQ(names.size(), types.size());
    fields_.reserve(names.size());
    for (velox::column_index_t i = 0; i < names.size(); i++) {
      fields_.push_back(std::make_shared<velox::core::FieldAccessTypedExpr>(
          types.at(i), names.at(i)));
    }
  }

  std::vector<velox::core::FieldAccessTypedExprPtr> fields() {
    return fields_;
  }

 private:
  std::vector<velox::core::FieldAccessTypedExprPtr> fields_;
};

} // namespace facebook::presto::tvf
