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

#include "arrow/adapters/orc/adapter.h"

#include <gtest/gtest.h>

#include <orc/OrcFile.hh>
#include <string>

#include "arrow/adapters/orc/adapter_util.h"
#include "arrow/api.h"
#include "arrow/array.h"
#include "arrow/compute/cast.h"
#include "arrow/io/api.h"
#include "arrow/io/interfaces.h"
#include "arrow/io/memory.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/testing/random.h"
#include "arrow/type.h"
#include "arrow/util/decimal.h"

namespace liborc = orc;

namespace arrow {

constexpr int kDefaultSmallMemStreamSize = 16384 * 5;  // 80KB
constexpr int kDefaultMemStreamSize = 10 * 1024 * 1024;
static constexpr random::SeedType kRandomSeed = 0x0ff1ce;

using ArrayBuilderVector = std::vector<std::shared_ptr<ArrayBuilder>>;
using ArrayBuilderMatrix = std::vector<ArrayBuilderVector>;
using ArrayMatrix = std::vector<ArrayVector>;
using BufferVector = std::vector<std::shared_ptr<Buffer>>;
using BufferMatrix = std::vector<BufferVector>;
using BufferBuilderVector = std::vector<std::shared_ptr<BufferBuilder>>;
using BufferBuilderMatrix = std::vector<BufferBuilderVector>;

class MemoryOutputStream : public liborc::OutputStream {
 public:
  explicit MemoryOutputStream(ssize_t capacity)
      : data_(capacity), name_("MemoryOutputStream"), length_(0) {}

  uint64_t getLength() const override { return length_; }

  uint64_t getNaturalWriteSize() const override { return natural_write_size_; }

  void write(const void* buf, size_t size) override {
    memcpy(data_.data() + length_, buf, size);
    length_ += size;
  }

  const std::string& getName() const override { return name_; }

  const char* getData() const { return data_.data(); }

  void close() override {}

  void reset() { length_ = 0; }

 private:
  std::vector<char> data_;
  std::string name_;
  uint64_t length_, natural_write_size_;
};

template <typename T, typename U>
void randintpartition(int64_t n, T sum, std::vector<U>* out) {
  const int random_seed = 0;
  std::default_random_engine gen(random_seed);
  out->resize(n, static_cast<T>(0));
  T remaining_sum = sum;
  std::generate(out->begin(), out->end() - 1, [&gen, &remaining_sum] {
    std::uniform_int_distribution<T> d(static_cast<T>(0), remaining_sum);
    auto res = d(gen);
    remaining_sum -= res;
    return static_cast<U>(res);
  });
  (*out)[n - 1] += remaining_sum;
  std::random_shuffle(out->begin(), out->end());
}

std::shared_ptr<ChunkedArray> GenerateRandomChunkedArray(
    const std::shared_ptr<DataType>& data_type, int64_t size, int64_t min_num_chunks,
    int64_t max_num_chunks, double null_probability) {
  arrow::random::RandomArrayGenerator rand(kRandomSeed);
  std::vector<int64_t> num_chunks(1, 0);
  std::vector<int64_t> current_size_chunks;
  arrow::randint<int64_t, int64_t>(1, min_num_chunks, max_num_chunks, &num_chunks);
  int64_t current_num_chunks = num_chunks[0];
  ArrayVector arrays(current_num_chunks, nullptr);
  randintpartition(current_num_chunks, size, &current_size_chunks);
  for (int j = 0; j < current_num_chunks; j++) {
    arrays[j] = rand.ArrayOf(data_type, current_size_chunks[j], null_probability);
  }
  return std::make_shared<ChunkedArray>(arrays);
}

std::shared_ptr<Table> GenerateRandomTable(const std::shared_ptr<Schema>& schema,
                                           int64_t size, int64_t min_num_chunks,
                                           int64_t max_num_chunks,
                                           double null_probability) {
  int num_cols = schema->num_fields();
  ChunkedArrayVector cv;
  for (int col = 0; col < num_cols; col++) {
    cv.push_back(GenerateRandomChunkedArray(schema->field(col)->type(), size,
                                            min_num_chunks, max_num_chunks,
                                            null_probability));
  }
  return Table::Make(schema, cv);
}

void AssertTableWriteReadEqual(const std::shared_ptr<Table>& input_table,
                               const std::shared_ptr<Table>& expected_output_table,
                               const int64_t max_size = kDefaultSmallMemStreamSize) {
  std::shared_ptr<io::BufferOutputStream> buffer_output_stream =
      io::BufferOutputStream::Create(max_size).ValueOrDie();
  std::unique_ptr<adapters::orc::ORCFileWriter> writer =
      adapters::orc::ORCFileWriter::Open(*buffer_output_stream).ValueOrDie();
  ARROW_EXPECT_OK(writer->Write(*input_table));
  ARROW_EXPECT_OK(writer->Close());
  std::shared_ptr<Buffer> buffer = buffer_output_stream->Finish().ValueOrDie();
  std::shared_ptr<io::RandomAccessFile> in_stream(new io::BufferReader(buffer));
  std::unique_ptr<adapters::orc::ORCFileReader> reader;
  ARROW_EXPECT_OK(
      adapters::orc::ORCFileReader::Open(in_stream, default_memory_pool(), &reader));
  std::shared_ptr<Table> actual_output_table;
  ARROW_EXPECT_OK(reader->Read(&actual_output_table));
  AssertTablesEqual(*actual_output_table, *expected_output_table, false, false);
}

void SchemaORCWriteReadTest(const std::shared_ptr<Schema>& schema, int64_t size,
                            int64_t min_num_chunks, int64_t max_num_chunks,
                            double null_probability,
                            int64_t max_size = kDefaultSmallMemStreamSize) {
  const std::shared_ptr<Table> table =
      GenerateRandomTable(schema, size, min_num_chunks, max_num_chunks, null_probability);
  AssertTableWriteReadEqual(table, table, max_size);
}

std::unique_ptr<liborc::Writer> CreateWriter(uint64_t stripe_size,
                                             const liborc::Type& type,
                                             liborc::OutputStream* stream) {
  liborc::WriterOptions options;
  options.setStripeSize(stripe_size);
  options.setCompressionBlockSize(1024);
  options.setMemoryPool(liborc::getDefaultPool());
  options.setRowIndexStride(0);
  return liborc::createWriter(type, stream, options);
}

TEST(TestAdapterRead, readIntAndStringFileMultipleStripes) {
  MemoryOutputStream mem_stream(kDefaultMemStreamSize);
  ORC_UNIQUE_PTR<liborc::Type> type(
      liborc::Type::buildTypeFromString("struct<col1:int,col2:string>"));

  constexpr uint64_t stripe_size = 1024;  // 1K
  constexpr uint64_t stripe_count = 10;
  constexpr uint64_t stripe_row_count = 65535;
  constexpr uint64_t reader_batch_size = 1024;

  auto writer = CreateWriter(stripe_size, *type, &mem_stream);
  auto batch = writer->createRowBatch(stripe_row_count);
  auto struct_batch = internal::checked_cast<liborc::StructVectorBatch*>(batch.get());
  auto long_batch =
      internal::checked_cast<liborc::LongVectorBatch*>(struct_batch->fields[0]);
  auto str_batch =
      internal::checked_cast<liborc::StringVectorBatch*>(struct_batch->fields[1]);
  int64_t accumulated = 0;

  for (uint64_t j = 0; j < stripe_count; ++j) {
    char data_buffer[327675];
    uint64_t offset = 0;
    for (uint64_t i = 0; i < stripe_row_count; ++i) {
      std::string str_data = std::to_string(accumulated % stripe_row_count);
      long_batch->data[i] = static_cast<int64_t>(accumulated % stripe_row_count);
      str_batch->data[i] = data_buffer + offset;
      str_batch->length[i] = static_cast<int64_t>(str_data.size());
      memcpy(data_buffer + offset, str_data.c_str(), str_data.size());
      accumulated++;
      offset += str_data.size();
    }
    struct_batch->numElements = stripe_row_count;
    long_batch->numElements = stripe_row_count;
    str_batch->numElements = stripe_row_count;

    writer->add(*batch);
  }

  writer->close();

  std::shared_ptr<io::RandomAccessFile> in_stream(new io::BufferReader(
      std::make_shared<Buffer>(reinterpret_cast<const uint8_t*>(mem_stream.getData()),
                               static_cast<int64_t>(mem_stream.getLength()))));

  std::unique_ptr<adapters::orc::ORCFileReader> reader;
  ASSERT_TRUE(
      adapters::orc::ORCFileReader::Open(in_stream, default_memory_pool(), &reader).ok());

  ASSERT_EQ(stripe_row_count * stripe_count, reader->NumberOfRows());
  ASSERT_EQ(stripe_count, reader->NumberOfStripes());
  accumulated = 0;
  std::shared_ptr<RecordBatchReader> stripe_reader;
  EXPECT_TRUE(reader->NextStripeReader(reader_batch_size, &stripe_reader).ok());
  while (stripe_reader) {
    std::shared_ptr<RecordBatch> record_batch;
    EXPECT_TRUE(stripe_reader->ReadNext(&record_batch).ok());
    while (record_batch) {
      auto int32_array = std::static_pointer_cast<Int32Array>(record_batch->column(0));
      auto str_array = std::static_pointer_cast<StringArray>(record_batch->column(1));
      for (int j = 0; j < record_batch->num_rows(); ++j) {
        EXPECT_EQ(accumulated % stripe_row_count, int32_array->Value(j));
        EXPECT_EQ(std::to_string(accumulated % stripe_row_count),
                  str_array->GetString(j));
        accumulated++;
      }
      EXPECT_TRUE(stripe_reader->ReadNext(&record_batch).ok());
    }
    EXPECT_TRUE(reader->NextStripeReader(reader_batch_size, &stripe_reader).ok());
  }

  // test seek operation
  int64_t start_offset = 830;
  EXPECT_TRUE(reader->Seek(stripe_row_count + start_offset).ok());

  EXPECT_TRUE(reader->NextStripeReader(reader_batch_size, &stripe_reader).ok());
  std::shared_ptr<RecordBatch> record_batch;
  EXPECT_TRUE(stripe_reader->ReadNext(&record_batch).ok());
  while (record_batch) {
    auto int32_array = std::dynamic_pointer_cast<Int32Array>(record_batch->column(0));
    auto str_array = std::dynamic_pointer_cast<StringArray>(record_batch->column(1));
    for (int j = 0; j < record_batch->num_rows(); ++j) {
      std::ostringstream os;
      os << start_offset % stripe_row_count;
      EXPECT_EQ(start_offset % stripe_row_count, int32_array->Value(j));
      EXPECT_EQ(os.str(), str_array->GetString(j));
      start_offset++;
    }
    EXPECT_TRUE(stripe_reader->ReadNext(&record_batch).ok());
  }
}

// WriteORC tests

// Trivial
TEST(TestAdapterWriteTrivial, writeZeroRowsNoConversion) {
  std::shared_ptr<Table> table = TableFromJSON(
      schema({field("bool", boolean()), field("int8", int8()), field("int16", int16()),
              field("int32", int32()), field("int64", int64()), field("float", float32()),
              field("double", float64()), field("decimal128nz", decimal(25, 6)),
              field("decimal128z", decimal(32, 0)), field("date32", date32()),
              field("ts3", timestamp(TimeUnit::NANO)), field("string", utf8()),
              field("binary", binary()),
              field("struct", struct_({field("a", utf8()), field("b", int64())})),
              field("list", list(int32())),
              field("lsl", list(struct_({field("lsl0", list(int32()))}))),
              field("map", map(utf8(), utf8()))}),
      {R"([])"});
  AssertTableWriteReadEqual(table, table, kDefaultSmallMemStreamSize / 16);
}
TEST(TestAdapterWriteTrivial, writeChunklessNoConversion) {
  std::shared_ptr<Table> table = TableFromJSON(
      schema({field("bool", boolean()), field("int8", int8()), field("int16", int16()),
              field("int32", int32()), field("int64", int64()), field("float", float32()),
              field("double", float64()), field("decimal128nz", decimal(25, 6)),
              field("decimal128z", decimal(32, 0)), field("date32", date32()),
              field("ts3", timestamp(TimeUnit::NANO)), field("string", utf8()),
              field("binary", binary()),
              field("struct", struct_({field("a", utf8()), field("b", int64())})),
              field("list", list(int32())),
              field("lsl", list(struct_({field("lsl0", list(int32()))}))),
              field("map", map(utf8(), utf8()))}),
      {});
  AssertTableWriteReadEqual(table, table, kDefaultSmallMemStreamSize / 16);
}
TEST(TestAdapterWriteTrivial, writeZeroRowsWithConversion) {
  std::shared_ptr<Table>
      input_table = TableFromJSON(
          schema({field("date64", date64()), field("ts0", timestamp(TimeUnit::SECOND)),
                  field("ts1", timestamp(TimeUnit::MILLI)),
                  field("ts2", timestamp(TimeUnit::MICRO)),
                  field("large_string", large_utf8()),
                  field("large_binary", large_binary()),
                  field("fixed_size_binary0", fixed_size_binary(0)),
                  field("fixed_size_binary", fixed_size_binary(5)),
                  field("large_list", large_list(int32())),
                  field("fixed_size_list", fixed_size_list(int32(), 3))}),
          {R"([])"}),
      expected_output_table = TableFromJSON(
          schema({field("date64", timestamp(TimeUnit::NANO)),
                  field("ts0", timestamp(TimeUnit::NANO)),
                  field("ts1", timestamp(TimeUnit::NANO)),
                  field("ts2", timestamp(TimeUnit::NANO)), field("large_string", utf8()),
                  field("large_binary", binary()), field("fixed_size_binary0", binary()),
                  field("fixed_size_binary", binary()),
                  field("large_list", list(int32())),
                  field("fixed_size_list", list(int32()))}),
          {R"([])"});
  AssertTableWriteReadEqual(input_table, expected_output_table,
                            kDefaultSmallMemStreamSize / 16);
}
TEST(TestAdapterWriteTrivial, writeChunklessWithConversion) {
  std::shared_ptr<Table>
      input_table = TableFromJSON(
          schema({field("date64", date64()), field("ts0", timestamp(TimeUnit::SECOND)),
                  field("ts1", timestamp(TimeUnit::MILLI)),
                  field("ts2", timestamp(TimeUnit::MICRO)),
                  field("large_string", large_utf8()),
                  field("large_binary", large_binary()),
                  field("fixed_size_binary0", fixed_size_binary(0)),
                  field("fixed_size_binary", fixed_size_binary(5)),
                  field("large_list", large_list(int32())),
                  field("fixed_size_list", fixed_size_list(int32(), 3))}),
          {}),
      expected_output_table = TableFromJSON(
          schema({field("date64", timestamp(TimeUnit::NANO)),
                  field("ts0", timestamp(TimeUnit::NANO)),
                  field("ts1", timestamp(TimeUnit::NANO)),
                  field("ts2", timestamp(TimeUnit::NANO)), field("large_string", utf8()),
                  field("large_binary", binary()), field("fixed_size_binary0", binary()),
                  field("fixed_size_binary", binary()),
                  field("large_list", list(int32())),
                  field("fixed_size_list", list(int32()))}),
          {});
  AssertTableWriteReadEqual(input_table, expected_output_table,
                            kDefaultSmallMemStreamSize / 16);
}

// General
TEST(TestAdapterWriteGeneral, writeCombined) {
  std::shared_ptr<Schema> table_schema =
      schema({field("bool", boolean()), field("int8", int8()), field("int16", int16()),
              field("int32", int32()), field("int64", int64()), field("float", float32()),
              field("double", float64()), field("decimal128nz", decimal(33, 4)),
              field("decimal128z", decimal(35, 0)), field("date32", date32()),
              field("ts3", timestamp(TimeUnit::NANO)), field("string", utf8()),
              field("binary", binary())});
  SchemaORCWriteReadTest(table_schema, 10030, 1, 10, 0, kDefaultSmallMemStreamSize * 5);
  SchemaORCWriteReadTest(table_schema, 9405, 5, 20, 0.6, kDefaultSmallMemStreamSize * 5);
  SchemaORCWriteReadTest(table_schema, 4006, 10, 40, 1);
}

// Converts
// Since Arrow has way more types than ORC type conversions are unavoidable
TEST(TestAdapterWriteConvert, writeAllNulls) {
  std::vector<std::shared_ptr<Field>> input_fields{
      field("date64", date64()),
      field("ts0", timestamp(TimeUnit::SECOND)),
      field("ts1", timestamp(TimeUnit::MILLI)),
      field("ts2", timestamp(TimeUnit::MICRO)),
      field("large_string", large_utf8()),
      field("large_binary", large_binary()),
      field("fixed_size_binary0", fixed_size_binary(0)),
      field("fixed_size_binary", fixed_size_binary(5))},
      output_fields{field("date64", timestamp(TimeUnit::NANO)),
                    field("ts0", timestamp(TimeUnit::NANO)),
                    field("ts1", timestamp(TimeUnit::NANO)),
                    field("ts2", timestamp(TimeUnit::NANO)),
                    field("large_string", utf8()),
                    field("large_binary", binary()),
                    field("fixed_size_binary0", binary()),
                    field("fixed_size_binary", binary())};
  std::shared_ptr<Schema> input_schema = std::make_shared<Schema>(input_fields),
                          output_schema = std::make_shared<Schema>(output_fields);

  int64_t num_rows = 10000;
  int64_t numCols = input_fields.size();

  ArrayBuilderMatrix buildersIn(numCols, ArrayBuilderVector(5, NULLPTR));
  ArrayBuilderVector buildersOut(numCols, NULLPTR);

  for (int i = 0; i < 5; i++) {
    buildersIn[0][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<Date64Builder>());
    buildersIn[1][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::SECOND), default_memory_pool()));
    buildersIn[2][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::MILLI), default_memory_pool()));
    buildersIn[3][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::MICRO), default_memory_pool()));
    buildersIn[4][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<LargeStringBuilder>());
    buildersIn[5][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<LargeBinaryBuilder>());
    buildersIn[6][i] = std::static_pointer_cast<ArrayBuilder>(
        std::make_shared<FixedSizeBinaryBuilder>(fixed_size_binary(0)));
    buildersIn[7][i] = std::static_pointer_cast<ArrayBuilder>(
        std::make_shared<FixedSizeBinaryBuilder>(fixed_size_binary(5)));
  }

  for (int col = 0; col < 4; col++) {
    buildersOut[col] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::NANO), default_memory_pool()));
  }
  buildersOut[4] =
      std::static_pointer_cast<ArrayBuilder>(std::make_shared<StringBuilder>());
  for (int col = 5; col < 8; col++) {
    buildersOut[col] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<BinaryBuilder>());
  }

  for (int i = 0; i < num_rows; i++) {
    int chunk = i < (num_rows / 2) ? 1 : 3;
    for (int col = 0; col < numCols; col++) {
      ARROW_EXPECT_OK(buildersIn[col][chunk]->AppendNull());
      ARROW_EXPECT_OK(buildersOut[col]->AppendNull());
    }
  }

  ArrayMatrix arraysIn(numCols, ArrayVector(5, NULLPTR));
  ArrayVector arraysOut(numCols, NULLPTR);

  ChunkedArrayVector cvIn, cvOut;
  cvIn.reserve(numCols);
  cvOut.reserve(numCols);

  for (int col = 0; col < numCols; col++) {
    for (int i = 0; i < 5; i++) {
      ARROW_EXPECT_OK(buildersIn[col][i]->Finish(&arraysIn[col][i]));
    }
    ARROW_EXPECT_OK(buildersOut[col]->Finish(&arraysOut[col]));
    cvIn.push_back(std::make_shared<ChunkedArray>(arraysIn[col]));
    cvOut.push_back(std::make_shared<ChunkedArray>(arraysOut[col]));
  }

  std::shared_ptr<Table> input_table = Table::Make(input_schema, cvIn),
                         expected_output_table = Table::Make(output_schema, cvOut);
  AssertTableWriteReadEqual(input_table, expected_output_table);
}
TEST(TestAdapterWriteConvert, writeNoNulls) {
  std::vector<std::shared_ptr<Field>> input_fields{
      field("date64", date64()),
      field("ts0", timestamp(TimeUnit::SECOND)),
      field("ts1", timestamp(TimeUnit::MILLI)),
      field("ts2", timestamp(TimeUnit::MICRO)),
      field("large_string", large_utf8()),
      field("large_binary", large_binary()),
      field("fixed_size_binary0", fixed_size_binary(0)),
      field("fixed_size_binary", fixed_size_binary(2))},
      output_fields{field("date64", timestamp(TimeUnit::NANO)),
                    field("ts0", timestamp(TimeUnit::NANO)),
                    field("ts1", timestamp(TimeUnit::NANO)),
                    field("ts2", timestamp(TimeUnit::NANO)),
                    field("large_string", utf8()),
                    field("large_binary", binary()),
                    field("fixed_size_binary0", binary()),
                    field("fixed_size_binary", binary())};
  std::shared_ptr<Schema> input_schema = std::make_shared<Schema>(input_fields),
                          output_schema = std::make_shared<Schema>(output_fields);

  int64_t num_rows = 10000;
  int64_t numCols = input_fields.size();

  ArrayBuilderMatrix buildersIn(numCols, ArrayBuilderVector(5, NULLPTR));
  ArrayBuilderVector buildersOut(numCols, NULLPTR);

  for (int i = 0; i < 5; i++) {
    buildersIn[0][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<Date64Builder>());
    buildersIn[1][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::SECOND), default_memory_pool()));
    buildersIn[2][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::MILLI), default_memory_pool()));
    buildersIn[3][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::MICRO), default_memory_pool()));
    buildersIn[4][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<LargeStringBuilder>());
    buildersIn[5][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<LargeBinaryBuilder>());
    buildersIn[6][i] = std::static_pointer_cast<ArrayBuilder>(
        std::make_shared<FixedSizeBinaryBuilder>(fixed_size_binary(0)));
    buildersIn[7][i] = std::static_pointer_cast<ArrayBuilder>(
        std::make_shared<FixedSizeBinaryBuilder>(fixed_size_binary(2)));
  }

  for (int col = 0; col < 4; col++) {
    buildersOut[col] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::NANO), default_memory_pool()));
  }
  buildersOut[4] =
      std::static_pointer_cast<ArrayBuilder>(std::make_shared<StringBuilder>());
  for (int col = 5; col < 8; col++) {
    buildersOut[col] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<BinaryBuilder>());
  }

  char bin1[2], bin2[3], bin3[] = "", string_[12];
  std::string str;

  for (int64_t i = 0; i < num_rows; i++) {
    int chunk = i < (num_rows / 2) ? 1 : 3;
    bin1[0] = i % 256;
    bin1[1] = (i + 1) % 256;
    bin2[0] = (2 * i) % 256;
    bin2[1] = (2 * i + 1) % 256;
    bin2[2] = (i - 1) % 256;
    str = "Arrow " + std::to_string(2 * i);
    snprintf(string_, sizeof(string_), "%s", str.c_str());
    ARROW_EXPECT_OK(std::static_pointer_cast<Date64Builder>(buildersIn[0][chunk])
                        ->Append(INT64_C(1605758461555) + i));
    ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersIn[1][chunk])
                        ->Append(INT64_C(1605758461) + i));
    ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersIn[2][chunk])
                        ->Append(INT64_C(1605758461000) + i));
    ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersIn[3][chunk])
                        ->Append(INT64_C(1605758461000111) + i));
    ARROW_EXPECT_OK(std::static_pointer_cast<LargeStringBuilder>(buildersIn[4][chunk])
                        ->Append(string_));
    ARROW_EXPECT_OK(std::static_pointer_cast<LargeBinaryBuilder>(buildersIn[5][chunk])
                        ->Append(bin2, 3));
    ARROW_EXPECT_OK(std::static_pointer_cast<FixedSizeBinaryBuilder>(buildersIn[6][chunk])
                        ->Append(bin3));
    ARROW_EXPECT_OK(std::static_pointer_cast<FixedSizeBinaryBuilder>(buildersIn[7][chunk])
                        ->Append(bin1));
    ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersOut[0])
                        ->Append(INT64_C(1605758461555000000) + INT64_C(1000000) * i));
    ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersOut[1])
                        ->Append(INT64_C(1605758461000000000) + INT64_C(1000000000) * i));
    ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersOut[2])
                        ->Append(INT64_C(1605758461000000000) + INT64_C(1000000) * i));
    ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersOut[3])
                        ->Append(INT64_C(1605758461000111000) + INT64_C(1000) * i));
    ARROW_EXPECT_OK(
        std::static_pointer_cast<StringBuilder>(buildersOut[4])->Append(string_));
    ARROW_EXPECT_OK(
        std::static_pointer_cast<BinaryBuilder>(buildersOut[5])->Append(bin2, 3));
    ARROW_EXPECT_OK(
        std::static_pointer_cast<BinaryBuilder>(buildersOut[6])->Append(bin3, 0));
    ARROW_EXPECT_OK(
        std::static_pointer_cast<BinaryBuilder>(buildersOut[7])->Append(bin1, 2));
  }

  ArrayMatrix arraysIn(numCols, ArrayVector(5, NULLPTR));
  ArrayVector arraysOut(numCols, NULLPTR);

  ChunkedArrayVector cvIn, cvOut;
  cvIn.reserve(numCols);
  cvOut.reserve(numCols);

  for (int col = 0; col < numCols; col++) {
    for (int i = 0; i < 5; i++) {
      ARROW_EXPECT_OK(buildersIn[col][i]->Finish(&arraysIn[col][i]));
    }
    ARROW_EXPECT_OK(buildersOut[col]->Finish(&arraysOut[col]));
    cvIn.push_back(std::make_shared<ChunkedArray>(arraysIn[col]));
    cvOut.push_back(std::make_shared<ChunkedArray>(arraysOut[col]));
  }

  std::shared_ptr<Table> input_table = Table::Make(input_schema, cvIn),
                         expected_output_table = Table::Make(output_schema, cvOut);
  AssertTableWriteReadEqual(input_table, expected_output_table);
}
TEST(TestAdapterWriteConvert, writeMixed) {
  std::vector<std::shared_ptr<Field>> input_fields{
      field("date64", date64()),
      field("ts0", timestamp(TimeUnit::SECOND)),
      field("ts1", timestamp(TimeUnit::MILLI)),
      field("ts2", timestamp(TimeUnit::MICRO)),
      field("large_string", large_utf8()),
      field("large_binary", large_binary()),
      field("fixed_size_binary0", fixed_size_binary(0)),
      field("fixed_size_binary", fixed_size_binary(3))},
      output_fields{field("date64", timestamp(TimeUnit::NANO)),
                    field("ts0", timestamp(TimeUnit::NANO)),
                    field("ts1", timestamp(TimeUnit::NANO)),
                    field("ts2", timestamp(TimeUnit::NANO)),
                    field("large_string", utf8()),
                    field("large_binary", binary()),
                    field("fixed_size_binary0", binary()),
                    field("fixed_size_binary", binary())};
  std::shared_ptr<Schema> input_schema = std::make_shared<Schema>(input_fields),
                          output_schema = std::make_shared<Schema>(output_fields);

  int64_t num_rows = 10000;
  int64_t numCols = input_fields.size();

  ArrayBuilderMatrix buildersIn(numCols, ArrayBuilderVector(5, NULLPTR));
  ArrayBuilderVector buildersOut(numCols, NULLPTR);

  for (int i = 0; i < 5; i++) {
    buildersIn[0][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<Date64Builder>());
    buildersIn[1][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::SECOND), default_memory_pool()));
    buildersIn[2][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::MILLI), default_memory_pool()));
    buildersIn[3][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::MICRO), default_memory_pool()));
    buildersIn[4][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<LargeStringBuilder>());
    buildersIn[5][i] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<LargeBinaryBuilder>());
    buildersIn[6][i] = std::static_pointer_cast<ArrayBuilder>(
        std::make_shared<FixedSizeBinaryBuilder>(fixed_size_binary(0)));
    buildersIn[7][i] = std::static_pointer_cast<ArrayBuilder>(
        std::make_shared<FixedSizeBinaryBuilder>(fixed_size_binary(3)));
  }

  for (int col = 0; col < 4; col++) {
    buildersOut[col] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<TimestampBuilder>(
            timestamp(TimeUnit::NANO), default_memory_pool()));
  }
  buildersOut[4] =
      std::static_pointer_cast<ArrayBuilder>(std::make_shared<StringBuilder>());
  for (int col = 5; col < 8; col++) {
    buildersOut[col] =
        std::static_pointer_cast<ArrayBuilder>(std::make_shared<BinaryBuilder>());
  }

  char bin1[3], bin2[4], bin3[] = "", string_[13];
  std::string str;

  for (int64_t i = 0; i < num_rows; i++) {
    int chunk = i < (num_rows / 2) ? 1 : 3;
    if (i % 2) {
      str = "Arrow " + std::to_string(-4 * i + 8);
      snprintf(string_, sizeof(string_), "%s", str.c_str());
      ARROW_EXPECT_OK(std::static_pointer_cast<Date64Builder>(buildersIn[0][chunk])
                          ->Append(INT64_C(1605758461555) + 3 * i));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<TimestampBuilder>(buildersIn[1][chunk])->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersIn[2][chunk])
                          ->Append(INT64_C(1605758461000) - 14 * i));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<TimestampBuilder>(buildersIn[3][chunk])->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<LargeStringBuilder>(buildersIn[4][chunk])
                          ->Append(string_));
      ARROW_EXPECT_OK(std::static_pointer_cast<LargeBinaryBuilder>(buildersIn[5][chunk])
                          ->AppendNull());
      ARROW_EXPECT_OK(
          std::static_pointer_cast<FixedSizeBinaryBuilder>(buildersIn[6][chunk])
              ->Append(bin3));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<FixedSizeBinaryBuilder>(buildersIn[7][chunk])
              ->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersOut[0])
                          ->Append(INT64_C(1605758461555000000) + INT64_C(3000000) * i));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<TimestampBuilder>(buildersOut[1])->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersOut[2])
                          ->Append(INT64_C(1605758461000000000) - INT64_C(14000000) * i));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<TimestampBuilder>(buildersOut[3])->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<StringBuilder>(buildersOut[4])
                          ->Append("Arrow " + std::to_string(-4 * i + 8)));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<BinaryBuilder>(buildersOut[5])->AppendNull());
      ARROW_EXPECT_OK(
          std::static_pointer_cast<BinaryBuilder>(buildersOut[6])->Append(bin3, 0));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<BinaryBuilder>(buildersOut[7])->AppendNull());
    } else {
      bin1[0] = i % 256;
      bin1[1] = (i + 1) % 256;
      bin1[2] = (i - 1) % 256;
      bin2[0] = (29 * i - 192) % 256;
      bin2[1] = (2 * i + 1) % 256;
      bin2[2] = (4 * i + 103) % 256;
      bin2[3] = (17 * i + 122) % 256;
      ARROW_EXPECT_OK(
          std::static_pointer_cast<Date64Builder>(buildersIn[0][chunk])->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersIn[1][chunk])
                          ->Append(INT64_C(1605758461) + INT64_C(61) * i));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<TimestampBuilder>(buildersIn[2][chunk])->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersIn[3][chunk])
                          ->Append(INT64_C(1605758461000111) + INT64_C(1021) * i));
      ARROW_EXPECT_OK(std::static_pointer_cast<LargeStringBuilder>(buildersIn[4][chunk])
                          ->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<LargeBinaryBuilder>(buildersIn[5][chunk])
                          ->Append(bin2, 4));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<FixedSizeBinaryBuilder>(buildersIn[6][chunk])
              ->AppendNull());
      ARROW_EXPECT_OK(
          std::static_pointer_cast<FixedSizeBinaryBuilder>(buildersIn[7][chunk])
              ->Append(bin1));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<TimestampBuilder>(buildersOut[0])->AppendNull());
      ARROW_EXPECT_OK(
          std::static_pointer_cast<TimestampBuilder>(buildersOut[1])
              ->Append(INT64_C(1605758461000000000) + INT64_C(61000000000) * i));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<TimestampBuilder>(buildersOut[2])->AppendNull());
      ARROW_EXPECT_OK(std::static_pointer_cast<TimestampBuilder>(buildersOut[3])
                          ->Append(INT64_C(1605758461000111000) + INT64_C(1021000) * i));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<StringBuilder>(buildersOut[4])->AppendNull());
      ARROW_EXPECT_OK(
          std::static_pointer_cast<BinaryBuilder>(buildersOut[5])->Append(bin2, 4));
      ARROW_EXPECT_OK(
          std::static_pointer_cast<BinaryBuilder>(buildersOut[6])->AppendNull());
      ARROW_EXPECT_OK(
          std::static_pointer_cast<BinaryBuilder>(buildersOut[7])->Append(bin1, 3));
    }
  }

  ArrayMatrix arraysIn(numCols, ArrayVector(5, NULLPTR));
  ArrayVector arraysOut(numCols, NULLPTR);

  ChunkedArrayVector cvIn, cvOut;
  cvIn.reserve(numCols);
  cvOut.reserve(numCols);

  for (int col = 0; col < numCols; col++) {
    for (int i = 0; i < 5; i++) {
      ARROW_EXPECT_OK(buildersIn[col][i]->Finish(&arraysIn[col][i]));
    }
    ARROW_EXPECT_OK(buildersOut[col]->Finish(&arraysOut[col]));
    cvIn.push_back(std::make_shared<ChunkedArray>(arraysIn[col]));
    cvOut.push_back(std::make_shared<ChunkedArray>(arraysOut[col]));
  }

  std::shared_ptr<Table> input_table = Table::Make(input_schema, cvIn),
                         expected_output_table = Table::Make(output_schema, cvOut);
  AssertTableWriteReadEqual(input_table, expected_output_table);
}

// Nested types
TEST(TestAdapterWriteNested, ListTest) {
  int64_t num_rows = 2;
  static constexpr random::SeedType kRandomSeed2 = 0x0ff1ce;
  arrow::random::RandomArrayGenerator rand(kRandomSeed2);
  std::shared_ptr<Array> value_array = rand.ArrayOf(int32(), 2 * num_rows, 0.2);
  std::shared_ptr<Array> array = rand.List(*value_array, num_rows, 1);
  RecordProperty("bitmap", *(array->null_bitmap_data()));
  RecordProperty("length", array->length());
  RecordProperty("array", array->ToString());
}
TEST(TestAdapterWriteNested, writeList) {
  std::shared_ptr<Schema> table_schema = schema({field("list", list(int32()))});
  int64_t num_rows = 10000;
  arrow::random::RandomArrayGenerator rand(kRandomSeed);
  auto value_array = rand.ArrayOf(int32(), 5 * num_rows, 0.6);
  std::shared_ptr<Array> array = rand.List(*value_array, num_rows + 1, 0.8);
  std::shared_ptr<ChunkedArray> chunked_array = std::make_shared<ChunkedArray>(array);
  std::shared_ptr<Table> table = Table::Make(table_schema, {chunked_array});

  std::shared_ptr<io::BufferOutputStream> buffer_output_stream =
      io::BufferOutputStream::Create(kDefaultSmallMemStreamSize * 15).ValueOrDie();
  std::unique_ptr<adapters::orc::ORCFileWriter> writer =
      adapters::orc::ORCFileWriter::Open(*buffer_output_stream).ValueOrDie();
  ARROW_EXPECT_OK(writer->Write(*table));
  ARROW_EXPECT_OK(writer->Close());
  std::shared_ptr<Buffer> buffer = buffer_output_stream->Finish().ValueOrDie();
  std::shared_ptr<io::RandomAccessFile> in_stream(new io::BufferReader(buffer));
  std::unique_ptr<adapters::orc::ORCFileReader> reader;
  ARROW_EXPECT_OK(
      adapters::orc::ORCFileReader::Open(in_stream, default_memory_pool(), &reader));
  std::shared_ptr<Table> actual_output_table;
  ARROW_EXPECT_OK(reader->Read(&actual_output_table));
  auto actual_array =
      std::static_pointer_cast<ListArray>(actual_output_table->column(0)->chunk(0));
  auto expected_array = std::static_pointer_cast<ListArray>(table->column(0)->chunk(0));
  AssertArraysEqual(*(actual_array->offsets()), *(expected_array->offsets()));
  AssertArraysEqual(*(actual_array->values()), *(expected_array->values()));
  AssertBufferEqual(*(actual_array->null_bitmap()), *(expected_array->null_bitmap()));
  ASSERT_TRUE(actual_array->type()->Equals(*(expected_array->type()), true));
  RecordProperty("output_type", actual_array->type()->ToString());
  RecordProperty("input_type", expected_array->type()->ToString());
  RecordProperty("array_equality", actual_array->Equals(*expected_array));
}
TEST(TestAdapterWriteNested, writeMap) {
  std::shared_ptr<Schema> table_schema = schema({field("map", map(int32(), int32()))});
  int64_t num_rows = 2;
  arrow::random::RandomArrayGenerator rand(kRandomSeed);
  auto map_type = std::static_pointer_cast<MapType>(table_schema->field(0)->type());
  auto key_array = rand.ArrayOf(map_type->key_type(), 2 * num_rows, 0);
  auto item_array = rand.ArrayOf(map_type->item_type(), 2 * num_rows, 0.2);
  std::shared_ptr<Array> array = rand.Map(key_array, item_array, num_rows, 0.3);
  std::shared_ptr<ChunkedArray> chunked_array = std::make_shared<ChunkedArray>(array);
  std::shared_ptr<Table> table = Table::Make(table_schema, {chunked_array});
  // AssertTableWriteReadEqual(table, table, kDefaultSmallMemStreamSize * 5);
}
TEST(TestAdapterWriteNested, writeStruct) {
  std::vector<std::shared_ptr<Field>> subsubfields{
      field("bool", boolean()),
      field("int8", int8()),
      field("int16", int16()),
      field("int32", int32()),
      field("int64", int64()),
      field("decimal128nz", decimal(38, 6)),
      field("decimal128z", decimal(38, 0)),
      field("date32", date32()),
      field("ts3", timestamp(TimeUnit::NANO)),
      field("string", utf8()),
      field("binary", binary())};
  std::shared_ptr<Schema> table_schema =
      schema({field("struct", struct_({field("struct2", struct_(subsubfields))}))});
  int64_t num_rows = 10000;
  int num_subsubcols = subsubfields.size();
  arrow::random::RandomArrayGenerator rand(kRandomSeed);
  ArrayVector av00(num_subsubcols), av0(1);
  for (int i = 0; i < num_subsubcols; i++) {
    av00[i] = rand.ArrayOf(subsubfields[i]->type(), num_rows, 0.9);
  }
  std::shared_ptr<Buffer> bitmap0 = rand.NullBitmap(num_rows, 0.8);
  av0[0] = std::make_shared<StructArray>(table_schema->field(0)->type()->field(0)->type(),
                                         num_rows, av00, bitmap0);
  std::shared_ptr<Buffer> bitmap = rand.NullBitmap(num_rows, 0.7);
  std::shared_ptr<Array> array = std::make_shared<StructArray>(
      table_schema->field(0)->type(), num_rows, av0, bitmap);
  std::shared_ptr<ChunkedArray> chunked_array = std::make_shared<ChunkedArray>(array);
  std::shared_ptr<Table> table = Table::Make(table_schema, {chunked_array});
  AssertTableWriteReadEqual(table, table, kDefaultSmallMemStreamSize * 10);
}
TEST(TestAdapterWriteNested, writeListOfStruct) {
  std::shared_ptr<Schema> table_schema =
      schema({field("ls", list(struct_({field("a", int32())})))});
  int64_t num_rows = 10000;
  arrow::random::RandomArrayGenerator rand(kRandomSeed);
  ArrayVector av00(1);
  av00[0] = rand.ArrayOf(int32(), num_rows, 0.7);
  std::shared_ptr<Buffer> bitmap = rand.NullBitmap(num_rows, 0.8);
  std::shared_ptr<Array> value_array = std::make_shared<StructArray>(
      table_schema->field(0)->type()->field(0)->type(), 5 * num_rows, av00, bitmap);
  std::shared_ptr<Array> array = rand.List(*value_array, num_rows + 1, 0.6);
  std::shared_ptr<ChunkedArray> chunked_array = std::make_shared<ChunkedArray>(array);
  std::shared_ptr<Table> table = Table::Make(table_schema, {chunked_array});
  // AssertTableWriteReadEqual(table, table, kDefaultSmallMemStreamSize * 15);
}
// TEST(TestAdapterWriteNested, writeMixedConvert) {
//   std::vector<std::shared_ptr<Field>> input_fields{
//       field("large_list", large_list(int32())),
//       field("fixed_size_list", fixed_size_list(int32(), 3)),
//       field("map", map(int32(), int32()))},
//       output_fields{
//           field("large_list", list(int32())), field("fixed_size_list",
//           list(int32())), field("map", list(struct_({field("key", int32()),
//           field("value", int32())})))};

//   std::shared_ptr<Schema> input_schema = std::make_shared<Schema>(input_fields);
//   std::shared_ptr<Schema> output_schema = std::make_shared<Schema>(output_fields);

//   int64_t num_rows = 10000;
//   int64_t numCols = input_fields.size();

//   ArrayBuilderVector valuesBuilders0In(5, NULLPTR), offsetsBuilders0In(3, NULLPTR),
//       valuesBuilders1In(5, NULLPTR), keysBuilders2In(5, NULLPTR),
//       itemsBuilders2In(5, NULLPTR), offsetsBuilders2In(3, NULLPTR);
//   ArrayVector valuesArrays0In(5, NULLPTR), offsetsArrays0In(3, NULLPTR),
//       valuesArrays1In(5, NULLPTR), keysArrays2In(5, NULLPTR), itemsArrays2In(5,
//       NULLPTR), offsetsArrays2In(3, NULLPTR);
//   std::shared_ptr<ArrayBuilder>
//       valuesBuilder0Out =
//           std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>()),
//       valuesBuilder1Out =
//           std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>()),
//       keysBuilder2Out =
//           std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>()),
//       itemsBuilder2Out =
//           std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>());
//   std::shared_ptr<Array> valuesArray0Out, valuesArray1Out, keysArray2Out,
//   itemsArray2Out;

//   for (int i = 0; i < 5; i++) {
//     valuesBuilders0In[i] =
//         std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>());
//     valuesBuilders1In[i] =
//         std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>());
//     keysBuilders2In[i] =
//         std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>());
//     itemsBuilders2In[i] =
//         std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>());
//   }
//   for (int i = 0; i < 3; i++) {
//     offsetsBuilders0In[i] =
//         std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int64Builder>());
//     offsetsBuilders2In[i] =
//         std::static_pointer_cast<ArrayBuilder>(std::make_shared<Int32Builder>());
//   }
//   int arrayOffsetSizeIn = num_rows / 2 + 1;
//   int arrayOffsetSizeOut = num_rows + 1;
//   int64_t offsets0In[2][arrayOffsetSizeIn];
//   int32_t offsets2In[2][arrayOffsetSizeIn];
//   int32_t offsetsOut[numCols][arrayOffsetSizeOut];

//   offsets0In[0][0] = 0;
//   offsets0In[1][0] = 0;
//   offsets2In[0][0] = 0;
//   offsets2In[1][0] = 0;
//   for (int col = 0; col < numCols; col++) {
//     offsetsOut[col][0] = 0;
//   }
//   for (int i = 0; i < num_rows; i++) {
//     int offsetsChunk = i < (num_rows / 2) ? 0 : 1;
//     int valuesChunk = 2 * offsetsChunk + 1;
//     int offsetsOffset = offsetsChunk * num_rows / 2;
//     switch (i % 4) {
//       case 0: {
//         offsets0In[offsetsChunk][i + 1 - offsetsOffset] =
//             offsets0In[offsetsChunk][i - offsetsOffset];
//         offsetsOut[0][i + 1] = offsetsOut[0][i];
//         for (int j = 0; j < 4; j++) {
//           ARROW_EXPECT_OK(
//               std::static_pointer_cast<Int32Builder>(keysBuilders2In[valuesChunk])
//                   ->Append(2 * i + j));
//           ARROW_EXPECT_OK(
//               std::static_pointer_cast<Int32Builder>(itemsBuilders2In[valuesChunk])
//                   ->Append((2 * i + j) % 8));
//           ARROW_EXPECT_OK(
//               std::static_pointer_cast<Int32Builder>(keysBuilder2Out)->Append(2 * i +
//               j));
//           ARROW_EXPECT_OK(std::static_pointer_cast<Int32Builder>(itemsBuilder2Out)
//                               ->Append((2 * i + j) % 8));
//         }
//         offsets2In[offsetsChunk][i + 1 - offsetsOffset] =
//             offsets2In[offsetsChunk][i - offsetsOffset] + 4;
//         offsetsOut[2][i + 1] = offsetsOut[2][i] + 4;
//         break;
//       }
//       case 1: {
//         ARROW_EXPECT_OK(
//             std::static_pointer_cast<Int32Builder>(valuesBuilders0In[valuesChunk])
//                 ->Append(i - 1));
//         ARROW_EXPECT_OK(
//             std::static_pointer_cast<Int32Builder>(valuesBuilder0Out)->Append(i -
//             1));
//         offsets0In[offsetsChunk][i + 1 - offsetsOffset] =
//             offsets0In[offsetsChunk][i - offsetsOffset] + 1;
//         offsetsOut[0][i + 1] = offsetsOut[0][i] + 1;
//         for (int j = 0; j < 3; j++) {
//           ARROW_EXPECT_OK(
//               std::static_pointer_cast<Int32Builder>(keysBuilders2In[valuesChunk])
//                   ->Append(2 * i + 2 + j));
//           ARROW_EXPECT_OK(std::static_pointer_cast<Int32Builder>(keysBuilder2Out)
//                               ->Append(2 * i + 2 + j));
//           if (j != 1) {
//             ARROW_EXPECT_OK(itemsBuilders2In[valuesChunk]->AppendNull());
//             ARROW_EXPECT_OK(itemsBuilder2Out->AppendNull());
//           } else {
//             ARROW_EXPECT_OK(
//                 std::static_pointer_cast<Int32Builder>(itemsBuilders2In[valuesChunk])
//                     ->Append((2 * i + 2 + j) % 8));
//             ARROW_EXPECT_OK(std::static_pointer_cast<Int32Builder>(itemsBuilder2Out)
//                                 ->Append((2 * i + j + 2) % 8));
//           }
//         }
//         offsets2In[offsetsChunk][i + 1 - offsetsOffset] =
//             offsets2In[offsetsChunk][i - offsetsOffset] + 3;
//         offsetsOut[2][i + 1] = offsetsOut[2][i] + 3;
//         break;
//       }
//       case 2: {
//         for (int j = 0; j < 8; j++) {
//           ARROW_EXPECT_OK(valuesBuilders0In[valuesChunk]->AppendNull());
//           ARROW_EXPECT_OK(valuesBuilder0Out->AppendNull());
//         }
//         offsets0In[offsetsChunk][i + 1 - offsetsOffset] =
//             offsets0In[offsetsChunk][i - offsetsOffset] + 8;
//         offsetsOut[0][i + 1] = offsetsOut[0][i] + 8;
//         ARROW_EXPECT_OK(
//             std::static_pointer_cast<Int32Builder>(keysBuilders2In[valuesChunk])
//                 ->Append(2 * i + 3));
//         ARROW_EXPECT_OK(
//             std::static_pointer_cast<Int32Builder>(itemsBuilders2In[valuesChunk])
//                 ->Append((2 * i + 3) % 8));

//         offsets2In[offsetsChunk][i + 1 - offsetsOffset] =
//             offsets2In[offsetsChunk][i - offsetsOffset] + 1;
//         ARROW_EXPECT_OK(
//             std::static_pointer_cast<Int32Builder>(keysBuilder2Out)->Append(2 * i +
//             3));
//         ARROW_EXPECT_OK(std::static_pointer_cast<Int32Builder>(itemsBuilder2Out)
//                             ->Append((2 * i + 3) % 8));
//         offsetsOut[2][i + 1] = offsetsOut[2][i] + 1;
//         break;
//       }
//       default: {
//         for (int j = 0; j < 3; j++) {
//           ARROW_EXPECT_OK(valuesBuilders0In[valuesChunk]->AppendNull());
//           ARROW_EXPECT_OK(valuesBuilder0Out->AppendNull());
//         }
//         ARROW_EXPECT_OK(
//             std::static_pointer_cast<Int32Builder>(valuesBuilders0In[valuesChunk])
//                 ->Append(i - 1));
//         ARROW_EXPECT_OK(
//             std::static_pointer_cast<Int32Builder>(valuesBuilder0Out)->Append(i -
//             1));
//         for (int j = 0; j < 3; j++) {
//           ARROW_EXPECT_OK(valuesBuilders0In[valuesChunk]->AppendNull());
//           ARROW_EXPECT_OK(valuesBuilder0Out->AppendNull());
//         }
//         offsets0In[offsetsChunk][i + 1 - offsetsOffset] =
//             offsets0In[offsetsChunk][i - offsetsOffset] + 7;
//         offsetsOut[0][i + 1] = offsetsOut[0][i] + 7;
//         offsets2In[offsetsChunk][i + 1 - offsetsOffset] =
//             offsets2In[offsetsChunk][i - offsetsOffset];
//         offsetsOut[2][i + 1] = offsetsOut[2][i];
//       }
//     }
//     ARROW_EXPECT_OK(std::static_pointer_cast<Int32Builder>(valuesBuilders1In[valuesChunk])
//                         ->Append(2 * i));
//     ARROW_EXPECT_OK(valuesBuilders1In[valuesChunk]->AppendNull());
//     ARROW_EXPECT_OK(valuesBuilders1In[valuesChunk]->AppendNull());
//     ARROW_EXPECT_OK(
//         std::static_pointer_cast<Int32Builder>(valuesBuilder1Out)->Append(2 * i));
//     ARROW_EXPECT_OK(valuesBuilder1Out->AppendNull());
//     ARROW_EXPECT_OK(valuesBuilder1Out->AppendNull());
//     offsetsOut[1][i + 1] = offsetsOut[1][i] + 3;
//   }

//   int arrayBitmapSizeIn = num_rows / 16, arrayBitmapSizeOut = num_rows / 8;

//   uint8_t bitmapsIn[numCols][2][arrayBitmapSizeIn],
//       bitmapsOut[numCols][arrayBitmapSizeOut];
//   for (int i = 0; i < arrayBitmapSizeIn; i++) {
//     for (int j = 0; j < 2; j++) {
//       bitmapsIn[0][j][i] = 90;   // 01011010
//       bitmapsIn[1][j][i] = 195;  // 11000011
//       bitmapsIn[2][j][i] = 255;  // 11111111
//     }
//   }
//   for (int i = 0; i < arrayBitmapSizeOut; i++) {
//     bitmapsOut[0][i] = 90;   // 01011010
//     bitmapsOut[1][i] = 195;  // 11000011
//     // This is a reader bug we need to fix
//     bitmapsOut[2][i] = 255;  // 11111111
//   }

//   BufferBuilderMatrix bitmapBufferBuildersIn(numCols, BufferBuilderVector(2,
//   NULLPTR)); BufferMatrix bitmapBuffersIn(numCols, BufferVector(2, NULLPTR)); for
//   (int i = 0; i < numCols; i++) {
//     for (int j = 0; j < 2; j++) {
//       bitmapBufferBuildersIn[i][j] = std::make_shared<BufferBuilder>();
//     }
//   }

//   BufferBuilderVector offsetsBufferBuilders0In(2, std::make_shared<BufferBuilder>()),
//       offsetsBufferBuilders2In(2, std::make_shared<BufferBuilder>());
//   BufferVector offsetsBuffers0In(2, NULLPTR), offsetsBuffers2In(2, NULLPTR);
//   BufferBuilderVector bitmapBufferBuildersOut(numCols,
//   std::make_shared<BufferBuilder>()),
//       offsetsBufferBuildersOut(numCols, std::make_shared<BufferBuilder>());
//   BufferVector bitmapBuffersOut(numCols, NULLPTR), offsetsBuffersOut(numCols,
//   NULLPTR);

//   int arrayOffsetSizeBytesInLarge = 8 * arrayOffsetSizeIn;
//   int arrayOffsetSizeBytesInNormal = 4 * arrayOffsetSizeIn;
//   int arrayOffsetSizeBytesOutNormal = 4 * arrayOffsetSizeOut;

//   for (int j = 0; j < numCols; j++) {
//     for (int i = 0; i < 2; i++) {
//       ARROW_EXPECT_OK(bitmapBufferBuildersIn[j][i]->Resize(arrayBitmapSizeIn));
//       ARROW_EXPECT_OK(
//           bitmapBufferBuildersIn[j][i]->Append(bitmapsIn[j][i], arrayBitmapSizeIn));
//       ARROW_EXPECT_OK(bitmapBufferBuildersIn[j][i]->Finish(&bitmapBuffersIn[j][i]));
//     }
//   }

//   for (int i = 0; i < 2; i++) {
//     ARROW_EXPECT_OK(offsetsBufferBuilders0In[i]->Resize(arrayOffsetSizeBytesInLarge));
//     ARROW_EXPECT_OK(
//         offsetsBufferBuilders0In[i]->Append(offsets0In[i],
//         arrayOffsetSizeBytesInLarge));
//     ARROW_EXPECT_OK(offsetsBufferBuilders0In[i]->Finish(&offsetsBuffers0In[i]));

//     ARROW_EXPECT_OK(offsetsBufferBuilders2In[i]->Resize(arrayOffsetSizeBytesInNormal));
//     ARROW_EXPECT_OK(
//         offsetsBufferBuilders2In[i]->Append(offsets2In[i],
//         arrayOffsetSizeBytesInNormal));
//     ARROW_EXPECT_OK(offsetsBufferBuilders2In[i]->Finish(&offsetsBuffers2In[i]));
//   }

//   for (int j = 0; j < numCols; j++) {
//     ARROW_EXPECT_OK(bitmapBufferBuildersOut[j]->Resize(arrayBitmapSizeOut));
//     ARROW_EXPECT_OK(
//         bitmapBufferBuildersOut[j]->Append(bitmapsOut[j], arrayBitmapSizeOut));
//     ARROW_EXPECT_OK(bitmapBufferBuildersOut[j]->Finish(&bitmapBuffersOut[j]));
//     ARROW_EXPECT_OK(offsetsBufferBuildersOut[j]->Resize(arrayOffsetSizeBytesOutNormal));
//     ARROW_EXPECT_OK(offsetsBufferBuildersOut[j]->Append(offsetsOut[j],
//                                                         arrayOffsetSizeBytesOutNormal));
//     ARROW_EXPECT_OK(offsetsBufferBuildersOut[j]->Finish(&offsetsBuffersOut[j]));
//   }

//   for (int i = 0; i < 3; i++) {
//     ARROW_EXPECT_OK(
//         std::static_pointer_cast<Int64Builder>(offsetsBuilders0In[i])->Append(0));
//     ARROW_EXPECT_OK(offsetsBuilders0In[i]->Finish(&offsetsArrays0In[i]));
//     ARROW_EXPECT_OK(
//         std::static_pointer_cast<Int32Builder>(offsetsBuilders2In[i])->Append(0));
//     ARROW_EXPECT_OK(offsetsBuilders2In[i]->Finish(&offsetsArrays2In[i]));
//   }

//   ArrayMatrix arraysIn(numCols, ArrayVector(5, NULLPTR));
//   ArrayVector arraysOut(numCols, NULLPTR);

//   ChunkedArrayVector cvIn, cvOut;
//   cvIn.reserve(numCols);
//   cvOut.reserve(numCols);

//   for (int i = 0; i < 5; i++) {
//     ARROW_EXPECT_OK(valuesBuilders0In[i]->Finish(&valuesArrays0In[i]));
//     ARROW_EXPECT_OK(valuesBuilders1In[i]->Finish(&valuesArrays1In[i]));
//     ARROW_EXPECT_OK(keysBuilders2In[i]->Finish(&keysArrays2In[i]));
//     ARROW_EXPECT_OK(itemsBuilders2In[i]->Finish(&itemsArrays2In[i]));
//     if (i == 1 || i == 3) {
//       arraysIn[0][i] = std::make_shared<LargeListArray>(
//           input_fields[0]->type(), num_rows / 2, offsetsBuffers0In[(i - 1) / 2],
//           valuesArrays0In[i], bitmapBuffersIn[0][(i - 1) / 2]);
//       arraysIn[1][i] = std::make_shared<FixedSizeListArray>(
//           input_fields[1]->type(), num_rows / 2, valuesArrays1In[i],
//           bitmapBuffersIn[1][(i - 1) / 2]);
//       arraysIn[2][i] = std::make_shared<MapArray>(
//           input_fields[2]->type(), num_rows / 2, offsetsBuffers2In[(i - 1) / 2],
//           keysArrays2In[i], itemsArrays2In[i], bitmapBuffersIn[2][(i - 1) / 2]);
//     } else {
//       arraysIn[0][i] =
//           LargeListArray::FromArrays(*offsetsArrays0In[i / 2], *valuesArrays0In[i])
//               .ValueOrDie();
//       arraysIn[1][i] = std::static_pointer_cast<FixedSizeListArray>(
//           FixedSizeListArray::FromArrays(valuesArrays1In[i], 3).ValueOrDie());
//       arraysIn[2][i] = std::static_pointer_cast<MapArray>(
//           MapArray::FromArrays(offsetsArrays2In[i / 2], keysArrays2In[i],
//                                itemsArrays2In[i])
//               .ValueOrDie());
//     }
//   }
//   ARROW_EXPECT_OK(valuesBuilder0Out->Finish(&valuesArray0Out));
//   ARROW_EXPECT_OK(valuesBuilder1Out->Finish(&valuesArray1Out));
//   ARROW_EXPECT_OK(keysBuilder2Out->Finish(&keysArray2Out));
//   ARROW_EXPECT_OK(itemsBuilder2Out->Finish(&itemsArray2Out));
//   arraysOut[0] = std::make_shared<ListArray>(output_fields[0]->type(), num_rows,
//                                              offsetsBuffersOut[0], valuesArray0Out,
//                                              bitmapBuffersOut[0]);
//   arraysOut[1] = std::make_shared<ListArray>(output_fields[1]->type(), num_rows,
//                                              offsetsBuffersOut[1], valuesArray1Out,
//                                              bitmapBuffersOut[1]);

//   ArrayVector children20{keysArray2Out, itemsArray2Out};
//   auto arraysOut20 = std::make_shared<StructArray>(
//       output_fields[2]->type()->field(0)->type(), 2 * num_rows, children20);
//   arraysOut[2] =
//       std::make_shared<ListArray>(output_fields[2]->type(), num_rows,
//                                   offsetsBuffersOut[2], arraysOut20,
//                                   bitmapBuffersOut[2]);

//   for (int col = 0; col < numCols; col++) {
//     cvIn.push_back(std::make_shared<ChunkedArray>(arraysIn[col]));
//     cvOut.push_back(std::make_shared<ChunkedArray>(arraysOut[col]));
//   }

//   std::shared_ptr<Table> input_table = Table::Make(input_schema, cvIn),
//                          expected_output_table = Table::Make(output_schema, cvOut);
//   AssertTableWriteReadEqual(input_table, expected_output_table);
// }
}  // namespace arrow
