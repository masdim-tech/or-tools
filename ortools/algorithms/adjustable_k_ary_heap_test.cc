// Copyright 2010-2024 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/algorithms/adjustable_k_ary_heap.h"

#include <limits>
#include <queue>
#include <random>
#include <vector>

#include "gtest/gtest.h"

namespace operations_research {

class PriorityAggregate {
 public:
  using Index = int;
  using Priority = float;
  PriorityAggregate() = default;
  PriorityAggregate(Priority priority, Index index)
      : index_(index), priority_(priority) {}
  Priority priority() const { return priority_; }
  Index index() const { return index_; }
  inline bool operator<(const PriorityAggregate other) const {
    if (other.priority() != priority()) {
      return priority() < other.priority();
    }
    return index() < other.index();
  }

 private:
  Index index_;
  Priority priority_;
};

TEST(AdjustableKAryHeapTest, RandomDataStrongCheck) {
  const int kSize = 10'000;
  const double priority_range = kSize / 100;
  std::random_device rd;
  std::mt19937 generator(rd());  // Mersenne Twister generator
  std::uniform_real_distribution<float> priority_dist(0, priority_range);
  std::vector<PriorityAggregate> subsets_and_values(kSize);
  for (int i = 0; i < kSize; ++i) {
    subsets_and_values[i] = {priority_dist(generator), i};
  }

  AdjustableKAryHeap<PriorityAggregate, 5, true> heap(subsets_and_values,
                                                      kSize);
  EXPECT_TRUE(heap.CheckHeapProperty());
  float last = std::numeric_limits<float>::max();
  while (!heap.IsEmpty()) {
    const auto prio = heap.Top().priority();
    heap.Pop();
    EXPECT_LE(prio, last);
    last = prio;
  }
  EXPECT_TRUE(heap.IsEmpty());
  EXPECT_TRUE(heap.CheckHeapProperty());
}

TEST(AdjustableKAryHeapTest, RandomDataSpeed) {
  const int kSize = 1'000'000;
  const double priority_range = kSize / 100;
  std::random_device rd;
  std::mt19937 generator(rd());  // Mersenne Twister generator
  std::uniform_real_distribution<float> priority_dist(0, priority_range);
  std::vector<PriorityAggregate> subsets_and_values(kSize);
  for (int i = 0; i < kSize; ++i) {
    subsets_and_values[i] = {priority_dist(generator), i};
  }

  AdjustableKAryHeap<PriorityAggregate, 4, true> heap(subsets_and_values,
                                                      kSize);
  EXPECT_TRUE(heap.CheckHeapProperty());
  while (!heap.IsEmpty()) {
    heap.Pop();
  }
  EXPECT_TRUE(heap.CheckHeapProperty());
  EXPECT_TRUE(heap.IsEmpty());
}

TEST(AdjustableKAryHeapTest, UpdateStrongCheck) {
  const int kSize = 10'000;
  const int kNumUpdates = kSize / 100;
  const double priority_range = kSize / 100;
  std::random_device rd;
  std::mt19937 generator(rd());  // Mersenne Twister generator
  std::uniform_real_distribution<float> priority_dist(0, priority_range);
  std::uniform_int_distribution<int> index_dist(0, kSize);
  std::vector<PriorityAggregate> subsets_and_values(kSize);
  for (int i = 0; i < kSize; ++i) {
    subsets_and_values[i] = {priority_dist(generator), i};
  }
  AdjustableKAryHeap<PriorityAggregate, 4, true> heap(subsets_and_values,
                                                      kSize);
  EXPECT_TRUE(heap.CheckHeapProperty());
  for (int iter = 0; iter < kNumUpdates; ++iter) {
    heap.Update({priority_dist(generator), index_dist(generator)});
    EXPECT_TRUE(heap.CheckHeapProperty());
  }
}

TEST(AdjustableKAryHeapTest, RemoveStrongCheck) {
  const int kSize = 10'000;
  const int kNumRemovals = kSize;
  const double priority_range = kSize / 10;
  std::random_device rd;
  std::mt19937 generator(rd());  // Mersenne Twister generator
  std::uniform_real_distribution<float> priority_dist(0, priority_range);
  std::uniform_int_distribution<int> index_dist(0, kSize);
  std::vector<PriorityAggregate> subsets_and_values(kSize);
  for (int i = 0; i < kSize; ++i) {
    subsets_and_values[i] = {priority_dist(generator), i};
  }
  AdjustableKAryHeap<PriorityAggregate, 4, true> heap(subsets_and_values,
                                                      kSize);
  EXPECT_TRUE(heap.CheckHeapProperty());
  for (int iter = 0; iter < kNumRemovals; ++iter) {
    heap.Remove(iter);
    EXPECT_TRUE(heap.CheckHeapProperty());
  }
}

TEST(AdjustableKAryHeapTest, OneByOneStrongCheck) {
  const int kSize = 10'000;
  const int kNumInsertions = kSize;
  const double priority_range = kSize / 10;
  std::random_device rd;
  std::mt19937 generator(rd());  // Mersenne Twister generator
  std::uniform_real_distribution<float> priority_dist(0, priority_range);
  std::uniform_int_distribution<int> index_dist(0, kSize);
  std::vector<PriorityAggregate> subsets_and_values;
  AdjustableKAryHeap<PriorityAggregate, 4, true> heap;
  EXPECT_TRUE(heap.CheckHeapProperty());
  for (int iter = 0; iter < kNumInsertions; ++iter) {
    heap.Insert({priority_dist(generator), index_dist(generator)});
    EXPECT_TRUE(heap.CheckHeapProperty());
  }
}

TEST(AdjustableKAryHeapTest, OneByOneStrongSpeed) {
  const int kSize = 1'000'000;
  const int kNumInsertions = kSize;
  const double priority_range = kSize / 10;
  std::random_device rd;
  std::mt19937 generator(rd());  // Mersenne Twister generator
  std::uniform_real_distribution<float> priority_dist(0, priority_range);
  std::uniform_int_distribution<int> index_dist(0, kSize);
  std::vector<PriorityAggregate> subsets_and_values;
  AdjustableKAryHeap<PriorityAggregate, 4, true> heap;
  EXPECT_TRUE(heap.CheckHeapProperty());
  for (int iter = 0; iter < kNumInsertions; ++iter) {
    heap.Insert({priority_dist(generator), index_dist(generator)});
  }
  EXPECT_TRUE(heap.CheckHeapProperty());
}

TEST(StandardHeapTest, RandomDataSpeed) {
  const int kSize = 1'000'000;
  const double priority_range = kSize / 100;
  std::random_device rd;
  std::mt19937 generator(rd());  // Mersenne Twister generator
  std::uniform_real_distribution<float> priority_dist(0, priority_range);

  std::vector<float> values(kSize);
  for (int i = 0; i < kSize; ++i) {
    values[i] = priority_dist(generator);
  }
  std::priority_queue<float> heap(values.begin(), values.end());
  while (!heap.empty()) {
    heap.pop();
  }
}

}  // namespace operations_research
