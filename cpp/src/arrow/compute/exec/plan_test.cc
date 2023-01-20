// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gmock/gmock-matchers.h>

#include <functional>
#include <memory>

#include "arrow/compute/exec.h"
#include "arrow/compute/exec/exec_plan.h"
#include "arrow/compute/exec/expression.h"
#include "arrow/compute/exec/options.h"
#include "arrow/compute/exec/test_util.h"
#include "arrow/compute/exec/util.h"
#include "arrow/io/util_internal.h"
#include "arrow/record_batch.h"
#include "arrow/table.h"
#include "arrow/testing/future_util.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/testing/matchers.h"
#include "arrow/testing/random.h"
#include "arrow/util/async_generator.h"
#include "arrow/util/logging.h"
#include "arrow/util/thread_pool.h"
#include "arrow/util/vector.h"

using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::HasSubstr;
using testing::Optional;
using testing::UnorderedElementsAreArray;

namespace arrow {

namespace compute {

TEST(ExecPlanConstruction, Empty) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());

  ASSERT_THAT(plan->Validate(), Raises(StatusCode::Invalid));
}

TEST(ExecPlanConstruction, SingleNode) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  auto node = MakeDummyNode(plan.get(), "dummy", /*inputs=*/{}, /*num_outputs=*/0);
  ASSERT_OK(plan->Validate());
  ASSERT_THAT(plan->sources(), ElementsAre(node));
  ASSERT_THAT(plan->sinks(), ElementsAre(node));

  ASSERT_OK_AND_ASSIGN(plan, ExecPlan::Make());
  node = MakeDummyNode(plan.get(), "dummy", /*inputs=*/{}, /*num_outputs=*/1);
  // Output not bound
  ASSERT_THAT(plan->Validate(), Raises(StatusCode::Invalid));
}

TEST(ExecPlanConstruction, SourceSink) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  auto source = MakeDummyNode(plan.get(), "source", /*inputs=*/{}, /*num_outputs=*/1);
  auto sink = MakeDummyNode(plan.get(), "sink", /*inputs=*/{source}, /*num_outputs=*/0);

  ASSERT_OK(plan->Validate());
  EXPECT_THAT(plan->sources(), ElementsAre(source));
  EXPECT_THAT(plan->sinks(), ElementsAre(sink));
}

TEST(ExecPlanConstruction, MultipleNode) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());

  auto source1 = MakeDummyNode(plan.get(), "source1", /*inputs=*/{}, /*num_outputs=*/2);

  auto source2 = MakeDummyNode(plan.get(), "source2", /*inputs=*/{}, /*num_outputs=*/1);

  auto process1 =
      MakeDummyNode(plan.get(), "process1", /*inputs=*/{source1}, /*num_outputs=*/2);

  auto process2 = MakeDummyNode(plan.get(), "process1", /*inputs=*/{source1, source2},
                                /*num_outputs=*/1);

  auto process3 =
      MakeDummyNode(plan.get(), "process3", /*inputs=*/{process1, process2, process1},
                    /*num_outputs=*/1);

  auto sink = MakeDummyNode(plan.get(), "sink", /*inputs=*/{process3}, /*num_outputs=*/0);

  ASSERT_OK(plan->Validate());
  ASSERT_THAT(plan->sources(), ElementsAre(source1, source2));
  ASSERT_THAT(plan->sinks(), ElementsAre(sink));
}

TEST(ExecPlanConstruction, AutoLabel) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  auto source1 = MakeDummyNode(plan.get(), "", /*inputs=*/{}, /*num_outputs=*/2);
  auto source2 =
      MakeDummyNode(plan.get(), "some_label", /*inputs=*/{}, /*num_outputs=*/1);
  auto source3 = MakeDummyNode(plan.get(), "", /*inputs=*/{}, /*num_outputs=*/2);

  ASSERT_EQ("0", source1->label());
  ASSERT_EQ("some_label", source2->label());
  ASSERT_EQ("2", source3->label());
}

struct StartStopTracker {
  std::vector<std::string> started, stopped;

  StartProducingFunc start_producing_func(Status st = Status::OK()) {
    return [this, st](ExecNode* node) {
      started.push_back(node->label());
      return st;
    };
  }

  StopProducingFunc stop_producing_func() {
    return [this](ExecNode* node) { stopped.push_back(node->label()); };
  }
};

TEST(ExecPlan, DummyStartProducing) {
  StartStopTracker t;

  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());

  auto source1 = MakeDummyNode(plan.get(), "source1", /*inputs=*/{}, /*num_outputs=*/2,
                               t.start_producing_func(), t.stop_producing_func());

  auto source2 = MakeDummyNode(plan.get(), "source2", /*inputs=*/{}, /*num_outputs=*/1,
                               t.start_producing_func(), t.stop_producing_func());

  auto process1 =
      MakeDummyNode(plan.get(), "process1", /*inputs=*/{source1}, /*num_outputs=*/2,
                    t.start_producing_func(), t.stop_producing_func());

  auto process2 =
      MakeDummyNode(plan.get(), "process2", /*inputs=*/{process1, source2},
                    /*num_outputs=*/1, t.start_producing_func(), t.stop_producing_func());

  auto process3 =
      MakeDummyNode(plan.get(), "process3", /*inputs=*/{process1, source1, process2},
                    /*num_outputs=*/1, t.start_producing_func(), t.stop_producing_func());

  MakeDummyNode(plan.get(), "sink", /*inputs=*/{process3}, /*num_outputs=*/0,
                t.start_producing_func(), t.stop_producing_func());

  ASSERT_OK(plan->Validate());
  ASSERT_EQ(t.started.size(), 0);
  ASSERT_EQ(t.stopped.size(), 0);

  ASSERT_OK(plan->StartProducing());
  // Note that any correct reverse topological order may do
  ASSERT_THAT(t.started, ElementsAre("sink", "process3", "process2", "process1",
                                     "source2", "source1"));

  plan->StopProducing();
  ASSERT_THAT(plan->finished(), Finishes(Ok()));
  // Note that any correct topological order may do
  ASSERT_THAT(t.stopped, ElementsAre("source1", "source2", "process1", "process2",
                                     "process3", "sink"));

  ASSERT_THAT(plan->StartProducing(),
              Raises(StatusCode::Invalid, HasSubstr("restarted")));
}

TEST(ExecPlan, DummyStartProducingError) {
  StartStopTracker t;

  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  auto source1 = MakeDummyNode(
      plan.get(), "source1", /*num_inputs=*/{}, /*num_outputs=*/2,
      t.start_producing_func(Status::NotImplemented("zzz")), t.stop_producing_func());

  auto source2 =
      MakeDummyNode(plan.get(), "source2", /*num_inputs=*/{}, /*num_outputs=*/1,
                    t.start_producing_func(), t.stop_producing_func());

  auto process1 = MakeDummyNode(
      plan.get(), "process1", /*num_inputs=*/{source1}, /*num_outputs=*/2,
      t.start_producing_func(Status::IOError("xxx")), t.stop_producing_func());

  auto process2 =
      MakeDummyNode(plan.get(), "process2", /*num_inputs=*/{process1, source2},
                    /*num_outputs=*/1, t.start_producing_func(), t.stop_producing_func());

  auto process3 =
      MakeDummyNode(plan.get(), "process3", /*num_inputs=*/{process1, source1, process2},
                    /*num_outputs=*/1, t.start_producing_func(), t.stop_producing_func());

  MakeDummyNode(plan.get(), "sink", /*num_inputs=*/{process3}, /*num_outputs=*/0,
                t.start_producing_func(), t.stop_producing_func());

  ASSERT_OK(plan->Validate());
  ASSERT_EQ(t.started.size(), 0);
  ASSERT_EQ(t.stopped.size(), 0);

  // `process1` raises IOError
  ASSERT_THAT(plan->StartProducing(), Raises(StatusCode::IOError));
  ASSERT_THAT(t.started, ElementsAre("sink", "process3", "process2", "process1"));
  // Nodes that started successfully were stopped in reverse order
  ASSERT_THAT(t.stopped, ElementsAre("process2", "process3", "sink"));
}

TEST(ExecPlanExecution, SourceSink) {
  for (bool slow : {false, true}) {
    SCOPED_TRACE(slow ? "slowed" : "unslowed");

    for (bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "single threaded");

      auto basic_data = MakeBasicBatches();

      Declaration plan(
          "source", SourceNodeOptions{basic_data.schema, basic_data.gen(parallel, slow)});
      ASSERT_OK_AND_ASSIGN(auto result,
                           DeclarationToExecBatches(std::move(plan), parallel));
      AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches,
                                          basic_data.batches);
    }
  }
}

TEST(ExecPlanExecution, UseSinkAfterExecution) {
  AsyncGenerator<std::optional<ExecBatch>> sink_gen;
  {
    ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
    auto basic_data = MakeBasicBatches();
    ASSERT_OK(Declaration::Sequence(
                  {
                      {"source", SourceNodeOptions{basic_data.schema,
                                                   basic_data.gen(/*parallel=*/false,
                                                                  /*slow=*/false)}},
                      {"sink", SinkNodeOptions{&sink_gen}},
                  })
                  .AddToPlan(plan.get()));
    ASSERT_OK(plan->StartProducing());
    ASSERT_FINISHES_OK(plan->finished());
  }
  ASSERT_FINISHES_AND_RAISES(Invalid, sink_gen());
}

TEST(ExecPlanExecution, TableSourceSink) {
  for (int batch_size : {1, 4}) {
    auto exp_batches = MakeBasicBatches();
    ASSERT_OK_AND_ASSIGN(auto table,
                         TableFromExecBatches(exp_batches.schema, exp_batches.batches));
    Declaration plan("table_source", TableSourceNodeOptions{table, batch_size});

    ASSERT_OK_AND_ASSIGN(auto result_table,
                         DeclarationToTable(std::move(plan), /*use_threads=*/false));
    AssertTablesEqualIgnoringOrder(table, result_table);
  }
}

TEST(ExecPlanExecution, TableSourceSinkError) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  AsyncGenerator<std::optional<ExecBatch>> sink_gen;

  auto exp_batches = MakeBasicBatches();
  ASSERT_OK_AND_ASSIGN(auto table,
                       TableFromExecBatches(exp_batches.schema, exp_batches.batches));

  auto null_table_options = TableSourceNodeOptions{NULLPTR, 1};
  ASSERT_THAT(MakeExecNode("table_source", plan.get(), {}, null_table_options),
              Raises(StatusCode::Invalid, HasSubstr("not null")));

  auto negative_batch_size_options = TableSourceNodeOptions{table, -1};
  ASSERT_THAT(MakeExecNode("table_source", plan.get(), {}, negative_batch_size_options),
              Raises(StatusCode::Invalid, HasSubstr("batch_size > 0")));
}

template <typename ElementType, typename OptionsType>
void TestSourceSinkError(
    std::string source_factory_name,
    std::function<Result<std::vector<ElementType>>(const BatchesWithSchema&)>
        to_elements) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  std::shared_ptr<Schema> no_schema;

  auto exp_batches = MakeBasicBatches();
  ASSERT_OK_AND_ASSIGN(auto elements, to_elements(exp_batches));
  auto element_it_maker = [&elements]() {
    return MakeVectorIterator<ElementType>(elements);
  };

  auto null_executor_options = OptionsType{exp_batches.schema, element_it_maker};
  ASSERT_OK(MakeExecNode(source_factory_name, plan.get(), {}, null_executor_options));

  auto null_schema_options = OptionsType{no_schema, element_it_maker};
  ASSERT_THAT(MakeExecNode(source_factory_name, plan.get(), {}, null_schema_options),
              Raises(StatusCode::Invalid, HasSubstr("not null")));
}

template <typename ElementType, typename OptionsType>
void TestSourceSink(
    std::string source_factory_name,
    std::function<Result<std::vector<ElementType>>(const BatchesWithSchema&)>
        to_elements) {
  auto exp_batches = MakeBasicBatches();
  ASSERT_OK_AND_ASSIGN(auto elements, to_elements(exp_batches));
  auto element_it_maker = [&elements]() {
    return MakeVectorIterator<ElementType>(elements);
  };
  Declaration plan(source_factory_name,
                   OptionsType{exp_batches.schema, element_it_maker});
  ASSERT_OK_AND_ASSIGN(auto result,
                       DeclarationToExecBatches(std::move(plan), /*use_threads=*/false));
  AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, exp_batches.batches);
}

void TestRecordBatchReaderSourceSink(
    std::function<Result<std::shared_ptr<RecordBatchReader>>(const BatchesWithSchema&)>
        to_reader) {
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");
    auto exp_batches = MakeBasicBatches();
    ASSERT_OK_AND_ASSIGN(std::shared_ptr<RecordBatchReader> reader,
                         to_reader(exp_batches));
    RecordBatchReaderSourceNodeOptions options{reader};
    Declaration plan("record_batch_reader_source", std::move(options));
    ASSERT_OK_AND_ASSIGN(auto result, DeclarationToExecBatches(plan, parallel));
    AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches,
                                        exp_batches.batches);
  }
}

void TestRecordBatchReaderSourceSinkError(
    std::function<Result<std::shared_ptr<RecordBatchReader>>(const BatchesWithSchema&)>
        to_reader) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  auto source_factory_name = "record_batch_reader_source";
  auto exp_batches = MakeBasicBatches();
  ASSERT_OK_AND_ASSIGN(std::shared_ptr<RecordBatchReader> reader, to_reader(exp_batches));

  auto null_executor_options = RecordBatchReaderSourceNodeOptions{reader};
  ASSERT_OK(MakeExecNode(source_factory_name, plan.get(), {}, null_executor_options));

  std::shared_ptr<RecordBatchReader> no_reader;
  auto null_reader_options = RecordBatchReaderSourceNodeOptions{no_reader};
  ASSERT_THAT(MakeExecNode(source_factory_name, plan.get(), {}, null_reader_options),
              Raises(StatusCode::Invalid, HasSubstr("not null")));
}

TEST(ExecPlanExecution, ArrayVectorSourceSink) {
  TestSourceSink<std::shared_ptr<ArrayVector>, ArrayVectorSourceNodeOptions>(
      "array_vector_source", ToArrayVectors);
}

TEST(ExecPlanExecution, ArrayVectorSourceSinkError) {
  TestSourceSinkError<std::shared_ptr<ArrayVector>, ArrayVectorSourceNodeOptions>(
      "array_vector_source", ToArrayVectors);
}

TEST(ExecPlanExecution, ExecBatchSourceSink) {
  TestSourceSink<std::shared_ptr<ExecBatch>, ExecBatchSourceNodeOptions>(
      "exec_batch_source", ToExecBatches);
}

TEST(ExecPlanExecution, ExecBatchSourceSinkError) {
  TestSourceSinkError<std::shared_ptr<ExecBatch>, ExecBatchSourceNodeOptions>(
      "exec_batch_source", ToExecBatches);
}

TEST(ExecPlanExecution, RecordBatchSourceSink) {
  TestSourceSink<std::shared_ptr<RecordBatch>, RecordBatchSourceNodeOptions>(
      "record_batch_source", ToRecordBatches);
}

TEST(ExecPlanExecution, RecordBatchSourceSinkError) {
  TestSourceSinkError<std::shared_ptr<RecordBatch>, RecordBatchSourceNodeOptions>(
      "record_batch_source", ToRecordBatches);
}

TEST(ExecPlanExecution, RecordBatchReaderSourceSink) {
  TestRecordBatchReaderSourceSink(ToRecordBatchReader);
}

TEST(ExecPlanExecution, RecordBatchReaderSourceSinkError) {
  TestRecordBatchReaderSourceSinkError(ToRecordBatchReader);
}

TEST(ExecPlanExecution, SinkNodeBackpressure) {
  std::optional<ExecBatch> batch =
      ExecBatchFromJSON({int32(), boolean()},
                        "[[4, false], [5, null], [6, false], [7, false], [null, true]]");
  constexpr uint32_t kPauseIfAbove = 4;
  constexpr uint32_t kResumeIfBelow = 2;
  uint32_t pause_if_above_bytes =
      kPauseIfAbove * static_cast<uint32_t>(batch->TotalBufferSize());
  uint32_t resume_if_below_bytes =
      kResumeIfBelow * static_cast<uint32_t>(batch->TotalBufferSize());
  EXPECT_OK_AND_ASSIGN(std::shared_ptr<ExecPlan> plan, ExecPlan::Make());
  PushGenerator<std::optional<ExecBatch>> batch_producer;
  AsyncGenerator<std::optional<ExecBatch>> sink_gen;
  BackpressureMonitor* backpressure_monitor;
  BackpressureOptions backpressure_options(resume_if_below_bytes, pause_if_above_bytes);
  std::shared_ptr<Schema> schema_ = schema({field("data", uint32())});
  ARROW_EXPECT_OK(
      compute::Declaration::Sequence(
          {
              {"source", SourceNodeOptions(schema_, batch_producer)},
              {"sink", SinkNodeOptions{&sink_gen, /*schema=*/nullptr,
                                       backpressure_options, &backpressure_monitor}},
          })
          .AddToPlan(plan.get()));
  ASSERT_TRUE(backpressure_monitor);
  ARROW_EXPECT_OK(plan->StartProducing());

  ASSERT_FALSE(backpressure_monitor->is_paused());

  // Should be able to push kPauseIfAbove batches without triggering back pressure
  for (uint32_t i = 0; i < kPauseIfAbove; i++) {
    batch_producer.producer().Push(batch);
  }
  SleepABit();
  ASSERT_FALSE(backpressure_monitor->is_paused());

  // One more batch should trigger back pressure
  batch_producer.producer().Push(batch);
  BusyWait(10, [&] { return backpressure_monitor->is_paused(); });
  ASSERT_TRUE(backpressure_monitor->is_paused());

  // Reading as much as we can while keeping it paused
  for (uint32_t i = kPauseIfAbove; i >= kResumeIfBelow; i--) {
    ASSERT_FINISHES_OK(sink_gen());
  }
  SleepABit();
  ASSERT_TRUE(backpressure_monitor->is_paused());

  // Reading one more item should open up backpressure
  ASSERT_FINISHES_OK(sink_gen());
  BusyWait(10, [&] { return !backpressure_monitor->is_paused(); });
  ASSERT_FALSE(backpressure_monitor->is_paused());

  // Cleanup
  batch_producer.producer().Push(IterationEnd<std::optional<ExecBatch>>());
  plan->StopProducing();
  ASSERT_FINISHES_OK(plan->finished());
}

TEST(ExecPlan, ToString) {
  auto basic_data = MakeBasicBatches();
  AsyncGenerator<std::optional<ExecBatch>> sink_gen;

  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  ASSERT_OK(Declaration::Sequence(
                {
                    {"source", SourceNodeOptions{basic_data.schema,
                                                 basic_data.gen(/*parallel=*/false,
                                                                /*slow=*/false)}},
                    {"sink", SinkNodeOptions{&sink_gen}},
                })
                .AddToPlan(plan.get()));
  EXPECT_EQ(plan->sources()[0]->ToString(), R"(:SourceNode{})");
  EXPECT_EQ(plan->sinks()[0]->ToString(), R"(:SinkNode{})");
  EXPECT_EQ(plan->ToString(), R"(ExecPlan with 2 nodes:
:SinkNode{}
  :SourceNode{}
)");

  ASSERT_OK_AND_ASSIGN(plan, ExecPlan::Make());
  std::shared_ptr<CountOptions> options =
      std::make_shared<CountOptions>(CountOptions::ONLY_VALID);
  ASSERT_OK(
      Declaration::Sequence(
          {
              {"source",
               SourceNodeOptions{basic_data.schema,
                                 basic_data.gen(/*parallel=*/false, /*slow=*/false)},
               "custom_source_label"},
              {"filter", FilterNodeOptions{greater_equal(field_ref("i32"), literal(0))}},
              {"project", ProjectNodeOptions{{
                              field_ref("bool"),
                              call("multiply", {field_ref("i32"), literal(2)}),
                          }}},
              {"aggregate",
               AggregateNodeOptions{
                   /*aggregates=*/{
                       {"hash_sum", nullptr, "multiply(i32, 2)", "sum(multiply(i32, 2))"},
                       {"hash_count", options, "multiply(i32, 2)",
                        "count(multiply(i32, 2))"}},
                   /*keys=*/{"bool"}}},
              {"filter", FilterNodeOptions{greater(field_ref("sum(multiply(i32, 2))"),
                                                   literal(10))}},
              {"order_by_sink",
               OrderBySinkNodeOptions{
                   SortOptions({SortKey{"sum(multiply(i32, 2))", SortOrder::Ascending}}),
                   &sink_gen},
               "custom_sink_label"},
          })
          .AddToPlan(plan.get()));
  EXPECT_EQ(plan->ToString(), R"a(ExecPlan with 6 nodes:
custom_sink_label:OrderBySinkNode{by={sort_keys=[FieldRef.Name(sum(multiply(i32, 2))) ASC], null_placement=AtEnd}}
  :FilterNode{filter=(sum(multiply(i32, 2)) > 10)}
    :GroupByNode{keys=["bool"], aggregates=[
    	hash_sum(multiply(i32, 2)),
    	hash_count(multiply(i32, 2), {mode=NON_NULL}),
    ]}
      :ProjectNode{projection=[bool, multiply(i32, 2)]}
        :FilterNode{filter=(i32 >= 0)}
          custom_source_label:SourceNode{}
)a");

  ASSERT_OK_AND_ASSIGN(plan, ExecPlan::Make());

  Declaration union_node{"union", ExecNodeOptions{}};
  Declaration lhs{"source",
                  SourceNodeOptions{basic_data.schema,
                                    basic_data.gen(/*parallel=*/false, /*slow=*/false)}};
  lhs.label = "lhs";
  Declaration rhs{"source",
                  SourceNodeOptions{basic_data.schema,
                                    basic_data.gen(/*parallel=*/false, /*slow=*/false)}};
  rhs.label = "rhs";
  union_node.inputs.emplace_back(lhs);
  union_node.inputs.emplace_back(rhs);
  ASSERT_OK(
      Declaration::Sequence(
          {
              union_node,
              {"aggregate", AggregateNodeOptions{
                                /*aggregates=*/{{"count", options, "i32", "count(i32)"}},
                                /*keys=*/{}}},
              {"sink", SinkNodeOptions{&sink_gen}},
          })
          .AddToPlan(plan.get()));
  EXPECT_EQ(plan->ToString(), R"a(ExecPlan with 5 nodes:
:SinkNode{}
  :ScalarAggregateNode{aggregates=[
	count(i32, {mode=NON_NULL}),
]}
    :UnionNode{}
      rhs:SourceNode{}
      lhs:SourceNode{}
)a");
}

TEST(ExecPlanExecution, SourceOrderBy) {
  std::vector<ExecBatch> expected = {
      ExecBatchFromJSON({int32(), boolean()},
                        "[[4, false], [5, null], [6, false], [7, false], [null, true]]")};
  for (bool slow : {false, true}) {
    SCOPED_TRACE(slow ? "slowed" : "unslowed");

    for (bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "single threaded");

      ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
      AsyncGenerator<std::optional<ExecBatch>> sink_gen;

      auto basic_data = MakeBasicBatches();

      SortOptions options({SortKey("i32", SortOrder::Ascending)});
      ASSERT_OK(Declaration::Sequence(
                    {
                        {"source", SourceNodeOptions{basic_data.schema,
                                                     basic_data.gen(parallel, slow)}},
                        {"order_by_sink", OrderBySinkNodeOptions{options, &sink_gen}},
                    })
                    .AddToPlan(plan.get()));

      ASSERT_THAT(StartAndCollect(plan.get(), sink_gen),
                  Finishes(ResultWith(ElementsAreArray(expected))));
    }
  }
}

TEST(ExecPlanExecution, SourceSinkError) {
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  AsyncGenerator<std::optional<ExecBatch>> sink_gen;

  auto basic_data = MakeBasicBatches();
  auto it = basic_data.batches.begin();
  AsyncGenerator<std::optional<ExecBatch>> error_source_gen =
      [&]() -> Result<std::optional<ExecBatch>> {
    if (it == basic_data.batches.end()) {
      return Status::Invalid("Artificial error");
    }
    return std::make_optional(*it++);
  };

  ASSERT_OK(Declaration::Sequence(
                {
                    {"source", SourceNodeOptions{basic_data.schema, error_source_gen}},
                    {"sink", SinkNodeOptions{&sink_gen}},
                })
                .AddToPlan(plan.get()));

  ASSERT_THAT(StartAndCollect(plan.get(), sink_gen),
              Finishes(Raises(StatusCode::Invalid, HasSubstr("Artificial"))));
}

TEST(ExecPlanExecution, SourceConsumingSink) {
  for (bool slow : {false, true}) {
    SCOPED_TRACE(slow ? "slowed" : "unslowed");

    for (bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "single threaded");
      ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
      std::atomic<uint32_t> batches_seen{0};
      Future<> finish = Future<>::Make();
      struct TestConsumer : public SinkNodeConsumer {
        TestConsumer(std::atomic<uint32_t>* batches_seen, Future<> finish)
            : batches_seen(batches_seen), finish(std::move(finish)) {}

        Status Init(const std::shared_ptr<Schema>& schema,
                    BackpressureControl* backpressure_control, ExecPlan* plan) override {
          return Status::OK();
        }

        Status Consume(ExecBatch batch) override {
          (*batches_seen)++;
          return Status::OK();
        }

        Future<> Finish() override { return finish; }

        std::atomic<uint32_t>* batches_seen;
        Future<> finish;
      };
      std::shared_ptr<TestConsumer> consumer =
          std::make_shared<TestConsumer>(&batches_seen, finish);

      auto basic_data = MakeBasicBatches();
      ASSERT_OK_AND_ASSIGN(
          auto source, MakeExecNode("source", plan.get(), {},
                                    SourceNodeOptions(basic_data.schema,
                                                      basic_data.gen(parallel, slow))));
      ASSERT_OK(MakeExecNode("consuming_sink", plan.get(), {source},
                             ConsumingSinkNodeOptions(consumer)));
      ASSERT_OK(plan->StartProducing());
      // Source should finish fairly quickly
      ASSERT_FINISHES_OK(source->finished());
      SleepABit();
      // Consumer isn't finished and so plan shouldn't have finished
      AssertNotFinished(plan->finished());
      // Mark consumption complete, plan should finish
      finish.MarkFinished();
      ASSERT_FINISHES_OK(plan->finished());
      ASSERT_EQ(2, batches_seen);
    }
  }
}

TEST(ExecPlanExecution, SourceTableConsumingSink) {
  for (bool slow : {false, true}) {
    SCOPED_TRACE(slow ? "slowed" : "unslowed");

    for (bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "single threaded");
      ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());

      std::shared_ptr<Table> out = nullptr;

      auto basic_data = MakeBasicBatches();

      TableSinkNodeOptions options{&out};

      ASSERT_OK_AND_ASSIGN(
          auto source, MakeExecNode("source", plan.get(), {},
                                    SourceNodeOptions(basic_data.schema,
                                                      basic_data.gen(parallel, slow))));
      ASSERT_OK(MakeExecNode("table_sink", plan.get(), {source}, options));
      ASSERT_OK(plan->StartProducing());
      // Source should finish fairly quickly
      ASSERT_FINISHES_OK(source->finished());
      SleepABit();
      ASSERT_OK_AND_ASSIGN(auto expected,
                           TableFromExecBatches(basic_data.schema, basic_data.batches));
      ASSERT_FINISHES_OK(plan->finished());
      ASSERT_EQ(5, out->num_rows());
      AssertTablesEqualIgnoringOrder(expected, out);
    }
  }
}

TEST(ExecPlanExecution, ConsumingSinkNames) {
  struct SchemaKeepingConsumer : public SinkNodeConsumer {
    std::shared_ptr<Schema> schema_;
    Status Init(const std::shared_ptr<Schema>& schema,
                BackpressureControl* backpressure_control, ExecPlan* plan) override {
      schema_ = schema;
      return Status::OK();
    }
    Status Consume(ExecBatch batch) override { return Status::OK(); }
    Future<> Finish() override { return Future<>::MakeFinished(); }
  };
  std::vector<std::vector<std::string>> names_data = {{}, {"a", "b"}, {"a", "b", "c"}};
  for (const auto& names : names_data) {
    auto consumer = std::make_shared<SchemaKeepingConsumer>();
    ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
    auto basic_data = MakeBasicBatches();
    ASSERT_OK(Declaration::Sequence(
                  {{"source",
                    SourceNodeOptions(basic_data.schema, basic_data.gen(false, false))},
                   {"consuming_sink", ConsumingSinkNodeOptions(consumer, names)}})
                  .AddToPlan(plan.get()));
    ASSERT_OK_AND_ASSIGN(
        auto source,
        MakeExecNode("source", plan.get(), {},
                     SourceNodeOptions(basic_data.schema, basic_data.gen(false, false))));
    ASSERT_OK(MakeExecNode("consuming_sink", plan.get(), {source},
                           ConsumingSinkNodeOptions(consumer, names)));
    if (names.size() != 0 &&
        names.size() != static_cast<size_t>(basic_data.batches[0].num_values())) {
      ASSERT_RAISES(Invalid, plan->StartProducing());
    } else {
      auto expected_names = names.size() == 0 ? basic_data.schema->field_names() : names;
      ASSERT_OK(plan->StartProducing());
      ASSERT_FINISHES_OK(plan->finished());
      ASSERT_EQ(expected_names, consumer->schema_->field_names());
    }
  }
}

TEST(ExecPlanExecution, ConsumingSinkError) {
  struct InitErrorConsumer : public SinkNodeConsumer {
    Status Init(const std::shared_ptr<Schema>& schema,
                BackpressureControl* backpressure_control, ExecPlan* plan) override {
      return Status::Invalid("XYZ");
    }
    Status Consume(ExecBatch batch) override { return Status::OK(); }
    Future<> Finish() override { return Future<>::MakeFinished(); }
  };
  struct ConsumeErrorConsumer : public SinkNodeConsumer {
    Status Init(const std::shared_ptr<Schema>& schema,
                BackpressureControl* backpressure_control, ExecPlan* plan) override {
      return Status::OK();
    }
    Status Consume(ExecBatch batch) override { return Status::Invalid("XYZ"); }
    Future<> Finish() override { return Future<>::MakeFinished(); }
  };
  struct FinishErrorConsumer : public SinkNodeConsumer {
    Status Init(const std::shared_ptr<Schema>& schema,
                BackpressureControl* backpressure_control, ExecPlan* plan) override {
      return Status::OK();
    }
    Status Consume(ExecBatch batch) override { return Status::OK(); }
    Future<> Finish() override { return Future<>::MakeFinished(Status::Invalid("XYZ")); }
  };
  std::vector<std::shared_ptr<SinkNodeConsumer>> consumers{
      std::make_shared<InitErrorConsumer>(), std::make_shared<ConsumeErrorConsumer>(),
      std::make_shared<FinishErrorConsumer>()};

  for (auto& consumer : consumers) {
    auto basic_data = MakeBasicBatches();
    Declaration plan = Declaration::Sequence(
        {{"source", SourceNodeOptions(basic_data.schema, basic_data.gen(false, false))},
         {"consuming_sink", ConsumingSinkNodeOptions(consumer)}});
    // Since the source node is not parallel the entire plan is run during StartProducing
    ASSERT_RAISES(Invalid, DeclarationToStatus(std::move(plan)));
  }
}

TEST(ExecPlanExecution, StressSourceSink) {
  for (bool slow : {false, true}) {
    SCOPED_TRACE(slow ? "slowed" : "unslowed");

    for (bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "single threaded");

      int num_batches = (slow && !parallel) ? 30 : 300;

      auto random_data = MakeRandomBatches(
          schema({field("a", int32()), field("b", boolean())}), num_batches);
      Declaration plan("source", SourceNodeOptions{random_data.schema,
                                                   random_data.gen(parallel, slow)});
      ASSERT_OK_AND_ASSIGN(auto result,
                           DeclarationToExecBatches(std::move(plan), parallel));
      AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches,
                                          random_data.batches);
    }
  }
}

TEST(ExecPlanExecution, StressSourceOrderBy) {
  auto input_schema = schema({field("a", int32()), field("b", boolean())});
  for (bool slow : {false, true}) {
    SCOPED_TRACE(slow ? "slowed" : "unslowed");

    for (bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "single threaded");

      int num_batches = (slow && !parallel) ? 30 : 300;

      ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
      AsyncGenerator<std::optional<ExecBatch>> sink_gen;

      auto random_data = MakeRandomBatches(input_schema, num_batches);

      SortOptions options({SortKey("a", SortOrder::Ascending)});
      ASSERT_OK(Declaration::Sequence(
                    {
                        {"source", SourceNodeOptions{random_data.schema,
                                                     random_data.gen(parallel, slow)}},
                        {"order_by_sink", OrderBySinkNodeOptions{options, &sink_gen}},
                    })
                    .AddToPlan(plan.get()));

      // Check that data is sorted appropriately
      ASSERT_FINISHES_OK_AND_ASSIGN(auto exec_batches,
                                    StartAndCollect(plan.get(), sink_gen));
      ASSERT_OK_AND_ASSIGN(auto actual, TableFromExecBatches(input_schema, exec_batches));
      ASSERT_OK_AND_ASSIGN(auto original,
                           TableFromExecBatches(input_schema, random_data.batches));
      ASSERT_OK_AND_ASSIGN(auto sort_indices, SortIndices(original, options));
      ASSERT_OK_AND_ASSIGN(auto expected, Take(original, sort_indices));
      AssertSchemaEqual(actual->schema(), expected.table()->schema());
      AssertArraysEqual(*actual->column(0)->chunk(0),
                        *expected.table()->column(0)->chunk(0));
    }
  }
}

TEST(ExecPlanExecution, StressSourceGroupedSumStop) {
  auto input_schema = schema({field("a", int32()), field("b", boolean())});
  for (bool slow : {false, true}) {
    SCOPED_TRACE(slow ? "slowed" : "unslowed");

    for (bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "single threaded");

      int num_batches = (slow && !parallel) ? 30 : 300;

      ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
      AsyncGenerator<std::optional<ExecBatch>> sink_gen;

      auto random_data = MakeRandomBatches(input_schema, num_batches);

      SortOptions options({SortKey("a", SortOrder::Ascending)});
      ASSERT_OK(
          Declaration::Sequence(
              {
                  {"source", SourceNodeOptions{random_data.schema,
                                               random_data.gen(parallel, slow)}},
                  {"aggregate", AggregateNodeOptions{
                                    /*aggregates=*/{{"hash_sum", nullptr, "a", "sum(a)"}},
                                    /*keys=*/{"b"}}},
                  {"sink", SinkNodeOptions{&sink_gen}},
              })
              .AddToPlan(plan.get()));

      ASSERT_OK(plan->Validate());
      ASSERT_OK(plan->StartProducing());
      plan->StopProducing();
      ASSERT_FINISHES_OK(plan->finished());
    }
  }
}

TEST(ExecPlanExecution, StressSourceSinkStopped) {
  for (bool slow : {false, true}) {
    SCOPED_TRACE(slow ? "slowed" : "unslowed");

    for (bool parallel : {false, true}) {
      SCOPED_TRACE(parallel ? "parallel" : "single threaded");

      int num_batches = (slow && !parallel) ? 30 : 300;

      ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
      AsyncGenerator<std::optional<ExecBatch>> sink_gen;

      auto random_data = MakeRandomBatches(
          schema({field("a", int32()), field("b", boolean())}), num_batches);

      ASSERT_OK(Declaration::Sequence(
                    {
                        {"source", SourceNodeOptions{random_data.schema,
                                                     random_data.gen(parallel, slow)}},
                        {"sink", SinkNodeOptions{&sink_gen}},
                    })
                    .AddToPlan(plan.get()));

      ASSERT_OK(plan->Validate());
      ASSERT_OK(plan->StartProducing());

      EXPECT_THAT(sink_gen(), Finishes(ResultWith(Optional(random_data.batches[0]))));

      plan->StopProducing();
      ASSERT_THAT(plan->finished(), Finishes(Ok()));
    }
  }
}

TEST(ExecPlanExecution, SourceFilterSink) {
  auto basic_data = MakeBasicBatches();
  Declaration plan = Declaration::Sequence(
      {{"source", SourceNodeOptions{basic_data.schema, basic_data.gen(/*parallel=*/false,
                                                                      /*slow=*/false)}},
       {"filter", FilterNodeOptions{equal(field_ref("i32"), literal(6))}}});
  ASSERT_OK_AND_ASSIGN(auto result, DeclarationToExecBatches(std::move(plan)));
  auto exp_batches = {ExecBatchFromJSON({int32(), boolean()}, "[]"),
                      ExecBatchFromJSON({int32(), boolean()}, "[[6, false]]")};
  AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, exp_batches);
}

TEST(ExecPlanExecution, SourceProjectSink) {
  auto basic_data = MakeBasicBatches();
  Declaration plan = Declaration::Sequence(
      {{"source", SourceNodeOptions{basic_data.schema, basic_data.gen(/*parallel=*/false,
                                                                      /*slow=*/false)}},
       {"project", ProjectNodeOptions{{
                                          not_(field_ref("bool")),
                                          call("add", {field_ref("i32"), literal(1)}),
                                      },
                                      {"!bool", "i32 + 1"}}}});

  auto exp_batches = {
      ExecBatchFromJSON({boolean(), int32()}, "[[false, null], [true, 5]]"),
      ExecBatchFromJSON({boolean(), int32()}, "[[null, 6], [true, 7], [true, 8]]")};
  ASSERT_OK_AND_ASSIGN(auto result, DeclarationToExecBatches(std::move(plan)));
  AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, exp_batches);
}

namespace {

BatchesWithSchema MakeGroupableBatches(int multiplicity = 1) {
  BatchesWithSchema out;

  out.batches = {ExecBatchFromJSON({int32(), utf8()}, R"([
                   [12, "alfa"],
                   [7,  "beta"],
                   [3,  "alfa"]
                 ])"),
                 ExecBatchFromJSON({int32(), utf8()}, R"([
                   [-2, "alfa"],
                   [-1, "gama"],
                   [3,  "alfa"]
                 ])"),
                 ExecBatchFromJSON({int32(), utf8()}, R"([
                   [5,  "gama"],
                   [3,  "beta"],
                   [-8, "alfa"]
                 ])")};

  size_t batch_count = out.batches.size();
  for (int repeat = 1; repeat < multiplicity; ++repeat) {
    for (size_t i = 0; i < batch_count; ++i) {
      out.batches.push_back(out.batches[i]);
    }
  }

  out.schema = schema({field("i32", int32()), field("str", utf8())});

  return out;
}

}  // namespace

TEST(ExecPlanExecution, SourceGroupedSum) {
  std::shared_ptr<Schema> out_schema =
      schema({field("sum(i32)", int64()), field("str", utf8())});
  const std::shared_ptr<Table> expected_parallel =
      TableFromJSON(out_schema, {R"([[800, "alfa"], [1000, "beta"], [400, "gama"]])"});
  const std::shared_ptr<Table> expected_single =
      TableFromJSON(out_schema, {R"([[8, "alfa"], [10, "beta"], [4, "gama"]])"});

  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    auto input = MakeGroupableBatches(/*multiplicity=*/parallel ? 100 : 1);

    Declaration plan = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"aggregate",
          AggregateNodeOptions{/*aggregates=*/{{"hash_sum", nullptr, "i32", "sum(i32)"}},
                               /*keys=*/{"str"}}}});

    ASSERT_OK_AND_ASSIGN(std::shared_ptr<Table> actual,
                         DeclarationToTable(std::move(plan), parallel));

    auto expected = parallel ? expected_parallel : expected_single;

    AssertTablesEqualIgnoringOrder(expected, actual);
  }
}

TEST(ExecPlanExecution, SourceMinMaxScalar) {
  // Regression test for ARROW-16904
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    auto input = MakeGroupableBatches(/*multiplicity=*/parallel ? 100 : 1);
    auto minmax_opts = std::make_shared<ScalarAggregateOptions>();
    auto min_max_type = struct_({field("min", int32()), field("max", int32())});
    auto expected_table = TableFromJSON(schema({field("struct", min_max_type)}), {R"([
      [{"min": -8, "max": 12}]
    ])"});

    // NOTE: Test `ScalarAggregateNode` by omitting `keys` attribute
    Declaration plan = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"aggregate",
          AggregateNodeOptions{
              /*aggregates=*/{{"min_max", std::move(minmax_opts), "i32", "min_max"}},
              /*keys=*/{}}}});
    ASSERT_OK_AND_ASSIGN(auto result_table,
                         DeclarationToTable(std::move(plan), parallel));
    // No need to ignore order since there is only 1 row
    AssertTablesEqual(*result_table, *expected_table);
  }
}

TEST(ExecPlanExecution, NestedSourceFilter) {
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    auto input = MakeNestedBatches();
    auto expected_table = TableFromJSON(input.schema, {R"([])",
                                                       R"([
      [{"i32": 5, "bool": null}],
      [{"i32": 6, "bool": false}],
      [{"i32": 7, "bool": false}]
    ])"});

    Declaration plan = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"filter", FilterNodeOptions{greater_equal(field_ref(FieldRef("struct", "i32")),
                                                    literal(5))}}});
    ASSERT_OK_AND_ASSIGN(auto result_table,
                         DeclarationToTable(std::move(plan), parallel));
    AssertTablesEqual(*result_table, *expected_table);
  }
}

TEST(ExecPlanExecution, NestedSourceProjectGroupedSum) {
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    auto input = MakeNestedBatches();
    auto expected =
        TableFromJSON(schema({field("x", int64()), field("y", boolean())}), {R"([
      [null, true],
      [17, false],
      [5, null]
])"});

    Declaration plan = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"project", ProjectNodeOptions{{
                                            field_ref(FieldRef("struct", "i32")),
                                            field_ref(FieldRef("struct", "bool")),
                                        },
                                        {"i32", "bool"}}},
         {"aggregate",
          AggregateNodeOptions{/*aggregates=*/{{"hash_sum", nullptr, "i32", "sum(i32)"}},
                               /*keys=*/{"bool"}}}});

    ASSERT_OK_AND_ASSIGN(auto actual, DeclarationToTable(std::move(plan), parallel));
    AssertTablesEqualIgnoringOrder(expected, actual);
  }
}

TEST(ExecPlanExecution, SourceFilterProjectGroupedSumFilter) {
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    int batch_multiplicity = parallel ? 100 : 1;
    auto input = MakeGroupableBatches(/*multiplicity=*/batch_multiplicity);

    Declaration plan = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"filter", FilterNodeOptions{greater_equal(field_ref("i32"), literal(0))}},
         {"project", ProjectNodeOptions{{
                         field_ref("str"),
                         call("multiply", {field_ref("i32"), literal(2)}),
                     }}},
         {"aggregate",
          AggregateNodeOptions{/*aggregates=*/{{"hash_sum", nullptr, "multiply(i32, 2)",
                                                "sum(multiply(i32, 2))"}},
                               /*keys=*/{"str"}}},
         {"filter", FilterNodeOptions{greater(field_ref("sum(multiply(i32, 2))"),
                                              literal(10 * batch_multiplicity))}}});

    auto expected = TableFromJSON(schema({field("a", int64()), field("b", utf8())}),
                                  {parallel ? R"([[3600, "alfa"], [2000, "beta"]])"
                                            : R"([[36, "alfa"], [20, "beta"]])"});
    ASSERT_OK_AND_ASSIGN(auto actual, DeclarationToTable(std::move(plan), parallel));
    AssertTablesEqualIgnoringOrder(expected, actual);
  }
}

TEST(ExecPlanExecution, SourceFilterProjectGroupedSumOrderBy) {
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    int batch_multiplicity = parallel ? 100 : 1;
    auto input = MakeGroupableBatches(/*multiplicity=*/batch_multiplicity);

    ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
    AsyncGenerator<std::optional<ExecBatch>> sink_gen;

    SortOptions options({SortKey("str", SortOrder::Descending)});
    ASSERT_OK(
        Declaration::Sequence(
            {
                {"source",
                 SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
                {"filter",
                 FilterNodeOptions{greater_equal(field_ref("i32"), literal(0))}},
                {"project", ProjectNodeOptions{{
                                field_ref("str"),
                                call("multiply", {field_ref("i32"), literal(2)}),
                            }}},
                {"aggregate",
                 AggregateNodeOptions{
                     /*aggregates=*/{{"hash_sum", nullptr, "multiply(i32, 2)",
                                      "sum(multiply(i32, 2))"}},
                     /*keys=*/{"str"}}},
                {"filter", FilterNodeOptions{greater(field_ref("sum(multiply(i32, 2))"),
                                                     literal(10 * batch_multiplicity))}},
                {"order_by_sink", OrderBySinkNodeOptions{options, &sink_gen}},
            })
            .AddToPlan(plan.get()));

    ASSERT_THAT(StartAndCollect(plan.get(), sink_gen),
                Finishes(ResultWith(ElementsAreArray({ExecBatchFromJSON(
                    {int64(), utf8()}, parallel ? R"([[2000, "beta"], [3600, "alfa"]])"
                                                : R"([[20, "beta"], [36, "alfa"]])")}))));
  }
}

TEST(ExecPlanExecution, SourceFilterProjectGroupedSumTopK) {
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    int batch_multiplicity = parallel ? 100 : 1;
    auto input = MakeGroupableBatches(/*multiplicity=*/batch_multiplicity);

    ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
    AsyncGenerator<std::optional<ExecBatch>> sink_gen;

    SelectKOptions options = SelectKOptions::TopKDefault(/*k=*/1, {"str"});
    ASSERT_OK(Declaration::Sequence(
                  {
                      {"source", SourceNodeOptions{input.schema,
                                                   input.gen(parallel, /*slow=*/false)}},
                      {"project", ProjectNodeOptions{{
                                      field_ref("str"),
                                      call("multiply", {field_ref("i32"), literal(2)}),
                                  }}},
                      {"aggregate",
                       AggregateNodeOptions{
                           /*aggregates=*/{{"hash_sum", nullptr, "multiply(i32, 2)",
                                            "sum(multiply(i32, 2))"}},
                           /*keys=*/{"str"}}},
                      {"select_k_sink", SelectKSinkNodeOptions{options, &sink_gen}},
                  })
                  .AddToPlan(plan.get()));

    ASSERT_THAT(
        StartAndCollect(plan.get(), sink_gen),
        Finishes(ResultWith(ElementsAreArray({ExecBatchFromJSON(
            {int64(), utf8()}, parallel ? R"([[800, "gama"]])" : R"([[8, "gama"]])")}))));
  }
}

TEST(ExecPlanExecution, SourceScalarAggSink) {
  auto basic_data = MakeBasicBatches();

  Declaration plan = Declaration::Sequence(
      {{"source", SourceNodeOptions{basic_data.schema,
                                    basic_data.gen(/*parallel=*/false, /*slow=*/false)}},
       {"aggregate", AggregateNodeOptions{
                         /*aggregates=*/{{"sum", nullptr, "i32", "sum(i32)"},
                                         {"any", nullptr, "bool", "any(bool)"}},
                     }}});
  auto exp_batches = {ExecBatchFromJSON(
      {int64(), boolean()}, {ArgShape::SCALAR, ArgShape::SCALAR}, "[[22, true]]")};
  ASSERT_OK_AND_ASSIGN(auto result, DeclarationToExecBatches(std::move(plan)));
  AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, exp_batches);
}

TEST(ExecPlanExecution, AggregationPreservesOptions) {
  // ARROW-13638: aggregation nodes initialize per-thread kernel state lazily
  // and need to keep a copy/strong reference to function options
  {
    auto basic_data = MakeBasicBatches();
    Future<std::shared_ptr<Table>> table_future;
    {
      auto options = std::make_shared<TDigestOptions>(TDigestOptions::Defaults());
      Declaration plan = Declaration::Sequence(
          {{"source",
            SourceNodeOptions{basic_data.schema, basic_data.gen(/*parallel=*/false,
                                                                /*slow=*/false)}},
           {"aggregate", AggregateNodeOptions{
                             /*aggregates=*/{{"tdigest", options, "i32", "tdigest(i32)"}},
                         }}});
      table_future = DeclarationToTableAsync(std::move(plan));
    }

    std::shared_ptr<Table> expected =
        TableFromJSON(schema({field("tdigest(i32)", float64())}), {"[[5.5]]"});

    ASSERT_FINISHES_OK_AND_ASSIGN(std::shared_ptr<Table> actual, table_future);
    AssertTablesEqualIgnoringOrder(expected, actual);
  }
  {
    auto data = MakeGroupableBatches(/*multiplicity=*/100);
    Future<std::shared_ptr<Table>> table_future;
    {
      auto options = std::make_shared<CountOptions>(CountOptions::Defaults());
      Declaration plan = Declaration::Sequence(
          {{"source", SourceNodeOptions{data.schema, data.gen(/*parallel=*/false,
                                                              /*slow=*/false)}},
           {"aggregate", AggregateNodeOptions{/*aggregates=*/{{"hash_count", options,
                                                               "i32", "count(i32)"}},
                                              /*keys=*/{"str"}}}});
      table_future = DeclarationToTableAsync(std::move(plan));
    }

    std::shared_ptr<Table> expected =
        TableFromJSON(schema({field("count(i32)", int64()), field("str", utf8())}),
                      {R"([[500, "alfa"], [200, "beta"], [200, "gama"]])"});

    ASSERT_FINISHES_OK_AND_ASSIGN(std::shared_ptr<Table> actual, table_future);
    AssertTablesEqualIgnoringOrder(expected, actual);
  }
}

TEST(ExecPlanExecution, ScalarSourceScalarAggSink) {
  // ARROW-9056: scalar aggregation can be done over scalars, taking
  // into account batch.length > 1 (e.g. a partition column)
  BatchesWithSchema scalar_data;
  scalar_data.batches = {
      ExecBatchFromJSON({int32(), boolean()}, {ArgShape::SCALAR, ArgShape::SCALAR},
                        "[[5, false], [5, false], [5, false]]"),
      ExecBatchFromJSON({int32(), boolean()}, "[[5, true], [6, false], [7, true]]")};
  scalar_data.schema = schema({field("a", int32()), field("b", boolean())});

  // index can't be tested as it's order-dependent
  // mode/quantile can't be tested as they're technically vector kernels
  Declaration plan = Declaration::Sequence(
      {{"source", SourceNodeOptions{scalar_data.schema,
                                    scalar_data.gen(/*parallel=*/false, /*slow=*/false)}},
       {"aggregate", AggregateNodeOptions{
                         /*aggregates=*/{{"all", nullptr, "b", "all(b)"},
                                         {"any", nullptr, "b", "any(b)"},
                                         {"count", nullptr, "a", "count(a)"},
                                         {"mean", nullptr, "a", "mean(a)"},
                                         {"product", nullptr, "a", "product(a)"},
                                         {"stddev", nullptr, "a", "stddev(a)"},
                                         {"sum", nullptr, "a", "sum(a)"},
                                         {"tdigest", nullptr, "a", "tdigest(a)"},
                                         {"variance", nullptr, "a", "variance(a)"}}}}});

  auto exp_batches = {
      ExecBatchFromJSON(
          {boolean(), boolean(), int64(), float64(), int64(), float64(), int64(),
           float64(), float64()},
          {ArgShape::SCALAR, ArgShape::SCALAR, ArgShape::SCALAR, ArgShape::SCALAR,
           ArgShape::SCALAR, ArgShape::SCALAR, ArgShape::SCALAR, ArgShape::ARRAY,
           ArgShape::SCALAR},
          R"([[false, true, 6, 5.5, 26250, 0.7637626158259734, 33, 5.0, 0.5833333333333334]])"),
  };
  ASSERT_OK_AND_ASSIGN(auto result, DeclarationToExecBatches(std::move(plan)));
  AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, exp_batches);
}

TEST(ExecPlanExecution, ScalarSourceGroupedSum) {
  // ARROW-14630: ensure grouped aggregation with a scalar key/array input doesn't
  // error
  ASSERT_OK_AND_ASSIGN(auto plan, ExecPlan::Make());
  AsyncGenerator<std::optional<ExecBatch>> sink_gen;

  BatchesWithSchema scalar_data;
  scalar_data.batches = {
      ExecBatchFromJSON({int32(), boolean()}, {ArgShape::ARRAY, ArgShape::SCALAR},
                        "[[5, false], [6, false], [7, false]]"),
      ExecBatchFromJSON({int32(), boolean()}, {ArgShape::ARRAY, ArgShape::SCALAR},
                        "[[1, true], [2, true], [3, true]]"),
  };
  scalar_data.schema = schema({field("a", int32()), field("b", boolean())});

  SortOptions options({SortKey("b", SortOrder::Descending)});
  ASSERT_OK(
      Declaration::Sequence(
          {
              {"source",
               SourceNodeOptions{scalar_data.schema, scalar_data.gen(/*parallel=*/false,
                                                                     /*slow=*/false)}},
              {"aggregate", AggregateNodeOptions{/*aggregates=*/{{"hash_sum", nullptr,
                                                                  "a", "hash_sum(a)"}},
                                                 /*keys=*/{"b"}}},
              {"order_by_sink", OrderBySinkNodeOptions{options, &sink_gen}},
          })
          .AddToPlan(plan.get()));

  ASSERT_THAT(StartAndCollect(plan.get(), sink_gen),
              Finishes(ResultWith(UnorderedElementsAreArray({
                  ExecBatchFromJSON({int64(), boolean()}, R"([[6, true], [18, false]])"),
              }))));
}

TEST(ExecPlanExecution, SelfInnerHashJoinSink) {
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    auto input = MakeGroupableBatches();

    auto left = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"filter", FilterNodeOptions{greater_equal(field_ref("i32"), literal(-1))}}});

    auto right = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"filter", FilterNodeOptions{less_equal(field_ref("i32"), literal(2))}}});

    // left side: [3,  "alfa"], [3,  "alfa"], [12, "alfa"], [3,  "beta"], [7,  "beta"],
    // [-1, "gama"], [5,  "gama"]
    // right side: [-2, "alfa"], [-8, "alfa"], [-1, "gama"]

    HashJoinNodeOptions join_opts{JoinType::INNER,
                                  /*left_keys=*/{"str"},
                                  /*right_keys=*/{"str"}, literal(true), "l_", "r_"};

    auto plan = Declaration("hashjoin", {left, right}, std::move(join_opts));

    ASSERT_OK_AND_ASSIGN(auto result,
                         DeclarationToExecBatches(std::move(plan), parallel));

    std::vector<ExecBatch> expected = {
        ExecBatchFromJSON({int32(), utf8(), int32(), utf8()}, R"([
            [3, "alfa", -2, "alfa"], [3, "alfa", -8, "alfa"],
            [3, "alfa", -2, "alfa"], [3, "alfa", -8, "alfa"],
            [12, "alfa", -2, "alfa"], [12, "alfa", -8, "alfa"],
            [-1, "gama", -1, "gama"], [5, "gama", -1, "gama"]])")};

    AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, expected);
  }
}

TEST(ExecPlanExecution, SelfOuterHashJoinSink) {
  for (bool parallel : {false, true}) {
    SCOPED_TRACE(parallel ? "parallel/merged" : "serial");

    auto input = MakeGroupableBatches();

    auto left = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"filter", FilterNodeOptions{greater_equal(field_ref("i32"), literal(-1))}}});

    auto right = Declaration::Sequence(
        {{"source", SourceNodeOptions{input.schema, input.gen(parallel, /*slow=*/false)}},
         {"filter", FilterNodeOptions{less_equal(field_ref("i32"), literal(2))}}});

    // left side: [3,  "alfa"], [3,  "alfa"], [12, "alfa"], [3,  "beta"], [7,  "beta"],
    // [-1, "gama"], [5,  "gama"]
    // right side: [-2, "alfa"], [-8, "alfa"], [-1, "gama"]

    HashJoinNodeOptions join_opts{JoinType::FULL_OUTER,
                                  /*left_keys=*/{"str"},
                                  /*right_keys=*/{"str"}, literal(true), "l_", "r_"};

    auto plan = Declaration("hashjoin", {left, right}, std::move(join_opts));

    ASSERT_OK_AND_ASSIGN(auto result,
                         DeclarationToExecBatches(std::move(plan), parallel));

    std::vector<ExecBatch> expected = {
        ExecBatchFromJSON({int32(), utf8(), int32(), utf8()}, R"([
            [3, "alfa", -2, "alfa"], [3, "alfa", -8, "alfa"],
            [3, "alfa", -2, "alfa"], [3, "alfa", -8, "alfa"],
            [12, "alfa", -2, "alfa"], [12, "alfa", -8, "alfa"],
            [3,  "beta", null, null], [7,  "beta", null, null],
            [-1, "gama", -1, "gama"], [5, "gama", -1, "gama"]])")};

    AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, expected);
  }
}

TEST(ExecPlan, RecordBatchReaderSourceSink) {
  // set up a RecordBatchReader:
  auto input = MakeBasicBatches();

  RecordBatchVector batches;
  for (const ExecBatch& exec_batch : input.batches) {
    ASSERT_OK_AND_ASSIGN(auto batch, exec_batch.ToRecordBatch(input.schema));
    batches.push_back(std::move(batch));
  }

  ASSERT_OK_AND_ASSIGN(auto table, Table::FromRecordBatches(batches));
  std::shared_ptr<RecordBatchReader> reader = std::make_shared<TableBatchReader>(*table);

  // Map the RecordBatchReader to a SourceNode
  ASSERT_OK_AND_ASSIGN(
      auto batch_gen,
      MakeReaderGenerator(std::move(reader), arrow::io::internal::GetIOThreadPool()));

  Declaration plan =
      Declaration::Sequence({{"source", SourceNodeOptions{table->schema(), batch_gen}}});
  ASSERT_OK_AND_ASSIGN(auto result, DeclarationToExecBatches(std::move(plan)));
  AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, input.batches);
}

TEST(ExecPlan, SourceEnforcesBatchLimit) {
  auto random_data = MakeRandomBatches(
      schema({field("a", int32()), field("b", boolean())}), /*num_batches=*/3,
      /*batch_size=*/static_cast<int32_t>(std::floor(ExecPlan::kMaxBatchSize * 3.5)));

  Declaration plan = Declaration::Sequence(
      {{"source",
        SourceNodeOptions{random_data.schema,
                          random_data.gen(/*parallel=*/false, /*slow=*/false)}}});
  ASSERT_OK_AND_ASSIGN(auto result, DeclarationToExecBatches(std::move(plan)));
  AssertExecBatchesEqualIgnoringOrder(result.schema, result.batches, random_data.batches);
  for (const auto& batch : result.batches) {
    ASSERT_LE(batch.length, ExecPlan::kMaxBatchSize);
  }
}

}  // namespace compute
}  // namespace arrow
