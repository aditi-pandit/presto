# Table-Valued Functions (TVF) — C++ API Design

## Overview

Table-Valued Functions (TVFs) are SQL functions that return tables rather than scalar values.
They appear in the `FROM` clause and can accept other tables, descriptors, and scalar constants as arguments.
This document covers the **C++ native worker SPI** and provides worked examples for both execution modes.

Reference: [RFC-0020-tvf](https://github.com/prestodb/rfcs/blob/main/RFC-0020-tvf.md)
Implementation: [PR #26722](https://github.com/prestodb/presto/pull/26722)

---

## SQL Syntax

```sql
SELECT * FROM TABLE(
    catalog.schema.my_function(
        input  => TABLE(SELECT * FROM orders) t PARTITION BY custkey ORDER BY orderdate,
        layout => DESCRIPTOR(order_id bigint, status varchar),
        limit  => BIGINT '100'
    )
) result
```

### Argument Types

| Kind | SQL Form | Description |
|------|----------|-------------|
| Table | `TABLE(query) alias PARTITION BY … ORDER BY …` | A full relation; rows arrive partitioned and ordered |
| Descriptor | `DESCRIPTOR(col1 type1, col2)` | A list of column names with optional types |
| Scalar | A typed constant, e.g., `BIGINT '42'` | A single constant value |

### Table Argument Modifiers

| Modifier | Meaning |
|----------|---------|
| `PARTITION BY cols` | Distribute rows across parallel worker instances |
| `ORDER BY cols` | Sort rows within each partition |
| `COPARTITION (t1, t2)` | Co-locate two table arguments on matching partition columns |
| `PRUNE WHEN EMPTY` (default) | Short-circuit to empty output when input is empty |
| `KEEP WHEN EMPTY` | Invoke the processor even when the input is empty |
| Pass-through columns | Input columns forwarded unchanged to the output |

---

## Polymorphism for Table Functions
Table-valued functions are inherently polymorphic because their input and output schemas are not fixed at declaration
time. They vary depending on which arguments the user supplies in the SQL.

Unlike scalar and aggregate functions whose types can be resolved at registration time,
TVFs require a dynamic analysis phase during query planning to determine the output schema and runtime context.
This design enables maximum flexibility while keeping the C++ SPI clean and intuitive.


## Part I — C++ SPI

### 1. Argument Classes

#### 1a. ArgumentSpecification (base)

```cpp
// presto_cpp/main/tvf/spi/Argument.h
class ArgumentSpecification {
 public:
  ArgumentSpecification(
      std::string name,
      bool required,
      std::optional<velox::variant> defaultValue = std::nullopt);

  const std::string& name() const;
  bool required() const;
  const std::optional<velox::variant>& defaultValue() const;
};
```

#### 1b. ScalarArgumentSpecification / ScalarArgument

```cpp
// presto_cpp/main/tvf/spi/ScalarArgument.h

// Declared at registration time — describes a scalar parameter.
class ScalarArgumentSpecification : public ArgumentSpecification {
 public:
  velox::TypePtr rowType() const;   // the argument's Velox type
 private:
  velox::TypePtr type_;
};

// Received at analysis time — carries the caller-supplied constant.
class ScalarArgument : public Argument {
 public:
  velox::TypePtr rowType() const;
  velox::VectorPtr value() const;   // constant value as a single-row vector
 private:
  velox::TypePtr type_;
  velox::VectorPtr constantValue_;
};
```

#### 1c. TableArgumentSpecification / TableArgument

```cpp
// presto_cpp/main/tvf/spi/TableArgument.h

// Declared at registration time.
class TableArgumentSpecification : public ArgumentSpecification {
 public:
  bool rowSemantics;       // row-by-row processing; disables PARTITION BY
  bool pruneWhenEmpty;     // (default) return empty output when input is empty
  bool passThroughColumns; // forward all input columns to the output
};

// Type alias used when registering functions.
using TableArgumentSpecList =
    std::vector<std::shared_ptr<ArgumentSpecification>>;

// Received at analysis time — carries the input relation's schema.
class TableArgument : public Argument {
 public:
  // Row type of the input table.
  velox::RowTypePtr rowType() const;
  // Partition and sort keys specified by the caller in SQL.
  const std::vector<velox::core::FieldAccessTypedExprPtr>& partitionKeys() const;
  const std::vector<velox::core::FieldAccessTypedExprPtr>& sortingKeys() const;
  const std::vector<velox::core::SortOrder>& sortingOrders() const;
};
```

#### 1d. DescriptorArgumentSpecification / DescriptorArgument

```cpp
// presto_cpp/main/tvf/spi/DescriptorArgument.h

class DescriptorArgumentSpecification : public ArgumentSpecification {};

class DescriptorArgument : public Argument {
 public:
  const Descriptor& descriptor() const;
};
```

#### 1e. Descriptor

```cpp
// presto_cpp/main/tvf/spi/Descriptor.h

class Descriptor {
 public:
  // Names only (no types).
  explicit Descriptor(std::vector<std::string> names);
  // Names with corresponding Velox types.
  Descriptor(std::vector<std::string> names, std::vector<velox::TypePtr> types);

  const std::vector<std::string>& names() const;
  const std::vector<velox::TypePtr>& types() const;
  // Field access expressions for use in plan nodes.
  std::vector<velox::core::FieldAccessTypedExprPtr> typedExprs() const;
};

using DescriptorPtr = std::shared_ptr<const Descriptor>;
```

---

### 2. Return Type Specifications

```cpp
// presto_cpp/main/tvf/spi/ReturnTypeSpecification.h

class ReturnTypeSpecification {
 public:
  enum class ReturnType { kGenericTable, kDescribedTable, kOnlyPassThrough };
  virtual ReturnType returnType() const = 0;
};

using ReturnSpecPtr = std::shared_ptr<ReturnTypeSpecification>;

// Output schema determined dynamically in analyze().
class GenericTableReturnTypeSpecification : public ReturnTypeSpecification {};

// Output schema fixed and fully known at registration time.
class DescribedTableReturnTypeSpecification : public ReturnTypeSpecification {
 public:
  explicit DescribedTableReturnTypeSpecification(DescriptorPtr descriptor);
  DescriptorPtr descriptor() const;
};

// No new columns added; output is purely pass-through from input tables.
class OnlyPassThroughReturnTypeSpecification : public ReturnTypeSpecification {};
```

---

### 3. Analysis — TableFunctionHandle and TableFunctionAnalysis

`analyze()` runs on the **coordinator** at planning time. It validates arguments,
determines the output schema, and packages runtime context into a
`TableFunctionHandle` that is serialized and shipped to workers.

```cpp
// presto_cpp/main/tvf/spi/TableFunctionAnalysis.h

// Opaque coordinator-to-worker context. Subclass and serialize via
// velox::ISerializable (implement serialize() and name()).
class TableFunctionHandle : public velox::ISerializable {
 public:
  virtual folly::dynamic serialize() const = 0;
  virtual std::string name() const = 0;
};

using TableFunctionHandlePtr = std::shared_ptr<TableFunctionHandle>;

// Opaque split context. Returned by getSplits(); passed to the split processor.
class TableSplitHandle : public velox::ISerializable {
 public:
  virtual folly::dynamic serialize() const = 0;
  virtual std::string name() const = 0;
};

using TableSplitHandlePtr = std::shared_ptr<TableSplitHandle>;

// Result returned by analyze().
struct TableFunctionAnalysis {
  // Dynamic output schema. Required when GenericTableReturnTypeSpecification
  // is used; leave null otherwise.
  DescriptorPtr returnType_;

  // Opaque context passed to workers and to getSplits().
  TableFunctionHandlePtr tableFunctionHandle_;

  // For each table argument name, the list of column indexes actually read.
  // The optimizer prunes all other columns before they reach the processor.
  std::unordered_map<std::string, std::vector<int>> requiredColumns_;
};
```

---

### 4. TableFunctionResult

Both processor types return `TableFunctionResult` from every `apply()` call.

```cpp
// presto_cpp/main/tvf/spi/TableFunctionResult.h

class TableFunctionResult {
 public:
  enum class State { kBlocked, kFinished, kProcessed };

  // Processing complete — apply() will not be called again.
  static TableFunctionResult finished();

  // Output produced. outputVectors contains:
  //   - Columns declared by the function's return type.
  //   - Optional trailing BIGINT pass-through columns: each entry is the
  //     0-based row index within the current partition to attach, or null
  //     to attach a row of nulls.
  static TableFunctionResult processed(
      std::vector<velox::VectorPtr> outputVectors);

  // Waiting for an asynchronous dependency.
  static TableFunctionResult blocked(
      std::shared_ptr<velox::ContinueFuture> future);

  State state() const;
  const std::vector<velox::VectorPtr>& outputVectors() const;
};
```

---

### 5. Execution Processors

#### 5a. TableFunctionDataProcessor

Used when the function consumes one or more table arguments (set semantics).
One processor instance is created per partition.

```cpp
// presto_cpp/main/tvf/spi/TableFunction.h

class TableFunctionDataProcessor {
 public:
  virtual ~TableFunctionDataProcessor() = default;

  // Called repeatedly until kFinished is returned.
  //
  // input  — one RowVectorPtr per table argument, in declaration order.
  //          Each vector contains only the columns listed in requiredColumns_.
  //          An empty vector (zero rows) means the source is not yet ready.
  //          A null pointer means all sources are exhausted.
  virtual TableFunctionResult apply(
      const std::vector<velox::RowVectorPtr>& input) = 0;

 protected:
  velox::memory::MemoryPool* pool_;
  velox::HashStringAllocator* allocator_;
};
```

#### 5b. TableFunctionSplitProcessor

Used for leaf functions that generate data from scratch (no table inputs).
One processor instance is created per split.

```cpp
class TableFunctionSplitProcessor {
 public:
  virtual ~TableFunctionSplitProcessor() = default;

  // Called repeatedly until kFinished is returned.
  //
  // split  — the split to process.
  //          null when KEEP WHEN EMPTY is set and the source was empty.
  virtual TableFunctionResult apply(const TableSplitHandlePtr& split) = 0;

 protected:
  velox::memory::MemoryPool* pool_;
  velox::HashStringAllocator* allocator_;
};
```

---

### 6. The TableFunction Factory and Registration

`TableFunction` is a static factory/registry. Use `registerTableFunction()` to
register a function at startup.

```cpp
// presto_cpp/main/tvf/spi/TableFunction.h

// Callback types supplied at registration.
using TableFunctionAnalyzer =
    std::function<TableFunctionAnalysis(
        const std::unordered_map<std::string, std::shared_ptr<Argument>>&)>;

using TableFunctionSplitGenerator =
    std::function<std::vector<TableSplitHandlePtr>(
        const TableFunctionHandlePtr&)>;

using TableFunctionDataProcessorFactory =
    std::function<std::unique_ptr<TableFunctionDataProcessor>(
        const TableFunctionHandlePtr&,
        velox::memory::MemoryPool*,
        velox::HashStringAllocator*)>;

using TableFunctionSplitProcessorFactory =
    std::function<std::unique_ptr<TableFunctionSplitProcessor>(
        const TableSplitHandlePtr&,
        velox::memory::MemoryPool*,
        velox::HashStringAllocator*)>;

struct TableFunctionEntry {
  TableArgumentSpecList                argumentSpecs;
  ReturnSpecPtr                        returnTypeSpec;
  TableFunctionAnalyzer                analyzer;
  TableFunctionSplitGenerator          splitGenerator;      // null for data processors
  TableFunctionDataProcessorFactory    dataProcessorFactory;  // null for split processors
  TableFunctionSplitProcessorFactory   splitProcessorFactory; // null for data processors
};

using TableFunctionMap = std::unordered_map<std::string, TableFunctionEntry>;

class TableFunction {
 public:
  static void registerTableFunction(
      const std::string& name,
      TableArgumentSpecList argumentSpecs,
      ReturnSpecPtr returnTypeSpec,
      TableFunctionAnalyzer analyzer,
      TableFunctionSplitGenerator splitGenerator,
      TableFunctionSplitProcessorFactory splitProcessorFactory);

  static void registerTableFunction(
      const std::string& name,
      TableArgumentSpecList argumentSpecs,
      ReturnSpecPtr returnTypeSpec,
      TableFunctionAnalyzer analyzer,
      TableFunctionDataProcessorFactory dataProcessorFactory);

  static const TableFunctionMap& tableFunctions();
};
```

All built-in functions are registered via:

```cpp
// presto_cpp/main/tvf/functions/TableFunctionsRegistration.h
namespace facebook::presto::tvf {
  void registerAllTableFunctions(const std::string& prefix = "");
}
```

---

### 7. Planning Overview

| Input shape | Operator used |
|-------------|---------------|
| No table inputs | `LeafTableFunctionOperator` (split-driven) |
| Single table, row semantics | Planned as `Project`/`Filter` |
| Single table, set semantics + `PARTITION BY` | Planned like a window function |
| Multiple table inputs | Full outer join with row-number tracking, one window per input |

---

## Part II — Examples

### Example 1 — `sequence` (TableFunctionSplitProcessor)

`sequence` generates a range of integers with no input table. Each split independently
produces a contiguous sub-range, up to 1 000 000 values per split.

#### SQL usage

```sql
-- Ascending sequence
SELECT * FROM TABLE(system.sequence(start => 1, stop => 10));

-- Descending with custom step
SELECT * FROM TABLE(system.sequence(start => 1000000, stop => -2000000, step => -3));
```

#### Handles

```cpp
// SequenceHandle — coordinator context; drives split generation.
class SequenceHandle : public TableFunctionHandle {
 public:
  SequenceHandle(int64_t start, int64_t stop, int64_t step)
      : start_(start), stop_(stop), step_(step) {}

  int64_t start() const { return start_; }
  int64_t stop()  const { return stop_;  }
  int64_t step()  const { return step_;  }

  folly::dynamic serialize() const override { /* start, stop, step */ }
  std::string name() const override { return "SequenceHandle"; }

 private:
  int64_t start_, stop_, step_;
};

// SequenceSplitHandle — per-split context; drives the processor.
class SequenceSplitHandle : public TableSplitHandle {
 public:
  SequenceSplitHandle(int64_t rangeStart, int64_t numSteps, int64_t step)
      : rangeStart_(rangeStart), numSteps_(numSteps), step_(step) {}

  int64_t rangeStart() const { return rangeStart_; }
  int64_t numSteps()   const { return numSteps_;   }
  int64_t step()       const { return step_;       }

  folly::dynamic serialize() const override { /* ... */ }
  std::string name() const override { return "SequenceSplitHandle"; }

 private:
  int64_t rangeStart_, numSteps_, step_;
};
```

#### Analyzer

```cpp
TableFunctionAnalysis sequenceAnalyzer(
    const std::unordered_map<std::string, std::shared_ptr<Argument>>& args)
{
  auto& startArg = std::dynamic_pointer_cast<ScalarArgument>(args.at("start"));
  auto& stopArg  = std::dynamic_pointer_cast<ScalarArgument>(args.at("stop"));
  auto& stepArg  = std::dynamic_pointer_cast<ScalarArgument>(args.at("step"));

  int64_t start = startArg->value()->as<velox::FlatVector<int64_t>>()->valueAt(0);
  int64_t stop  = stopArg ->value()->as<velox::FlatVector<int64_t>>()->valueAt(0);
  int64_t step  = stepArg ->value()->as<velox::FlatVector<int64_t>>()->valueAt(0);

  VELOX_USER_CHECK_NE(step, 0, "Step must not be zero");
  VELOX_USER_CHECK(
      !(step > 0 && start > stop),
      "Step must be negative for descending sequence");
  VELOX_USER_CHECK(
      !(step < 0 && start < stop),
      "Step must be positive for ascending sequence");

  TableFunctionAnalysis analysis;
  // Return type is DescribedTable — no need to set returnType_ dynamically.
  analysis.tableFunctionHandle_ = std::make_shared<SequenceHandle>(start, stop, step);
  // No requiredColumns_ — leaf function has no table inputs.
  return analysis;
}
```

#### Split generator

```cpp
std::vector<TableSplitHandlePtr> sequenceSplitGenerator(
    const TableFunctionHandlePtr& handle)
{
  auto& h = dynamic_cast<const SequenceHandle&>(*handle);
  constexpr int64_t kMaxSplitSize = 1'000'000;

  std::vector<TableSplitHandlePtr> splits;
  int64_t current = h.start();
  while (h.step() > 0 ? current <= h.stop() : current >= h.stop()) {
    int64_t remaining = std::abs(h.stop() - current) / std::abs(h.step()) + 1;
    int64_t numSteps  = std::min(remaining, kMaxSplitSize);
    splits.push_back(
        std::make_shared<SequenceSplitHandle>(current, numSteps, h.step()));
    current += numSteps * h.step();
  }
  return splits;
}
```

#### Split processor

```cpp
class SequenceSplitProcessor : public TableFunctionSplitProcessor {
 public:
  TableFunctionResult apply(const TableSplitHandlePtr& splitHandle) override {
    if (done_) {
      return TableFunctionResult::finished();
    }

    auto& split = dynamic_cast<const SequenceSplitHandle&>(*splitHandle);

    // First call — initialise cursor.
    if (!initialised_) {
      current_   = split.rangeStart();
      remaining_ = split.numSteps();
      step_      = split.step();
      initialised_ = true;
    }

    // Fill one output batch (up to 1 024 rows).
    auto out = velox::BaseVector::create<velox::FlatVector<int64_t>>(
        velox::BIGINT(), 0, pool_);
    int64_t count = std::min<int64_t>(remaining_, 1024);
    out->resize(count);
    for (int64_t i = 0; i < count; ++i) {
      out->set(i, current_);
      current_ += step_;
    }
    remaining_ -= count;
    done_ = (remaining_ == 0);

    return TableFunctionResult::processed({out});
  }

 private:
  bool    initialised_ = false;
  bool    done_        = false;
  int64_t current_     = 0;
  int64_t remaining_   = 0;
  int64_t step_        = 1;
};
```

#### Registration

```cpp
void registerSequence(const std::string& prefix) {
  TableFunction::registerTableFunction(
      prefix + "sequence",
      {
        std::make_shared<ScalarArgumentSpecification>("start", /*required=*/false,
            velox::variant(int64_t{0})),
        std::make_shared<ScalarArgumentSpecification>("stop",  /*required=*/true),
        std::make_shared<ScalarArgumentSpecification>("step",  /*required=*/false,
            velox::variant(int64_t{1})),
      },
      std::make_shared<DescribedTableReturnTypeSpecification>(
          std::make_shared<Descriptor>(
              std::vector<std::string>{"sequence_value"},
              std::vector<velox::TypePtr>{velox::BIGINT()})),
      sequenceAnalyzer,
      sequenceSplitGenerator,
      [](const TableSplitHandlePtr& /*split*/, velox::memory::MemoryPool* pool,
         velox::HashStringAllocator* alloc) {
        auto p = std::make_unique<SequenceSplitProcessor>();
        p->pool_ = pool; p->allocator_ = alloc;
        return p;
      });
}
```

#### Execution flow

```
Coordinator
  sequenceAnalyzer(args)       → SequenceHandle{start, stop, step}
  sequenceSplitGenerator(h)    → [SplitHandle(0..1M), SplitHandle(1M+1..2M), …]

Worker (one SequenceSplitProcessor per split)
  loop: processor.apply(splitHandle)
    → TableFunctionResult::processed({batch})   // up to 1 024 rows
    → TableFunctionResult::finished()           // split complete
```

---

### Example 2 — `exclude_columns` (TableFunctionDataProcessor)

`exclude_columns` accepts an input table and a descriptor of columns to remove.
It uses `TableFunctionDataProcessor` because it processes rows from an input relation.

#### SQL usage

```sql
-- Return all columns from 'orders' except 'clerk' and 'comment'
SELECT * FROM TABLE(
    system.exclude_columns(
        input   => TABLE(SELECT * FROM orders),
        columns => DESCRIPTOR(clerk, comment)
    )
);
```

#### Handle

```cpp
class ExcludeColumnsHandle : public TableFunctionHandle {
 public:
  folly::dynamic serialize() const override { return folly::dynamic::object(); }
  std::string name() const override { return "ExcludeColumnsHandle"; }
};
```

#### Analyzer

```cpp
TableFunctionAnalysis excludeColumnsAnalyzer(
    const std::unordered_map<std::string, std::shared_ptr<Argument>>& args)
{
  auto inputArg = std::dynamic_pointer_cast<TableArgument>(args.at("input"));
  auto colsArg  = std::dynamic_pointer_cast<DescriptorArgument>(args.at("columns"));

  const auto& desc = colsArg->descriptor();

  // Descriptor must contain names only — no types.
  VELOX_USER_CHECK(
      desc.types().empty(),
      "Column types must not be specified in the COLUMNS descriptor");

  // Build set of excluded names (case-insensitive).
  std::unordered_set<std::string> excluded;
  for (const auto& name : desc.names()) {
    excluded.insert(folly::toLower(name));
  }

  // Determine which input column indexes to keep.
  const auto& inputType = inputArg->rowType();
  std::vector<int> keptIndexes;
  for (int i = 0; i < inputType->size(); ++i) {
    if (!excluded.count(folly::toLower(inputType->nameOf(i)))) {
      keptIndexes.push_back(i);
    }
  }

  VELOX_USER_CHECK(!keptIndexes.empty(), "All columns would be excluded");

  TableFunctionAnalysis analysis;
  // OnlyPassThrough — no new columns; no need to set returnType_.
  // requiredColumns_ tells the optimizer to project away excluded columns.
  analysis.requiredColumns_["input"]    = keptIndexes;
  analysis.tableFunctionHandle_         = std::make_shared<ExcludeColumnsHandle>();
  return analysis;
}
```

#### Data processor

Because the optimizer projects away excluded columns before rows reach the processor
(via `requiredColumns_`), the processor simply forwards each batch unchanged.

```cpp
class ExcludeColumnsDataProcessor : public TableFunctionDataProcessor {
 public:
  TableFunctionResult apply(
      const std::vector<velox::RowVectorPtr>& input) override
  {
    // null input → all sources exhausted.
    if (input.empty() || input[0] == nullptr) {
      return TableFunctionResult::finished();
    }

    const auto& batch = input[0];  // single table argument: INPUT

    // Zero-row batch → source not yet ready; signal we consumed the slot.
    if (batch->size() == 0) {
      return TableFunctionResult::processed({});
    }

    // Excluded columns already stripped by the engine; forward as-is.
    // pass-through columns are implicit — OnlyPassThrough return type.
    return TableFunctionResult::processed(
        {batch->childAt(0), batch->childAt(1) /*, … */});
  }
};
```

#### Registration

```cpp
void registerExcludeColumns(const std::string& prefix) {
  auto inputSpec = std::make_shared<TableArgumentSpecification>(
      "input", /*required=*/true);
  inputSpec->passThroughColumns = true;
  // keepWhenEmpty = true: invoke the processor even on empty input.
  inputSpec->pruneWhenEmpty = false;

  TableFunction::registerTableFunction(
      prefix + "exclude_columns",
      {
        inputSpec,
        std::make_shared<DescriptorArgumentSpecification>("columns", /*required=*/true),
      },
      std::make_shared<OnlyPassThroughReturnTypeSpecification>(),
      excludeColumnsAnalyzer,
      [](const TableFunctionHandlePtr& /*handle*/,
         velox::memory::MemoryPool* pool,
         velox::HashStringAllocator* alloc) {
        auto p = std::make_unique<ExcludeColumnsDataProcessor>();
        p->pool_ = pool; p->allocator_ = alloc;
        return p;
      });
}
```

#### Execution flow

```
Coordinator
  excludeColumnsAnalyzer(args)
    → requiredColumns_["input"] = [0, 1, 3, 4]   // columns 2 and 5 excluded
    → ExcludeColumnsHandle{}

Planner
  Inserts a Project before the TVF operator that strips excluded columns.

Worker (one ExcludeColumnsDataProcessor per partition)
  loop: processor.apply(input)
    batch has rows  → TableFunctionResult::processed(batch)   // forward
    batch is empty  → TableFunctionResult::processed({})      // wait
    input is null   → TableFunctionResult::finished()
```

---

## Summary: Choosing the Right Processor

| Criterion | `TableFunctionSplitProcessor` | `TableFunctionDataProcessor` |
|-----------|-------------------------------|------------------------------|
| Input tables | None — generates data itself | One or more table arguments |
| Parallelism unit | One processor per split | One processor per partition |
| Split source | `TableFunctionSplitGenerator` callback | Engine-managed from input operators |
| Typical use case | Sequence generators, external data pulls | Column transforms, analytics, ML inference |
| Null/empty input | `KEEP WHEN EMPTY` — input was empty | All sources exhausted |

---

## Key Design Constraints

- `analyze()` runs on the **coordinator** at planning time — keep it fast and
  side-effect free.
- `requiredColumns_` drives optimizer column pruning — always declare the minimal
  set of columns actually read.
- `TableFunctionHandle` and `TableSplitHandle` must implement `velox::ISerializable`
  (`serialize()` + `name()`) as they are shipped over the wire.
- `TableFunctionSplitProcessor` instances are **not** reused across splits.
- `TableFunctionDataProcessor` receives vectors in the order table arguments were
  declared in the registration call.
- Pass-through columns are represented as trailing BIGINT columns in the output
  vectors; each value is a 0-based row index into the current partition, or null
  to substitute a row of nulls.
