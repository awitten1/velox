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

#include "velox/exec/SortWindowBuild.h"
#include "velox/exec/MemoryReclaimer.h"

namespace facebook::velox::exec {

namespace {
std::vector<CompareFlags> makeCompareFlags(
    int32_t numPartitionKeys,
    const std::vector<core::SortOrder>& sortingOrders) {
  std::vector<CompareFlags> compareFlags;
  compareFlags.reserve(numPartitionKeys + sortingOrders.size());

  for (auto i = 0; i < numPartitionKeys; ++i) {
    compareFlags.push_back({});
  }

  for (const auto& order : sortingOrders) {
    compareFlags.push_back(
        {order.isNullsFirst(), order.isAscending(), false /*equalsOnly*/});
  }

  return compareFlags;
}
} // namespace

SortWindowBuild::SortWindowBuild(
    const std::shared_ptr<const core::WindowNode>& node,
    velox::memory::MemoryPool* pool,
    common::PrefixSortConfig&& prefixSortConfig,
    const common::SpillConfig* spillConfig,
    tsan_atomic<bool>* nonReclaimableSection,
    folly::Synchronized<common::SpillStats>* spillStats)
    : WindowBuild(node, pool, spillConfig, nonReclaimableSection),
      numPartitionKeys_{node->partitionKeys().size()},
      compareFlags_{makeCompareFlags(numPartitionKeys_, node->sortingOrders())},
      pool_(pool),
      prefixSortConfig_(prefixSortConfig),
      spillStats_(spillStats),
      sortedRows_(0, memory::StlAllocator<char*>(*pool)),
      partitionStartRows_(0, memory::StlAllocator<char*>(*pool)) {
  VELOX_CHECK_NOT_NULL(pool_);
  allKeyInfo_.reserve(partitionKeyInfo_.size() + sortKeyInfo_.size());
  allKeyInfo_.insert(
      allKeyInfo_.cend(), partitionKeyInfo_.begin(), partitionKeyInfo_.end());
  allKeyInfo_.insert(
      allKeyInfo_.cend(), sortKeyInfo_.begin(), sortKeyInfo_.end());
  partitionStartRows_.resize(0);
}

void SortWindowBuild::addInput(RowVectorPtr input) {
  for (auto i = 0; i < inputChannels_.size(); ++i) {
    decodedInputVectors_[i].decode(*input->childAt(inputChannels_[i]));
  }

  ensureInputFits(input);

  // Add all the rows into the RowContainer.
  for (auto row = 0; row < input->size(); ++row) {
    char* newRow = data_->newRow();

    for (auto col = 0; col < input->childrenSize(); ++col) {
      data_->store(decodedInputVectors_[col], row, newRow, col);
    }
  }
  numRows_ += input->size();
}

void SortWindowBuild::ensureInputFits(const RowVectorPtr& input) {
  if (spillConfig_ == nullptr) {
    // Spilling is disabled.
    return;
  }

  if (data_->numRows() == 0) {
    // Nothing to spill.
    return;
  }

  // Test-only spill path.
  if (testingTriggerSpill(pool_->name())) {
    spill();
    return;
  }

  auto [freeRows, outOfLineFreeBytes] = data_->freeSpace();
  const auto outOfLineBytes =
      data_->stringAllocator().retainedSize() - outOfLineFreeBytes;
  const auto outOfLineBytesPerRow = outOfLineBytes / data_->numRows();

  const auto currentUsage = data_->pool()->usedBytes();
  const auto minReservationBytes =
      currentUsage * spillConfig_->minSpillableReservationPct / 100;
  const auto availableReservationBytes = data_->pool()->availableReservation();
  const auto incrementBytes =
      data_->sizeIncrement(input->size(), outOfLineBytesPerRow * input->size());

  // First to check if we have sufficient minimal memory reservation.
  if (availableReservationBytes >= minReservationBytes) {
    if ((freeRows > input->size()) &&
        (outOfLineBytes == 0 ||
         outOfLineFreeBytes >= outOfLineBytesPerRow * input->size())) {
      // Enough free rows for input rows and enough variable length free space.
      return;
    }
  }

  // Check if we can increase reservation. The increment is the largest of twice
  // the maximum increment from this input and 'spillableReservationGrowthPct_'
  // of the current memory usage.
  const auto targetIncrementBytes = std::max<int64_t>(
      incrementBytes * 2,
      currentUsage * spillConfig_->spillableReservationGrowthPct / 100);
  {
    memory::ReclaimableSectionGuard guard(nonReclaimableSection_);
    if (data_->pool()->maybeReserve(targetIncrementBytes)) {
      return;
    }
  }

  LOG(WARNING) << "Failed to reserve " << succinctBytes(targetIncrementBytes)
               << " for memory pool " << data_->pool()->name()
               << ", usage: " << succinctBytes(data_->pool()->usedBytes())
               << ", reservation: "
               << succinctBytes(data_->pool()->reservedBytes());
}

void SortWindowBuild::ensureSortFits() {
  // Check if spilling is enabled or not.
  if (spillConfig_ == nullptr) {
    return;
  }

  // Test-only spill path.
  if (testingTriggerSpill(pool_->name())) {
    spill();
    return;
  }

  if (spiller_ != nullptr) {
    return;
  }

  // The memory for std::vector sorted rows, `partitionStartRows_` and prefix
  // sort required buffer.
  uint64_t sortBufferToReserve =
      numRows_ * (sizeof(char*) + sizeof(vector_size_t)) +
      PrefixSort::maxRequiredBytes(
          data_.get(), compareFlags_, prefixSortConfig_, pool_);
  {
    memory::ReclaimableSectionGuard guard(nonReclaimableSection_);
    if (pool_->maybeReserve(sortBufferToReserve)) {
      return;
    }
  }

  LOG(WARNING) << fmt::format(
      "Failed to reserve {} for sort window build from memory pool {}, usage: {}, reservation: {}",
      succinctBytes(sortBufferToReserve),
      pool_->name(),
      succinctBytes(pool_->usedBytes()),
      succinctBytes(pool_->reservedBytes()));
}

void SortWindowBuild::setupSpiller() {
  VELOX_CHECK_NULL(spiller_);
  const auto sortingKeys = SpillState::makeSortingKeys(compareFlags_);
  spiller_ = std::make_unique<SortInputSpiller>(
      data_.get(), inputType_, sortingKeys, spillConfig_, spillStats_);
}

void SortWindowBuild::spill() {
  if (spiller_ == nullptr) {
    setupSpiller();
  }

  spiller_->spill();
  data_->clear();
  data_->pool()->release();
}

std::optional<common::SpillStats> SortWindowBuild::spilledStats() const {
  if (spiller_ == nullptr) {
    return std::nullopt;
  }
  return spiller_->stats();
}

// Use double front and back search algorithm to find next partition start row.
// It is more efficient than linear or binary search.
// This algorithm is described at
// https://medium.com/@insomniocode/search-algorithm-double-front-and-back-20f5f28512e7
vector_size_t SortWindowBuild::findNextPartitionStartRow(vector_size_t start) {
  auto partitionCompare = [&](const char* lhs, const char* rhs) -> bool {
    return compareRowsWithKeys(lhs, rhs, partitionKeyInfo_);
  };

  auto left = start;
  auto right = left + 1;
  auto lastPosition = sortedRows_.size();
  while (right < lastPosition) {
    auto distance = 1;
    for (; distance < lastPosition - left; distance *= 2) {
      right = left + distance;
      if (partitionCompare(sortedRows_[left], sortedRows_[right]) != 0) {
        lastPosition = right;
        break;
      }
    }
    left += distance / 2;
    right = left + 1;
  }
  return right;
}

void SortWindowBuild::computePartitionStartRows() {
  partitionStartRows_.reserve(numRows_);

  // Using a sequential traversal to find changing partitions.
  // This algorithm is inefficient and can be changed
  // i) Use a binary search kind of strategy.
  // ii) If we use a Hashtable instead of a full sort then the count
  // of rows in the partition can be directly used.
  partitionStartRows_.push_back(0);

  VELOX_CHECK_GT(sortedRows_.size(), 0);

  vector_size_t start = 0;
  while (start < sortedRows_.size()) {
    auto next = findNextPartitionStartRow(start);
    partitionStartRows_.push_back(next);
    start = next;
  }
}

void SortWindowBuild::sortPartitions() {
  // This is a very inefficient but easy implementation to order the input rows
  // by partition keys + sort keys.
  // Sort the pointers to the rows in RowContainer (data_) instead of sorting
  // the rows.
  sortedRows_.resize(numRows_);
  RowContainerIterator iter;
  data_->listRows(&iter, numRows_, sortedRows_.data());

  PrefixSort::sort(
      data_.get(), compareFlags_, prefixSortConfig_, pool_, sortedRows_);

  computePartitionStartRows();
}

void SortWindowBuild::noMoreInput() {
  if (numRows_ == 0) {
    return;
  }

  ensureSortFits();

  if (spiller_ != nullptr) {
    // Spill remaining data to avoid running out of memory while sort-merging
    // spilled data.
    spill();

    VELOX_CHECK_NULL(merge_);
    SpillPartitionSet spillPartitionSet;
    spiller_->finishSpill(spillPartitionSet);
    VELOX_CHECK_EQ(spillPartitionSet.size(), 1);
    merge_ = spillPartitionSet.begin()->second->createOrderedReader(
        spillConfig_->readBufferSize, pool_, spillStats_);
  } else {
    // At this point we have seen all the input rows. The operator is
    // being prepared to output rows now.
    // To prepare the rows for output in SortWindowBuild they need to
    // be separated into partitions and sort by ORDER BY keys within
    // the partition. This will order the rows for getOutput().
    sortPartitions();
  }

  // Releases the unused memory reservation after procesing input.
  pool_->release();
}

void SortWindowBuild::loadNextPartitionFromSpill() {
  sortedRows_.clear();
  sortedRows_.shrink_to_fit();
  data_->clear();

  for (;;) {
    auto next = merge_->next();
    if (next == nullptr) {
      break;
    }

    bool newPartition = false;
    if (!sortedRows_.empty()) {
      CompareFlags compareFlags =
          CompareFlags::equality(CompareFlags::NullHandlingMode::kNullAsValue);

      for (auto i = 0; i < numPartitionKeys_; ++i) {
        if (data_->compare(
                sortedRows_.back(),
                data_->columnAt(i),
                next->decoded(i),
                next->currentIndex(),
                compareFlags)) {
          newPartition = true;
          break;
        }
      }
    }

    if (newPartition) {
      break;
    }

    auto* newRow = data_->newRow();
    for (auto i = 0; i < inputChannels_.size(); ++i) {
      data_->store(next->decoded(i), next->currentIndex(), newRow, i);
    }
    sortedRows_.push_back(newRow);
    next->pop();
  }
}

std::shared_ptr<WindowPartition> SortWindowBuild::nextPartition() {
  if (merge_ != nullptr) {
    VELOX_CHECK(!sortedRows_.empty(), "No window partitions available");
    auto partition = folly::Range(sortedRows_.data(), sortedRows_.size());
    return std::make_shared<WindowPartition>(
        data_.get(), partition, inversedInputChannels_, sortKeyInfo_);
  }

  VELOX_CHECK(!partitionStartRows_.empty(), "No window partitions available");

  currentPartition_++;
  VELOX_CHECK_LE(
      currentPartition_,
      partitionStartRows_.size() - 2,
      "All window partitions consumed");

  // There is partition data available now.
  auto partitionSize = partitionStartRows_[currentPartition_ + 1] -
      partitionStartRows_[currentPartition_];
  auto partition = folly::Range(
      sortedRows_.data() + partitionStartRows_[currentPartition_],
      partitionSize);
  return std::make_shared<WindowPartition>(
      data_.get(), partition, inversedInputChannels_, sortKeyInfo_);
}

bool SortWindowBuild::hasNextPartition() {
  if (merge_ != nullptr) {
    loadNextPartitionFromSpill();
    return !sortedRows_.empty();
  }

  return partitionStartRows_.size() > 0 &&
      currentPartition_ < static_cast<int>(partitionStartRows_.size() - 2);
}
} // namespace facebook::velox::exec
