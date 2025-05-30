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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/memory/Memory.h"
#include "velox/common/memory/MemoryPool.h"
#include "velox/dwio/common/ChainedBuffer.h"

using namespace ::testing;

namespace facebook {
namespace velox {
namespace dwio {
namespace common {

class ChainedBufferTests : public Test {
 protected:
  static void SetUpTestCase() {
    memory::MemoryManager::testingSetInstance({});
  }

  std::shared_ptr<memory::MemoryPool> pool_{
      memory::memoryManager()->addLeafPool()};
};

TEST_F(ChainedBufferTests, testCreate) {
  ChainedBuffer<int32_t> buf{*pool_, 128, 1024};
  ASSERT_EQ(buf.capacity(), 128);
  ASSERT_EQ(buf.size(), 0);
  ASSERT_EQ(buf.pages_.size(), 1);
  ChainedBuffer<int32_t> buf2{*pool_, 256, 1024};
  ASSERT_EQ(buf2.capacity(), 256);
  ASSERT_EQ(buf2.pages_.size(), 1);
  ASSERT_EQ(buf2.size(), 0);
  ChainedBuffer<int32_t> buf3{*pool_, 257, 1024};
  ASSERT_EQ(buf3.capacity(), 512);
  ASSERT_EQ(buf3.pages_.size(), 2);
  ASSERT_EQ(buf3.size(), 0);

  VELOX_ASSERT_THROW(
      (ChainedBuffer<int32_t>{*pool_, 256, 257}),
      "(2 vs. 1) must be power of 2: 257");

  ChainedBuffer<int32_t> buf0{*pool_, 0, 1024};
  ASSERT_EQ(buf0.capacity(), 0);
  ASSERT_EQ(buf0.pages_.size(), 0);
  ASSERT_EQ(buf0.size(), 0);
}

TEST_F(ChainedBufferTests, testReserve) {
  for (const uint32_t initialCapacityBytes : {0, 16}) {
    SCOPED_TRACE(fmt::format(
        "initialCapacityBytes ", succinctBytes(initialCapacityBytes)));
    ChainedBuffer<int32_t> buf{*pool_, initialCapacityBytes, 1024};
    ASSERT_EQ(buf.capacity(), initialCapacityBytes);
    ASSERT_EQ(buf.size(), 0);
    buf.reserve(16);
    buf.reserve(17);
    ASSERT_EQ(buf.capacity(), 32);
    ASSERT_EQ(buf.pages_.size(), 1);
    buf.reserve(112);
    ASSERT_EQ(buf.capacity(), 128);
    ASSERT_EQ(buf.pages_.size(), 1);
    buf.reserve(257);
    ASSERT_EQ(buf.capacity(), 512);
    ASSERT_EQ(buf.pages_.size(), 2);
    buf.reserve(1025);
    ASSERT_EQ(buf.capacity(), 1024 + 256);
    ASSERT_EQ(buf.pages_.size(), 5);
  }
}

TEST_F(ChainedBufferTests, testAppend) {
  ChainedBuffer<int32_t> buf{*pool_, 16, 64};
  for (size_t i = 0; i < 16; ++i) {
    buf.unsafeAppend(i);
    ASSERT_EQ(buf.capacity(), 16);
    ASSERT_EQ(buf.size(), i + 1);
    ASSERT_EQ(buf.pages_.size(), 1);
  }
  buf.reserve(32);
  for (size_t i = 0; i < 16; ++i) {
    buf.unsafeAppend(i + 16);
    ASSERT_EQ(buf.capacity(), 32);
    ASSERT_EQ(buf.size(), i + 17);
    ASSERT_EQ(buf.pages_.size(), 2);
  }
  for (size_t i = 0; i < 32; ++i) {
    ASSERT_EQ(buf[i], i);
  }
  buf.append(100);
  ASSERT_EQ(buf.capacity(), 48);
  ASSERT_EQ(buf.pages_.size(), 3);
  ASSERT_EQ(buf[buf.size() - 1], 100);
}

TEST_F(ChainedBufferTests, testClear) {
  ChainedBuffer<int32_t> buf{*pool_, 128, 1024};
  buf.clear();
  ASSERT_EQ(buf.capacity(), 128);
  ASSERT_EQ(buf.size(), 0);
  ASSERT_EQ(buf.pages_.size(), 1);

  ChainedBuffer<int32_t> buf2{*pool_, 1024, 1024};
  buf2.clear(false);
  ASSERT_EQ(buf2.capacity(), 256);
  ASSERT_EQ(buf2.size(), 0);
  ASSERT_EQ(buf2.pages_.size(), 1);
}

TEST_F(ChainedBufferTests, testApplyRange) {
  std::vector<std::tuple<uint64_t, uint64_t, int32_t>> result;
  auto fn = [&](auto ptr, auto begin, auto end) {
    result.push_back({begin, end, *ptr});
  };

  ChainedBuffer<int32_t> buf{*pool_, 64, 64};
  for (size_t i = 0; i < 64 / 16; ++i) {
    for (size_t j = 0; j < 16; ++j) {
      buf.unsafeAppend(i);
    }
  }
  VELOX_ASSERT_THROW(buf.applyRange(2, 1, fn), "(2 vs. 1)");
  VELOX_ASSERT_THROW(buf.applyRange(1, 65, fn), "(65 vs. 64)");

  result.clear();
  buf.applyRange(1, 5, fn);
  ASSERT_THAT(
      result, ElementsAre(std::tuple<uint64_t, uint64_t, int32_t>{1, 5, 0}));

  result.clear();
  buf.applyRange(3, 16, fn);
  ASSERT_THAT(
      result, ElementsAre(std::tuple<uint64_t, uint64_t, int32_t>{3, 16, 0}));

  result.clear();
  buf.applyRange(1, 17, fn);
  ASSERT_THAT(
      result,
      ElementsAre(
          std::tuple<uint64_t, uint64_t, int32_t>{1, 16, 0},
          std::tuple<uint64_t, uint64_t, int32_t>{0, 1, 1}));

  result.clear();
  buf.applyRange(1, 37, fn);
  ASSERT_THAT(
      result,
      ElementsAre(
          std::tuple<uint64_t, uint64_t, int32_t>{1, 16, 0},
          std::tuple<uint64_t, uint64_t, int32_t>{0, 16, 1},
          std::tuple<uint64_t, uint64_t, int32_t>{0, 5, 2}));

  result.clear();
  buf.applyRange(1, 64, fn);
  ASSERT_THAT(
      result,
      ElementsAre(
          std::tuple<uint64_t, uint64_t, int32_t>{1, 16, 0},
          std::tuple<uint64_t, uint64_t, int32_t>{0, 16, 1},
          std::tuple<uint64_t, uint64_t, int32_t>{0, 16, 2},
          std::tuple<uint64_t, uint64_t, int32_t>{0, 16, 3}));
}

TEST_F(ChainedBufferTests, testGetPage) {
  ChainedBuffer<int32_t> buf{*pool_, 1024, 1024};
  ASSERT_EQ(
      std::addressof(buf.getPageUnsafe(0)), std::addressof(buf.pages_.at(0)));
  ASSERT_EQ(
      std::addressof(buf.getPageUnsafe(255)), std::addressof(buf.pages_.at(0)));
  ASSERT_EQ(
      std::addressof(buf.getPageUnsafe(256)), std::addressof(buf.pages_.at(1)));
  ASSERT_EQ(
      std::addressof(buf.getPageUnsafe(1023)),
      std::addressof(buf.pages_.at(3)));

  ChainedBuffer<int64_t> buf2{*pool_, 1024, 1024};
  ASSERT_EQ(
      std::addressof(buf2.getPageUnsafe(0)), std::addressof(buf2.pages_.at(0)));
  ASSERT_EQ(
      std::addressof(buf2.getPageUnsafe(127)),
      std::addressof(buf2.pages_.at(0)));
  ASSERT_EQ(
      std::addressof(buf2.getPageUnsafe(128)),
      std::addressof(buf2.pages_.at(1)));
  ASSERT_EQ(
      std::addressof(buf2.getPageUnsafe(1023)),
      std::addressof(buf2.pages_.at(7)));
}

TEST_F(ChainedBufferTests, testGetPageIndex) {
  ChainedBuffer<int8_t> buf{*pool_, 1024, 1024};
  ASSERT_EQ(buf.getPageIndex(0), 0);
  ASSERT_EQ(buf.getPageIndex(256), 0);
  ASSERT_EQ(buf.getPageIndex(1023), 0);
  ASSERT_EQ(buf.getPageIndex(1024), 1);
  ASSERT_EQ(buf.getPageIndex(4095), 3);
  ASSERT_EQ(buf.getPageIndex(4096), 4);

  ChainedBuffer<int32_t> buf2{*pool_, 1024, 1024};
  ASSERT_EQ(buf2.getPageIndex(0), 0);
  ASSERT_EQ(buf2.getPageIndex(255), 0);
  ASSERT_EQ(buf2.getPageIndex(256), 1);
  ASSERT_EQ(buf2.getPageIndex(4095), 15);
  ASSERT_EQ(buf2.getPageIndex(4096), 16);
}

TEST_F(ChainedBufferTests, testGetPageOffset) {
  ChainedBuffer<int8_t> buf{*pool_, 1024, 1024};
  ASSERT_EQ(buf.getPageOffset(0), 0);
  ASSERT_EQ(buf.getPageOffset(256), 256);
  ASSERT_EQ(buf.getPageOffset(1023), 1023);
  ASSERT_EQ(buf.getPageOffset(1024), 0);
  ASSERT_EQ(buf.getPageOffset(4095), 1023);
  ASSERT_EQ(buf.getPageOffset(4096), 0);

  ChainedBuffer<int32_t> buf2{*pool_, 1024, 1024};
  ASSERT_EQ(buf2.getPageOffset(0), 0);
  ASSERT_EQ(buf2.getPageOffset(255), 255);
  ASSERT_EQ(buf2.getPageOffset(256), 0);
  ASSERT_EQ(buf2.getPageOffset(4095), 255);
  ASSERT_EQ(buf2.getPageOffset(4096), 0);
}

TEST_F(ChainedBufferTests, testBitCount) {
  ASSERT_EQ(ChainedBuffer<int32_t>::bitCount(0), 0);
  ASSERT_EQ(ChainedBuffer<int32_t>::bitCount(1), 1);
  ASSERT_EQ(ChainedBuffer<int32_t>::bitCount(4), 1);
  ASSERT_EQ(ChainedBuffer<int32_t>::bitCount(15), 4);
}

TEST_F(ChainedBufferTests, testTrailingZeros) {
  ASSERT_EQ(ChainedBuffer<int32_t>::trailingZeros(1), 0);
  ASSERT_EQ(ChainedBuffer<int32_t>::trailingZeros(12), 2);
  ASSERT_EQ(ChainedBuffer<int32_t>::trailingZeros(1u << 31), 31);
  VELOX_ASSERT_THROW(ChainedBuffer<int32_t>::trailingZeros(0), "(0 vs. 0)");
}

TEST_F(ChainedBufferTests, testClearAll) {
  for (const uint32_t initialCapacityBytes : {0, 128}) {
    SCOPED_TRACE(fmt::format(
        "initialCapacityBytes ", succinctBytes(initialCapacityBytes)));
    ChainedBuffer<int32_t> buf{*pool_, initialCapacityBytes, 1024};
    ASSERT_EQ(buf.capacity(), initialCapacityBytes);
    ASSERT_EQ(buf.size(), 0);

    buf.clear(false);
    ASSERT_EQ(buf.capacity(), initialCapacityBytes);
    ASSERT_EQ(buf.size(), 0);
    ASSERT_EQ(buf.pages_.size(), initialCapacityBytes == 0 ? 0 : 1);
    buf.clear(true);
    ASSERT_EQ(buf.capacity(), 0);
    ASSERT_EQ(buf.size(), 0);
    ASSERT_EQ(buf.pages_.size(), 0);

    buf.reserve(256);
    ASSERT_EQ(buf.capacity(), 256);
    ASSERT_EQ(buf.size(), 0);

    buf.unsafeAppend(32);
    ASSERT_EQ(buf.size(), 1);
    for (int i = 1; i < 256; ++i) {
      buf.unsafeAppend(32);
    }
    ASSERT_EQ(buf.capacity(), 256);
    ASSERT_EQ(buf.size(), 256);
    ASSERT_EQ(buf.pages_.size(), 1);
    buf.append(32);
    ASSERT_EQ(buf.capacity(), 512);
    ASSERT_EQ(buf.size(), 257);
    ASSERT_EQ(buf.pages_.size(), 2);

    buf.clear(true);
    ASSERT_EQ(buf.capacity(), 0);
    ASSERT_EQ(buf.size(), 0);
    ASSERT_EQ(buf.pages_.size(), 0);

    for (int i = 0; i <= 256; ++i) {
      buf.append(32);
    }
    ASSERT_EQ(buf.capacity(), 512);
    ASSERT_EQ(buf.size(), 257);
    ASSERT_EQ(buf.pages_.size(), 2);
    buf.clear(true);

    ASSERT_EQ(buf.capacity(), 0);
    ASSERT_EQ(buf.size(), 0);
    ASSERT_EQ(buf.pages_.size(), 0);

    for (int i = 0; i <= 2048; ++i) {
      buf.append(32);
    }
    ASSERT_EQ(buf.capacity(), 2304);
    ASSERT_EQ(buf.size(), 2049);
    ASSERT_EQ(buf.pages_.size(), 9);

    buf.clear(true);
    ASSERT_EQ(buf.capacity(), 0);
    ASSERT_EQ(buf.size(), 0);
    ASSERT_EQ(buf.pages_.size(), 0);

    for (int i = 0; i <= 2048; ++i) {
      buf.append(32);
    }
    ASSERT_EQ(buf.capacity(), 2304);
    ASSERT_EQ(buf.size(), 2049);
    ASSERT_EQ(buf.pages_.size(), 9);
  }
}
} // namespace common
} // namespace dwio
} // namespace velox
} // namespace facebook
