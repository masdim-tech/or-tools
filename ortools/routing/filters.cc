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

// Implementation of local search filters for routing models.

#include "ortools/routing/filters.h"

#include <stddef.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "ortools/base/logging.h"
#include "ortools/base/map_util.h"
#include "ortools/base/strong_vector.h"
#include "ortools/base/types.h"
#include "ortools/constraint_solver/constraint_solver.h"
#include "ortools/constraint_solver/constraint_solveri.h"
#include "ortools/routing/lp_scheduling.h"
#include "ortools/routing/parameters.pb.h"
#include "ortools/routing/routing.h"
#include "ortools/routing/types.h"
#include "ortools/util/bitset.h"
#include "ortools/util/piecewise_linear_function.h"
#include "ortools/util/saturated_arithmetic.h"

ABSL_FLAG(bool, routing_strong_debug_checks, false,
          "Run stronger checks in debug; these stronger tests might change "
          "the complexity of the code in particular.");

namespace operations_research::routing {

namespace {
// Route constraint filter

class RouteConstraintFilter : public BasePathFilter {
 public:
  explicit RouteConstraintFilter(const RoutingModel& routing_model)
      : BasePathFilter(routing_model.Nexts(),
                       routing_model.Size() + routing_model.vehicles(),
                       routing_model.GetPathsMetadata()),
        routing_model_(routing_model),
        current_vehicle_cost_(0),
        delta_vehicle_cost_(0),
        current_vehicle_costs_(routing_model.vehicles(), 0) {
    start_to_vehicle_.resize(Size(), -1);
    vehicle_to_start_.resize(routing_model.vehicles());
    for (int v = 0; v < routing_model.vehicles(); v++) {
      const int64_t start = routing_model.Start(v);
      start_to_vehicle_[start] = v;
      vehicle_to_start_[v] = start;
    }
  }
  ~RouteConstraintFilter() override = default;
  std::string DebugString() const override { return "RouteConstraintFilter"; }
  int64_t GetSynchronizedObjectiveValue() const override {
    return current_vehicle_cost_;
  }
  int64_t GetAcceptedObjectiveValue() const override {
    return lns_detected() ? 0 : delta_vehicle_cost_;
  }

 private:
  void OnSynchronizePathFromStart(int64_t start) override {
    route_.clear();
    int64_t node = start;
    while (node < Size()) {
      route_.push_back(node);
      node = Value(node);
    }
    route_.push_back(node);
    std::optional<int64_t> route_cost = routing_model_.GetRouteCost(route_);
    DCHECK(route_cost.has_value());
    current_vehicle_costs_[start_to_vehicle_[start]] = route_cost.value();
  }
  void OnAfterSynchronizePaths() override {
    current_vehicle_cost_ = 0;
    for (int vehicle = 0; vehicle < vehicle_to_start_.size(); vehicle++) {
      const int64_t start = vehicle_to_start_[vehicle];
      DCHECK_EQ(vehicle, start_to_vehicle_[start]);
      if (!IsVarSynced(start)) {
        return;
      }
      CapAddTo(current_vehicle_costs_[vehicle], &current_vehicle_cost_);
    }
  }
  bool InitializeAcceptPath() override {
    delta_vehicle_cost_ = current_vehicle_cost_;
    return true;
  }
  bool AcceptPath(int64_t path_start, int64_t /*chain_start*/,
                  int64_t /*chain_end*/) override {
    delta_vehicle_cost_ =
        CapSub(delta_vehicle_cost_,
               current_vehicle_costs_[start_to_vehicle_[path_start]]);
    route_.clear();
    int64_t node = path_start;
    while (node < Size()) {
      route_.push_back(node);
      node = GetNext(node);
    }
    route_.push_back(node);
    std::optional<int64_t> route_cost = routing_model_.GetRouteCost(route_);
    if (!route_cost.has_value()) {
      return false;
    }
    CapAddTo(route_cost.value(), &delta_vehicle_cost_);
    return true;
  }
  bool FinalizeAcceptPath(int64_t /*objective_min*/,
                          int64_t objective_max) override {
    return delta_vehicle_cost_ <= objective_max;
  }

  const RoutingModel& routing_model_;
  int64_t current_vehicle_cost_;
  int64_t delta_vehicle_cost_;
  std::vector<int64_t> current_vehicle_costs_;
  std::vector<int64_t> vehicle_to_start_;
  std::vector<int> start_to_vehicle_;
  std::vector<int64_t> route_;
};

}  // namespace

IntVarLocalSearchFilter* MakeRouteConstraintFilter(
    const RoutingModel& routing_model) {
  return routing_model.solver()->RevAlloc(
      new RouteConstraintFilter(routing_model));
}

namespace {

// Max active vehicles filter.

class MaxActiveVehiclesFilter : public IntVarLocalSearchFilter {
 public:
  explicit MaxActiveVehiclesFilter(const RoutingModel& routing_model)
      : IntVarLocalSearchFilter(routing_model.Nexts()),
        routing_model_(routing_model),
        is_active_(routing_model.vehicles(), false),
        active_vehicles_(0) {}
  bool Accept(const Assignment* delta, const Assignment* /*deltadelta*/,
              int64_t /*objective_min*/, int64_t /*objective_max*/) override {
    const int64_t kUnassigned = -1;
    const Assignment::IntContainer& container = delta->IntVarContainer();
    int current_active_vehicles = active_vehicles_;
    for (const IntVarElement& new_element : container.elements()) {
      IntVar* const var = new_element.Var();
      int64_t index = kUnassigned;
      if (FindIndex(var, &index) && routing_model_.IsStart(index)) {
        if (new_element.Min() != new_element.Max()) {
          // LNS detected.
          return true;
        }
        const int vehicle = routing_model_.VehicleIndex(index);
        const bool is_active =
            (new_element.Min() != routing_model_.End(vehicle));
        if (is_active && !is_active_[vehicle]) {
          ++current_active_vehicles;
        } else if (!is_active && is_active_[vehicle]) {
          --current_active_vehicles;
        }
      }
    }
    return current_active_vehicles <=
           routing_model_.GetMaximumNumberOfActiveVehicles();
  }

 private:
  void OnSynchronize(const Assignment* /*delta*/) override {
    active_vehicles_ = 0;
    for (int i = 0; i < routing_model_.vehicles(); ++i) {
      const int index = routing_model_.Start(i);
      if (IsVarSynced(index) && Value(index) != routing_model_.End(i)) {
        is_active_[i] = true;
        ++active_vehicles_;
      } else {
        is_active_[i] = false;
      }
    }
  }

  const RoutingModel& routing_model_;
  std::vector<bool> is_active_;
  int active_vehicles_;
};
}  // namespace

IntVarLocalSearchFilter* MakeMaxActiveVehiclesFilter(
    const RoutingModel& routing_model) {
  return routing_model.solver()->RevAlloc(
      new MaxActiveVehiclesFilter(routing_model));
}

namespace {

class ActiveNodeGroupFilter : public IntVarLocalSearchFilter {
 public:
  explicit ActiveNodeGroupFilter(const RoutingModel& routing_model)
      : IntVarLocalSearchFilter(routing_model.Nexts()),
        routing_model_(routing_model),
        active_count_per_group_(routing_model.GetSameActivityGroupsCount(),
                                {.active = 0, .unknown = 0}),
        node_is_active_(routing_model.Nexts().size(), false),
        node_is_unknown_(routing_model.Nexts().size(), false) {}

  bool Accept(const Assignment* delta, const Assignment* /*deltadelta*/,
              int64_t /*objective_min*/, int64_t /*objective_max*/) override {
    active_count_per_group_.Revert();
    const Assignment::IntContainer& container = delta->IntVarContainer();
    for (const IntVarElement& new_element : container.elements()) {
      IntVar* const var = new_element.Var();
      int64_t index = -1;
      if (!FindIndex(var, &index)) continue;
      const int group = routing_model_.GetSameActivityGroupOfIndex(index);
      ActivityCounts counts = active_count_per_group_.Get(group);
      // Change contribution to counts: remove old state, add new state.
      if (node_is_unknown_[index]) --counts.unknown;
      if (node_is_active_[index]) --counts.active;
      if (new_element.Min() != new_element.Max()) {
        ++counts.unknown;
      } else if (new_element.Min() != index) {
        ++counts.active;
      }
      active_count_per_group_.Set(group, counts);
    }
    for (const int group : active_count_per_group_.ChangedIndices()) {
      const ActivityCounts counts = active_count_per_group_.Get(group);
      const int group_size =
          routing_model_.GetSameActivityIndicesOfGroup(group).size();
      // The group constraint is respected iff either 0 or group size is inside
      // interval [num_active, num_active + num_unknown],
      if (counts.active == 0) continue;
      if (counts.active <= group_size &&
          group_size <= counts.active + counts.unknown) {
        continue;
      }
      return false;
    }
    return true;
  }
  std::string DebugString() const override { return "ActiveNodeGroupFilter"; }

 private:
  void OnSynchronize(const Assignment* /*delta*/) override {
    const int num_groups = routing_model_.GetSameActivityGroupsCount();
    for (int group = 0; group < num_groups; ++group) {
      ActivityCounts counts = {.active = 0, .unknown = 0};
      for (int node : routing_model_.GetSameActivityIndicesOfGroup(group)) {
        if (IsVarSynced(node)) {
          const bool is_active = (Value(node) != node);
          node_is_active_[node] = is_active;
          node_is_unknown_[node] = false;
          counts.active += is_active ? 1 : 0;
        } else {
          ++counts.unknown;
          node_is_unknown_[node] = true;
          node_is_active_[node] = false;
        }
      }
      active_count_per_group_.Set(group, counts);
    }
    active_count_per_group_.Commit();
  }

  const RoutingModel& routing_model_;
  struct ActivityCounts {
    int active;
    int unknown;
  };
  CommittableVector<ActivityCounts> active_count_per_group_;
  // node_is_active_[node] is true iff node was synced and active at last
  // Synchronize().
  std::vector<bool> node_is_active_;
  // node_is_unknown_[node] is true iff node was not synced at last
  // Synchronize().
  std::vector<bool> node_is_unknown_;
};

}  // namespace

IntVarLocalSearchFilter* MakeActiveNodeGroupFilter(
    const RoutingModel& routing_model) {
  return routing_model.solver()->RevAlloc(
      new ActiveNodeGroupFilter(routing_model));
}

namespace {

// Node disjunction filter class.
class NodeDisjunctionFilter : public IntVarLocalSearchFilter {
 public:
  explicit NodeDisjunctionFilter(const RoutingModel& routing_model,
                                 bool filter_cost)
      : IntVarLocalSearchFilter(routing_model.Nexts()),
        routing_model_(routing_model),
        count_per_disjunction_(routing_model.GetNumberOfDisjunctions(),
                               {.active = 0, .inactive = 0}),
        synchronized_objective_value_(std::numeric_limits<int64_t>::min()),
        accepted_objective_value_(std::numeric_limits<int64_t>::min()),
        filter_cost_(filter_cost),
        has_mandatory_disjunctions_(routing_model.HasMandatoryDisjunctions()) {}

  using Disjunction = RoutingModel::DisjunctionIndex;

  bool Accept(const Assignment* delta, const Assignment* /*deltadelta*/,
              int64_t /*objective_min*/, int64_t objective_max) override {
    count_per_disjunction_.Revert();
    bool lns_detected = false;
    // Update the active/inactive counts of each modified disjunction.
    for (const IntVarElement& element : delta->IntVarContainer().elements()) {
      int64_t node = -1;
      if (!FindIndex(element.Var(), &node)) continue;
      lns_detected |= element.Min() != element.Max();
      // Compute difference in how this node contributes to activity counts.
      const bool is_var_synced = IsVarSynced(node);
      const bool was_active = is_var_synced && Value(node) != node;
      const bool is_active = node < element.Min() || element.Max() < node;
      ActivityCount contribution_delta = {.active = 0, .inactive = 0};
      if (is_var_synced) {
        contribution_delta.active -= was_active;
        contribution_delta.inactive -= !was_active;
      }
      contribution_delta.active += is_active;
      contribution_delta.inactive += !is_active;
      // Common shortcut: if the change is neutral, counts stay the same.
      if (contribution_delta.active == 0 && contribution_delta.inactive == 0) {
        continue;
      }
      // Change counts of all disjunctions affected by this node.
      for (const Disjunction disjunction :
           routing_model_.GetDisjunctionIndices(node)) {
        ActivityCount new_count =
            count_per_disjunction_.Get(disjunction.value());
        new_count.active += contribution_delta.active;
        new_count.inactive += contribution_delta.inactive;
        count_per_disjunction_.Set(disjunction.value(), new_count);
      }
    }
    // Check if any disjunction has too many active nodes.
    for (const int index : count_per_disjunction_.ChangedIndices()) {
      if (count_per_disjunction_.Get(index).active >
          routing_model_.GetDisjunctionMaxCardinality(Disjunction(index))) {
        return false;
      }
    }
    if (lns_detected || (!filter_cost_ && !has_mandatory_disjunctions_)) {
      accepted_objective_value_ = 0;
      return true;
    }
    // Update penalty costs for disjunctions.
    accepted_objective_value_ = synchronized_objective_value_;
    for (const int index : count_per_disjunction_.ChangedIndices()) {
      // If num inactives did not change, skip. Common shortcut.
      const int old_inactives =
          count_per_disjunction_.GetCommitted(index).inactive;
      const int new_inactives = count_per_disjunction_.Get(index).inactive;
      if (old_inactives == new_inactives) continue;
      // If this disjunction has no penalty for inactive nodes, skip.
      const Disjunction disjunction(index);
      const int64_t penalty = routing_model_.GetDisjunctionPenalty(disjunction);
      if (penalty == 0) continue;

      // Compute the new cost of activity bound violations.
      const int max_inactives =
          routing_model_.GetDisjunctionNodeIndices(disjunction).size() -
          routing_model_.GetDisjunctionMaxCardinality(disjunction);
      int new_violation = std::max(0, new_inactives - max_inactives);
      int old_violation = std::max(0, old_inactives - max_inactives);
      // If nodes are mandatory, there can be no violation.
      if (penalty < 0 && new_violation > 0) return false;
      if (routing_model_.GetDisjunctionPenaltyCostBehavior(disjunction) ==
          RoutingModel::PenaltyCostBehavior::PENALIZE_ONCE) {
        new_violation = std::min(1, new_violation);
        old_violation = std::min(1, old_violation);
      }
      CapAddTo(CapProd(penalty, (new_violation - old_violation)),
               &accepted_objective_value_);
    }
    // Only compare to max as a cost lower bound is computed.
    return accepted_objective_value_ <= objective_max;
  }
  std::string DebugString() const override { return "NodeDisjunctionFilter"; }
  int64_t GetSynchronizedObjectiveValue() const override {
    return synchronized_objective_value_;
  }
  int64_t GetAcceptedObjectiveValue() const override {
    return accepted_objective_value_;
  }

 private:
  void OnSynchronize(const Assignment* /*delta*/) override {
    synchronized_objective_value_ = 0;
    count_per_disjunction_.Revert();
    const int num_disjunctions = routing_model_.GetNumberOfDisjunctions();
    for (Disjunction disjunction(0); disjunction < num_disjunctions;
         ++disjunction) {
      // Count number of active/inactive nodes of this disjunction.
      ActivityCount count = {.active = 0, .inactive = 0};
      const auto& nodes = routing_model_.GetDisjunctionNodeIndices(disjunction);
      for (const int64_t node : nodes) {
        if (!IsVarSynced(node)) continue;
        const int is_active = Value(node) != node;
        count.active += is_active;
        count.inactive += !is_active;
      }
      count_per_disjunction_.Set(disjunction.value(), count);
      // Add penalty of this disjunction to total cost.
      if (!filter_cost_) continue;
      const int64_t penalty = routing_model_.GetDisjunctionPenalty(disjunction);
      const int max_actives =
          routing_model_.GetDisjunctionMaxCardinality(disjunction);
      int violation = count.inactive - (nodes.size() - max_actives);
      if (violation > 0 && penalty > 0) {
        if (routing_model_.GetDisjunctionPenaltyCostBehavior(disjunction) ==
            RoutingModel::PenaltyCostBehavior::PENALIZE_ONCE) {
          violation = std::min(1, violation);
        }
        CapAddTo(CapProd(penalty, violation), &synchronized_objective_value_);
      }
    }
    count_per_disjunction_.Commit();
    accepted_objective_value_ = synchronized_objective_value_;
  }

  const RoutingModel& routing_model_;
  struct ActivityCount {
    int active = 0;
    int inactive = 0;
  };
  CommittableVector<ActivityCount> count_per_disjunction_;
  int64_t synchronized_objective_value_;
  int64_t accepted_objective_value_;
  const bool filter_cost_;
  const bool has_mandatory_disjunctions_;
};
}  // namespace

IntVarLocalSearchFilter* MakeNodeDisjunctionFilter(
    const RoutingModel& routing_model, bool filter_cost) {
  return routing_model.solver()->RevAlloc(
      new NodeDisjunctionFilter(routing_model, filter_cost));
}

const int64_t BasePathFilter::kUnassigned = -1;

BasePathFilter::BasePathFilter(const std::vector<IntVar*>& nexts,
                               int next_domain_size,
                               const PathsMetadata& paths_metadata)
    : IntVarLocalSearchFilter(nexts),
      paths_metadata_(paths_metadata),
      node_path_starts_(next_domain_size, kUnassigned),
      new_synchronized_unperformed_nodes_(nexts.size()),
      new_nexts_(nexts.size(), kUnassigned),
      touched_paths_(nexts.size()),
      touched_path_chain_start_ends_(nexts.size(), {kUnassigned, kUnassigned}),
      ranks_(next_domain_size, kUnassigned),
      status_(BasePathFilter::UNKNOWN),
      lns_detected_(false) {}

bool BasePathFilter::Accept(const Assignment* delta,
                            const Assignment* /*deltadelta*/,
                            int64_t objective_min, int64_t objective_max) {
  if (IsDisabled()) return true;
  lns_detected_ = false;
  for (const int touched : delta_touched_) {
    new_nexts_[touched] = kUnassigned;
  }
  delta_touched_.clear();
  const Assignment::IntContainer& container = delta->IntVarContainer();
  delta_touched_.reserve(container.Size());
  // Determining touched paths and their touched chain start and ends (a node is
  // touched if it corresponds to an element of delta or that an element of
  // delta points to it).
  // The start and end of a touched path subchain will have remained on the same
  // path and will correspond to the min and max ranks of touched nodes in the
  // current assignment.
  for (int64_t touched_path : touched_paths_.PositionsSetAtLeastOnce()) {
    touched_path_chain_start_ends_[touched_path] = {kUnassigned, kUnassigned};
  }
  touched_paths_.SparseClearAll();

  const auto update_touched_path_chain_start_end = [this](int64_t index) {
    const int64_t start = node_path_starts_[index];
    if (start == kUnassigned) return;
    touched_paths_.Set(start);

    int64_t& chain_start = touched_path_chain_start_ends_[start].first;
    if (chain_start == kUnassigned || paths_metadata_.IsStart(index) ||
        ranks_[index] < ranks_[chain_start]) {
      chain_start = index;
    }

    int64_t& chain_end = touched_path_chain_start_ends_[start].second;
    if (chain_end == kUnassigned || paths_metadata_.IsEnd(index) ||
        ranks_[index] > ranks_[chain_end]) {
      chain_end = index;
    }
  };

  for (const IntVarElement& new_element : container.elements()) {
    IntVar* const var = new_element.Var();
    int64_t index = kUnassigned;
    if (FindIndex(var, &index)) {
      if (!new_element.Bound()) {
        // LNS detected
        lns_detected_ = true;
        return true;
      }
      new_nexts_[index] = new_element.Value();
      delta_touched_.push_back(index);
      update_touched_path_chain_start_end(index);
      update_touched_path_chain_start_end(new_nexts_[index]);
    }
  }
  // Checking feasibility of touched paths.
  if (!InitializeAcceptPath()) return false;
  for (const int64_t touched_start : touched_paths_.PositionsSetAtLeastOnce()) {
    const std::pair<int64_t, int64_t> start_end =
        touched_path_chain_start_ends_[touched_start];
    if (!AcceptPath(touched_start, start_end.first, start_end.second)) {
      return false;
    }
  }
  // NOTE: FinalizeAcceptPath() is only called if InitializeAcceptPath() is true
  // and all paths are accepted.
  return FinalizeAcceptPath(objective_min, objective_max);
}

void BasePathFilter::ComputePathStarts(std::vector<int64_t>* path_starts,
                                       std::vector<int>* index_to_path) {
  path_starts->clear();
  const int nexts_size = Size();
  index_to_path->assign(nexts_size, kUnassigned);
  Bitset64<> has_prevs(nexts_size);
  for (int i = 0; i < nexts_size; ++i) {
    if (!IsVarSynced(i)) {
      has_prevs.Set(i);
    } else {
      const int next = Value(i);
      if (next < nexts_size) {
        has_prevs.Set(next);
      }
    }
  }
  for (int i = 0; i < nexts_size; ++i) {
    if (!has_prevs[i]) {
      (*index_to_path)[i] = path_starts->size();
      path_starts->push_back(i);
    }
  }
}

void BasePathFilter::SynchronizeFullAssignment() {
  for (int64_t index = 0; index < Size(); index++) {
    if (IsVarSynced(index) && Value(index) == index &&
        node_path_starts_[index] != kUnassigned) {
      // index was performed before and is now unperformed.
      new_synchronized_unperformed_nodes_.Set(index);
    }
  }
  // Marking unactive nodes (which are not on a path).
  node_path_starts_.assign(node_path_starts_.size(), kUnassigned);
  // Marking nodes on a path and storing next values.
  const int nexts_size = Size();
  for (int path = 0; path < NumPaths(); ++path) {
    const int64_t start = Start(path);
    node_path_starts_[start] = start;
    if (IsVarSynced(start)) {
      int64_t next = Value(start);
      while (next < nexts_size) {
        int64_t node = next;
        node_path_starts_[node] = start;
        DCHECK(IsVarSynced(node));
        next = Value(node);
      }
      node_path_starts_[next] = start;
    }
    node_path_starts_[End(path)] = start;
  }
  for (const int touched : delta_touched_) {
    new_nexts_[touched] = kUnassigned;
  }
  delta_touched_.clear();
  OnBeforeSynchronizePaths();
  UpdateAllRanks();
  OnAfterSynchronizePaths();
}

void BasePathFilter::OnSynchronize(const Assignment* delta) {
  if (status_ == BasePathFilter::UNKNOWN) {
    status_ =
        DisableFiltering() ? BasePathFilter::DISABLED : BasePathFilter::ENABLED;
  }
  if (IsDisabled()) return;
  new_synchronized_unperformed_nodes_.ClearAll();
  if (delta == nullptr || delta->Empty() ||
      absl::c_all_of(ranks_, [](int rank) { return rank == kUnassigned; })) {
    SynchronizeFullAssignment();
    return;
  }
  const Assignment::IntContainer& container = delta->IntVarContainer();
  touched_paths_.SparseClearAll();
  for (const IntVarElement& new_element : container.elements()) {
    int64_t index = kUnassigned;
    if (FindIndex(new_element.Var(), &index)) {
      const int64_t start = node_path_starts_[index];
      if (start != kUnassigned) {
        touched_paths_.Set(start);
        if (Value(index) == index) {
          // New unperformed node (its previous start isn't unassigned).
          DCHECK_LT(index, new_nexts_.size());
          new_synchronized_unperformed_nodes_.Set(index);
          node_path_starts_[index] = kUnassigned;
        }
      }
    }
  }
  for (const int touched : delta_touched_) {
    new_nexts_[touched] = kUnassigned;
  }
  delta_touched_.clear();
  OnBeforeSynchronizePaths();
  for (const int64_t touched_start : touched_paths_.PositionsSetAtLeastOnce()) {
    int64_t node = touched_start;
    while (node < Size()) {
      node_path_starts_[node] = touched_start;
      node = Value(node);
    }
    node_path_starts_[node] = touched_start;
    UpdatePathRanksFromStart(touched_start);
    OnSynchronizePathFromStart(touched_start);
  }
  OnAfterSynchronizePaths();
}

void BasePathFilter::UpdateAllRanks() {
  ranks_.assign(ranks_.size(), kUnassigned);
  for (int r = 0; r < NumPaths(); ++r) {
    const int64_t start = Start(r);
    if (!IsVarSynced(start)) continue;
    UpdatePathRanksFromStart(start);
    OnSynchronizePathFromStart(start);
  }
}

void BasePathFilter::UpdatePathRanksFromStart(int start) {
  int rank = 0;
  int64_t node = start;
  while (node < Size()) {
    ranks_[node] = rank;
    rank++;
    node = Value(node);
  }
  ranks_[node] = rank;
}

namespace {

class VehicleAmortizedCostFilter : public BasePathFilter {
 public:
  explicit VehicleAmortizedCostFilter(const RoutingModel& routing_model);
  ~VehicleAmortizedCostFilter() override = default;
  std::string DebugString() const override {
    return "VehicleAmortizedCostFilter";
  }
  int64_t GetSynchronizedObjectiveValue() const override {
    return current_vehicle_cost_;
  }
  int64_t GetAcceptedObjectiveValue() const override {
    return lns_detected() ? 0 : delta_vehicle_cost_;
  }

 private:
  void OnSynchronizePathFromStart(int64_t start) override;
  void OnAfterSynchronizePaths() override;
  bool InitializeAcceptPath() override;
  bool AcceptPath(int64_t path_start, int64_t chain_start,
                  int64_t chain_end) override;
  bool FinalizeAcceptPath(int64_t objective_min,
                          int64_t objective_max) override;

  int64_t current_vehicle_cost_;
  int64_t delta_vehicle_cost_;
  std::vector<int> current_route_lengths_;
  std::vector<int64_t> start_to_end_;
  std::vector<int> start_to_vehicle_;
  std::vector<int64_t> vehicle_to_start_;
  const std::vector<int64_t>& linear_cost_factor_of_vehicle_;
  const std::vector<int64_t>& quadratic_cost_factor_of_vehicle_;
};

VehicleAmortizedCostFilter::VehicleAmortizedCostFilter(
    const RoutingModel& routing_model)
    : BasePathFilter(routing_model.Nexts(),
                     routing_model.Size() + routing_model.vehicles(),
                     routing_model.GetPathsMetadata()),
      current_vehicle_cost_(0),
      delta_vehicle_cost_(0),
      current_route_lengths_(Size(), -1),
      linear_cost_factor_of_vehicle_(
          routing_model.GetAmortizedLinearCostFactorOfVehicles()),
      quadratic_cost_factor_of_vehicle_(
          routing_model.GetAmortizedQuadraticCostFactorOfVehicles()) {
  start_to_end_.resize(Size(), -1);
  start_to_vehicle_.resize(Size(), -1);
  vehicle_to_start_.resize(routing_model.vehicles());
  for (int v = 0; v < routing_model.vehicles(); v++) {
    const int64_t start = routing_model.Start(v);
    start_to_vehicle_[start] = v;
    start_to_end_[start] = routing_model.End(v);
    vehicle_to_start_[v] = start;
  }
}

void VehicleAmortizedCostFilter::OnSynchronizePathFromStart(int64_t start) {
  const int64_t end = start_to_end_[start];
  CHECK_GE(end, 0);
  const int route_length = Rank(end) - 1;
  CHECK_GE(route_length, 0);
  current_route_lengths_[start] = route_length;
}

void VehicleAmortizedCostFilter::OnAfterSynchronizePaths() {
  current_vehicle_cost_ = 0;
  for (int vehicle = 0; vehicle < vehicle_to_start_.size(); vehicle++) {
    const int64_t start = vehicle_to_start_[vehicle];
    DCHECK_EQ(vehicle, start_to_vehicle_[start]);
    if (!IsVarSynced(start)) {
      return;
    }
    const int route_length = current_route_lengths_[start];
    DCHECK_GE(route_length, 0);

    if (route_length == 0) {
      // The path is empty.
      continue;
    }

    const int64_t linear_cost_factor = linear_cost_factor_of_vehicle_[vehicle];
    const int64_t route_length_cost =
        CapProd(quadratic_cost_factor_of_vehicle_[vehicle],
                route_length * route_length);

    CapAddTo(CapSub(linear_cost_factor, route_length_cost),
             &current_vehicle_cost_);
  }
}

bool VehicleAmortizedCostFilter::InitializeAcceptPath() {
  delta_vehicle_cost_ = current_vehicle_cost_;
  return true;
}

bool VehicleAmortizedCostFilter::AcceptPath(int64_t path_start,
                                            int64_t chain_start,
                                            int64_t chain_end) {
  // Number of nodes previously between chain_start and chain_end
  const int previous_chain_nodes = Rank(chain_end) - 1 - Rank(chain_start);
  CHECK_GE(previous_chain_nodes, 0);
  int new_chain_nodes = 0;
  int64_t node = GetNext(chain_start);
  while (node != chain_end) {
    new_chain_nodes++;
    node = GetNext(node);
  }

  const int previous_route_length = current_route_lengths_[path_start];
  CHECK_GE(previous_route_length, 0);
  const int new_route_length =
      previous_route_length - previous_chain_nodes + new_chain_nodes;

  const int vehicle = start_to_vehicle_[path_start];
  CHECK_GE(vehicle, 0);
  DCHECK_EQ(path_start, vehicle_to_start_[vehicle]);

  // Update the cost related to used vehicles.
  // TODO(user): Handle possible overflows.
  if (previous_route_length == 0) {
    // The route was empty before, it is no longer the case (changed path).
    CHECK_GT(new_route_length, 0);
    CapAddTo(linear_cost_factor_of_vehicle_[vehicle], &delta_vehicle_cost_);
  } else if (new_route_length == 0) {
    // The route is now empty.
    delta_vehicle_cost_ =
        CapSub(delta_vehicle_cost_, linear_cost_factor_of_vehicle_[vehicle]);
  }

  // Update the cost related to the sum of the squares of the route lengths.
  const int64_t quadratic_cost_factor =
      quadratic_cost_factor_of_vehicle_[vehicle];
  CapAddTo(CapProd(quadratic_cost_factor,
                   previous_route_length * previous_route_length),
           &delta_vehicle_cost_);
  delta_vehicle_cost_ = CapSub(
      delta_vehicle_cost_,
      CapProd(quadratic_cost_factor, new_route_length * new_route_length));

  return true;
}

bool VehicleAmortizedCostFilter::FinalizeAcceptPath(int64_t /*objective_min*/,
                                                    int64_t objective_max) {
  return delta_vehicle_cost_ <= objective_max;
}

}  // namespace

IntVarLocalSearchFilter* MakeVehicleAmortizedCostFilter(
    const RoutingModel& routing_model) {
  return routing_model.solver()->RevAlloc(
      new VehicleAmortizedCostFilter(routing_model));
}

namespace {

class TypeRegulationsFilter : public BasePathFilter {
 public:
  explicit TypeRegulationsFilter(const RoutingModel& model);
  ~TypeRegulationsFilter() override = default;
  std::string DebugString() const override { return "TypeRegulationsFilter"; }

 private:
  void OnSynchronizePathFromStart(int64_t start) override;
  bool AcceptPath(int64_t path_start, int64_t chain_start,
                  int64_t chain_end) override;

  bool HardIncompatibilitiesRespected(int vehicle, int64_t chain_start,
                                      int64_t chain_end);

  const RoutingModel& routing_model_;
  std::vector<int> start_to_vehicle_;
  // The following vector is used to keep track of the type counts for hard
  // incompatibilities.
  std::vector<std::vector<int>> hard_incompatibility_type_counts_per_vehicle_;
  // Used to verify the temporal incompatibilities and requirements.
  TypeIncompatibilityChecker temporal_incompatibility_checker_;
  TypeRequirementChecker requirement_checker_;
};

TypeRegulationsFilter::TypeRegulationsFilter(const RoutingModel& model)
    : BasePathFilter(model.Nexts(), model.Size() + model.vehicles(),
                     model.GetPathsMetadata()),
      routing_model_(model),
      start_to_vehicle_(model.Size(), -1),
      temporal_incompatibility_checker_(model,
                                        /*check_hard_incompatibilities*/ false),
      requirement_checker_(model) {
  const int num_vehicles = model.vehicles();
  const bool has_hard_type_incompatibilities =
      model.HasHardTypeIncompatibilities();
  if (has_hard_type_incompatibilities) {
    hard_incompatibility_type_counts_per_vehicle_.resize(num_vehicles);
  }
  const int num_visit_types = model.GetNumberOfVisitTypes();
  for (int vehicle = 0; vehicle < num_vehicles; vehicle++) {
    const int64_t start = model.Start(vehicle);
    start_to_vehicle_[start] = vehicle;
    if (has_hard_type_incompatibilities) {
      hard_incompatibility_type_counts_per_vehicle_[vehicle].resize(
          num_visit_types, 0);
    }
  }
}

void TypeRegulationsFilter::OnSynchronizePathFromStart(int64_t start) {
  if (!routing_model_.HasHardTypeIncompatibilities()) return;

  const int vehicle = start_to_vehicle_[start];
  CHECK_GE(vehicle, 0);
  std::vector<int>& type_counts =
      hard_incompatibility_type_counts_per_vehicle_[vehicle];
  std::fill(type_counts.begin(), type_counts.end(), 0);
  const int num_types = type_counts.size();

  int64_t node = start;
  while (node < Size()) {
    DCHECK(IsVarSynced(node));
    const int type = routing_model_.GetVisitType(node);
    if (type >= 0 && routing_model_.GetVisitTypePolicy(node) !=
                         RoutingModel::ADDED_TYPE_REMOVED_FROM_VEHICLE) {
      CHECK_LT(type, num_types);
      type_counts[type]++;
    }
    node = Value(node);
  }
}

bool TypeRegulationsFilter::HardIncompatibilitiesRespected(int vehicle,
                                                           int64_t chain_start,
                                                           int64_t chain_end) {
  if (!routing_model_.HasHardTypeIncompatibilities()) return true;

  const std::vector<int>& previous_type_counts =
      hard_incompatibility_type_counts_per_vehicle_[vehicle];

  absl::flat_hash_map</*type*/ int, /*new_count*/ int> new_type_counts;
  absl::flat_hash_set<int> types_to_check;

  // Go through the new nodes on the path and increment their type counts.
  int64_t node = GetNext(chain_start);
  while (node != chain_end) {
    const int type = routing_model_.GetVisitType(node);
    if (type >= 0 && routing_model_.GetVisitTypePolicy(node) !=
                         RoutingModel::ADDED_TYPE_REMOVED_FROM_VEHICLE) {
      DCHECK_LT(type, previous_type_counts.size());
      int& type_count = gtl::LookupOrInsert(&new_type_counts, type,
                                            previous_type_counts[type]);
      if (type_count++ == 0) {
        // New type on the route, mark to check its incompatibilities.
        types_to_check.insert(type);
      }
    }
    node = GetNext(node);
  }

  // Update new_type_counts by decrementing the occurrence of the types of the
  // nodes no longer on the route.
  if (IsVarSynced(chain_start)) {
    node = Value(chain_start);
    while (node != chain_end) {
      const int type = routing_model_.GetVisitType(node);
      if (type >= 0 && routing_model_.GetVisitTypePolicy(node) !=
                           RoutingModel::ADDED_TYPE_REMOVED_FROM_VEHICLE) {
        DCHECK_LT(type, previous_type_counts.size());
        int& type_count = gtl::LookupOrInsert(&new_type_counts, type,
                                              previous_type_counts[type]);
        CHECK_GE(type_count, 1);
        type_count--;
      }
      node = Value(node);
    }
  }

  // Check the incompatibilities for types in types_to_check.
  for (int type : types_to_check) {
    for (int incompatible_type :
         routing_model_.GetHardTypeIncompatibilitiesOfType(type)) {
      if (gtl::FindWithDefault(new_type_counts, incompatible_type,
                               previous_type_counts[incompatible_type]) > 0) {
        return false;
      }
    }
  }
  return true;
}

bool TypeRegulationsFilter::AcceptPath(int64_t path_start, int64_t chain_start,
                                       int64_t chain_end) {
  const int vehicle = start_to_vehicle_[path_start];
  CHECK_GE(vehicle, 0);
  const auto next_accessor = [this](int64_t node) { return GetNext(node); };
  return HardIncompatibilitiesRespected(vehicle, chain_start, chain_end) &&
         temporal_incompatibility_checker_.CheckVehicle(vehicle,
                                                        next_accessor) &&
         requirement_checker_.CheckVehicle(vehicle, next_accessor);
}

}  // namespace

IntVarLocalSearchFilter* MakeTypeRegulationsFilter(
    const RoutingModel& routing_model) {
  return routing_model.solver()->RevAlloc(
      new TypeRegulationsFilter(routing_model));
}

namespace {

// ChainCumul filter. Version of dimension path filter which is O(delta) rather
// than O(length of touched paths). Currently only supports dimensions without
// costs (global and local span cost, soft bounds) and with unconstrained
// cumul variables except overall capacity and cumul variables of path ends.

class ChainCumulFilter : public BasePathFilter {
 public:
  ChainCumulFilter(const RoutingModel& routing_model,
                   const RoutingDimension& dimension);
  ~ChainCumulFilter() override = default;
  std::string DebugString() const override {
    return "ChainCumulFilter(" + name_ + ")";
  }

 private:
  void OnSynchronizePathFromStart(int64_t start) override;
  bool AcceptPath(int64_t path_start, int64_t chain_start,
                  int64_t chain_end) override;

  const std::vector<IntVar*> cumuls_;
  std::vector<int64_t> start_to_vehicle_;
  std::vector<int64_t> start_to_end_;
  std::vector<const RoutingModel::TransitCallback2*> evaluators_;
  const std::vector<int64_t> vehicle_capacities_;
  std::vector<int64_t> current_path_cumul_mins_;
  std::vector<int64_t> current_max_of_path_end_cumul_mins_;
  std::vector<int64_t> old_nexts_;
  std::vector<int> old_vehicles_;
  std::vector<int64_t> current_transits_;
  const std::string name_;
};

ChainCumulFilter::ChainCumulFilter(const RoutingModel& routing_model,
                                   const RoutingDimension& dimension)
    : BasePathFilter(routing_model.Nexts(), dimension.cumuls().size(),
                     routing_model.GetPathsMetadata()),
      cumuls_(dimension.cumuls()),
      evaluators_(routing_model.vehicles(), nullptr),
      vehicle_capacities_(dimension.vehicle_capacities()),
      current_path_cumul_mins_(dimension.cumuls().size(), 0),
      current_max_of_path_end_cumul_mins_(dimension.cumuls().size(), 0),
      old_nexts_(routing_model.Size(), kUnassigned),
      old_vehicles_(routing_model.Size(), kUnassigned),
      current_transits_(routing_model.Size(), 0),
      name_(dimension.name()) {
  start_to_vehicle_.resize(Size(), -1);
  start_to_end_.resize(Size(), -1);
  for (int i = 0; i < routing_model.vehicles(); ++i) {
    start_to_vehicle_[routing_model.Start(i)] = i;
    start_to_end_[routing_model.Start(i)] = routing_model.End(i);
    evaluators_[i] = &dimension.transit_evaluator(i);
  }
}

// On synchronization, maintain "propagated" cumul mins and max level of cumul
// from each node to the end of the path; to be used by AcceptPath to
// incrementally check feasibility.
void ChainCumulFilter::OnSynchronizePathFromStart(int64_t start) {
  const int vehicle = start_to_vehicle_[start];
  std::vector<int64_t> path_nodes;
  int64_t node = start;
  int64_t cumul = cumuls_[node]->Min();
  while (node < Size()) {
    path_nodes.push_back(node);
    current_path_cumul_mins_[node] = cumul;
    const int64_t next = Value(node);
    if (next != old_nexts_[node] || vehicle != old_vehicles_[node]) {
      old_nexts_[node] = next;
      old_vehicles_[node] = vehicle;
      current_transits_[node] = (*evaluators_[vehicle])(node, next);
    }
    CapAddTo(current_transits_[node], &cumul);
    cumul = std::max(cumuls_[next]->Min(), cumul);
    node = next;
  }
  path_nodes.push_back(node);
  current_path_cumul_mins_[node] = cumul;
  int64_t max_cumuls = cumul;
  for (int i = path_nodes.size() - 1; i >= 0; --i) {
    const int64_t node = path_nodes[i];
    max_cumuls = std::max(max_cumuls, current_path_cumul_mins_[node]);
    current_max_of_path_end_cumul_mins_[node] = max_cumuls;
  }
}

// The complexity of the method is O(size of chain (chain_start...chain_end).
bool ChainCumulFilter::AcceptPath(int64_t path_start, int64_t chain_start,
                                  int64_t chain_end) {
  const int vehicle = start_to_vehicle_[path_start];
  const int64_t capacity = vehicle_capacities_[vehicle];
  int64_t node = chain_start;
  int64_t cumul = current_path_cumul_mins_[node];
  while (node != chain_end) {
    const int64_t next = GetNext(node);
    if (IsVarSynced(node) && next == Value(node) &&
        vehicle == old_vehicles_[node]) {
      CapAddTo(current_transits_[node], &cumul);
    } else {
      CapAddTo((*evaluators_[vehicle])(node, next), &cumul);
    }
    cumul = std::max(cumuls_[next]->Min(), cumul);
    if (cumul > capacity) return false;
    node = next;
  }
  const int64_t end = start_to_end_[path_start];
  const int64_t end_cumul_delta =
      CapSub(current_path_cumul_mins_[end], current_path_cumul_mins_[node]);
  const int64_t after_chain_cumul_delta =
      CapSub(current_max_of_path_end_cumul_mins_[node],
             current_path_cumul_mins_[node]);
  return CapAdd(cumul, after_chain_cumul_delta) <= capacity &&
         CapAdd(cumul, end_cumul_delta) <= cumuls_[end]->Max();
}

// PathCumul filter.

class PathCumulFilter : public BasePathFilter {
 public:
  PathCumulFilter(const RoutingModel& routing_model,
                  const RoutingDimension& dimension,
                  bool propagate_own_objective_value,
                  bool filter_objective_cost, bool may_use_optimizers);
  ~PathCumulFilter() override = default;
  std::string DebugString() const override {
    return "PathCumulFilter(" + name_ + ")";
  }
  int64_t GetSynchronizedObjectiveValue() const override {
    return propagate_own_objective_value_ ? synchronized_objective_value_ : 0;
  }
  int64_t GetAcceptedObjectiveValue() const override {
    return lns_detected() || !propagate_own_objective_value_
               ? 0
               : accepted_objective_value_;
  }
  bool UsesDimensionOptimizers() {
    if (!may_use_optimizers_) return false;
    for (int vehicle = 0; vehicle < routing_model_.vehicles(); ++vehicle) {
      if (FilterWithDimensionCumulOptimizerForVehicle(vehicle)) return true;
    }
    return false;
  }

 private:
  // This structure stores the "best" path cumul value for a solution, the path
  // supporting this value, and the corresponding path cumul values for all
  // paths.
  struct SupportedPathCumul {
    SupportedPathCumul() : cumul_value(0), cumul_value_support(0) {}
    int64_t cumul_value;
    int cumul_value_support;
    std::vector<int64_t> path_values;
  };

  struct SoftBound {
    int64_t bound = -1;
    int64_t coefficient = 0;
  };
  struct InitialInterval {
    int64_t min;
    int64_t max;
  };

  // Data extractors used in constructor.

  std::vector<InitialInterval> ExtractInitialCumulIntervals();
  std::vector<InitialInterval> ExtractInitialSlackIntervals();
  std::vector<std::vector<RoutingDimension::NodePrecedence>>
  ExtractNodeIndexToPrecedences() const;
  std::vector<SoftBound> ExtractCumulSoftUpperBounds() const;
  std::vector<SoftBound> ExtractCumulSoftLowerBounds() const;
  std::vector<const PiecewiseLinearFunction*> ExtractCumulPiecewiseLinearCosts()
      const;
  std::vector<const RoutingModel::TransitCallback2*> ExtractEvaluators() const;

  // This class caches transit values between nodes of paths. Transit and path
  // nodes are to be added in the order in which they appear on a path.
  class PathTransits {
   public:
    void Clear() {
      paths_.clear();
      transits_.clear();
    }
    void ClearPath(int path) {
      paths_[path].clear();
      transits_[path].clear();
    }
    int AddPaths(int num_paths) {
      const int first_path = paths_.size();
      paths_.resize(first_path + num_paths);
      transits_.resize(first_path + num_paths);
      return first_path;
    }
    void ReserveTransits(int path, int number_of_route_arcs) {
      transits_[path].reserve(number_of_route_arcs);
      paths_[path].reserve(number_of_route_arcs + 1);
    }
    // Stores the transit between node and next on path. For a given non-empty
    // path, node must correspond to next in the previous call to PushTransit.
    void PushTransit(int path, int node, int next, int64_t transit) {
      transits_[path].push_back(transit);
      if (paths_[path].empty()) {
        paths_[path].push_back(node);
      }
      DCHECK_EQ(paths_[path].back(), node);
      paths_[path].push_back(next);
    }
    int NumPaths() const { return paths_.size(); }
    int PathSize(int path) const { return paths_[path].size(); }
    int Node(int path, int position) const { return paths_[path][position]; }
    int64_t Transit(int path, int position) const {
      return transits_[path][position];
    }

   private:
    // paths_[r][i] is the ith node on path r.
    std::vector<std::vector<int64_t>> paths_;
    // transits_[r][i] is the transit value between nodes path_[i] and
    // path_[i+1] on path r.
    std::vector<std::vector<int64_t>> transits_;
  };

  bool InitializeAcceptPath() override {
    cumul_cost_delta_ = total_current_cumul_cost_value_;
    node_with_precedence_to_delta_min_max_cumuls_.clear();
    // Cleaning up for the new delta.
    delta_max_end_cumul_ = std::numeric_limits<int64_t>::min();
    delta_paths_.clear();
    delta_path_transits_.Clear();
    delta_nodes_with_precedences_and_changed_cumul_.ClearAll();
    return true;
  }
  bool AcceptPath(int64_t path_start, int64_t chain_start,
                  int64_t chain_end) override;
  bool FinalizeAcceptPath(int64_t objective_min,
                          int64_t objective_max) override;
  void OnBeforeSynchronizePaths() override;

  bool FilterSpanCost() const { return global_span_cost_coefficient_ != 0; }

  bool FilterSlackCost() const {
    return has_nonzero_vehicle_total_slack_cost_coefficients_ ||
           has_vehicle_span_upper_bounds_;
  }

  bool FilterBreakCost(int vehicle) const {
    return dimension_.HasBreakConstraints() &&
           !dimension_.GetBreakIntervalsOfVehicle(vehicle).empty();
  }

  bool FilterCumulSoftBounds() const {
    return !cumul_soft_upper_bounds_.empty();
  }

  int64_t GetCumulSoftCost(int64_t node, int64_t cumul_value) const;

  bool FilterCumulPiecewiseLinearCosts() const {
    return !cumul_piecewise_linear_costs_.empty();
  }

  bool FilterWithDimensionCumulOptimizerForVehicle(int vehicle) const {
    if (!may_use_optimizers_ || FilterCumulPiecewiseLinearCosts()) {
      return false;
    }

    int num_linear_constraints = 0;
    if (dimension_.GetSpanCostCoefficientForVehicle(vehicle) > 0 ||
        dimension_.GetSlackCostCoefficientForVehicle(vehicle) > 0) {
      ++num_linear_constraints;
    }
    if (FilterSoftSpanCost(vehicle)) ++num_linear_constraints;
    if (FilterCumulSoftLowerBounds()) ++num_linear_constraints;
    if (FilterCumulSoftBounds()) ++num_linear_constraints;
    if (vehicle_span_upper_bounds_[vehicle] <
        std::numeric_limits<int64_t>::max()) {
      ++num_linear_constraints;
    }
    const bool has_breaks = FilterBreakCost(vehicle);
    if (has_breaks) ++num_linear_constraints;

    // The DimensionCumulOptimizer is used to compute a more precise value of
    // the cost related to the cumul values (soft bounds and span/slack costs).
    // It is also used to guarantee feasibility with complex mixes of
    // constraints and in particular in the presence of break requests along
    // other constraints. Therefore, without breaks, we only use the optimizer
    // when the costs are actually used to filter the solutions, i.e. when
    // filter_objective_cost_ is true.
    return num_linear_constraints >= 2 &&
           (has_breaks || filter_objective_cost_);
  }

  int64_t GetCumulPiecewiseLinearCost(int64_t node, int64_t cumul_value) const;

  bool FilterCumulSoftLowerBounds() const {
    return !cumul_soft_lower_bounds_.empty();
  }

  bool FilterPrecedences() const { return !node_index_to_precedences_.empty(); }

  bool FilterSoftSpanCost() const {
    return dimension_.HasSoftSpanUpperBounds();
  }
  bool FilterSoftSpanCost(int vehicle) const {
    return dimension_.HasSoftSpanUpperBounds() &&
           dimension_.GetSoftSpanUpperBoundForVehicle(vehicle).cost > 0;
  }
  bool FilterSoftSpanQuadraticCost() const {
    return dimension_.HasQuadraticCostSoftSpanUpperBounds();
  }
  bool FilterSoftSpanQuadraticCost(int vehicle) const {
    return dimension_.HasQuadraticCostSoftSpanUpperBounds() &&
           dimension_.GetQuadraticCostSoftSpanUpperBoundForVehicle(vehicle)
                   .cost > 0;
  }

  int64_t GetCumulSoftLowerBoundCost(int64_t node, int64_t cumul_value) const;

  int64_t GetPathCumulSoftLowerBoundCost(const PathTransits& path_transits,
                                         int path) const;

  void InitializeSupportedPathCumul(SupportedPathCumul* supported_cumul,
                                    int64_t default_value);

  // Given the vector of minimum cumuls on the path, determines if the pickup to
  // delivery limits for this dimension (if there are any) can be respected by
  // this path.
  // Returns true if for every pickup/delivery nodes visited on this path,
  // min_cumul_value(delivery) - max_cumul_value(pickup) is less than the limit
  // set for this pickup to delivery.
  // TODO(user): Verify if we should filter the pickup/delivery limits using
  // the LP, for a perfect filtering.
  bool PickupToDeliveryLimitsRespected(
      const PathTransits& path_transits, int path,
      absl::Span<const int64_t> min_path_cumuls) const;

  // Computes the maximum cumul value of nodes along the path using
  // [current|delta]_path_transits_, and stores the min/max cumul
  // related to each node in the corresponding vector
  // [current|delta]_[min|max]_node_cumuls_.
  // The boolean is_delta indicates if the computations should take place on the
  // "delta" or "current" members. When true, the nodes for which the min/max
  // cumul has changed from the current value are marked in
  // delta_nodes_with_precedences_and_changed_cumul_.
  void StoreMinMaxCumulOfNodesOnPath(int path,
                                     absl::Span<const int64_t> min_path_cumuls,
                                     bool is_delta);

  // Compute the max start cumul value for a given path and a given minimal end
  // cumul value.
  // NOTE: Since this function is used to compute a lower bound on the span of
  // the routes, we don't "jump" over the forbidden intervals with this min end
  // cumul value. We do however concurrently compute the max possible start
  // given the max end cumul, for which we can "jump" over forbidden intervals,
  // and return the minimum of the two.
  int64_t ComputePathMaxStartFromEndCumul(const PathTransits& path_transits,
                                          int path, int64_t path_start,
                                          int64_t min_end_cumul) const;

  const RoutingModel& routing_model_;
  const RoutingDimension& dimension_;
  std::vector<int64_t> start_to_vehicle_;
  const std::vector<InitialInterval> initial_cumul_;
  const std::vector<InitialInterval> initial_slack_;
  const std::vector<const RoutingModel::TransitCallback2*> evaluators_;
  const std::vector<int64_t> vehicle_span_upper_bounds_;
  const bool has_vehicle_span_upper_bounds_;
  int64_t total_current_cumul_cost_value_;
  int64_t synchronized_objective_value_;
  int64_t accepted_objective_value_;
  // Map between paths and path soft cumul bound costs. The paths are indexed
  // by the index of the start node of the path.
  absl::flat_hash_map<int64_t, int64_t> current_cumul_cost_values_;
  int64_t cumul_cost_delta_;
  // Cumul cost values for paths in delta, indexed by vehicle.
  std::vector<int64_t> delta_path_cumul_cost_values_;
  const int64_t global_span_cost_coefficient_;
  const std::vector<SoftBound> cumul_soft_upper_bounds_;
  const std::vector<SoftBound> cumul_soft_lower_bounds_;
  const std::vector<const PiecewiseLinearFunction*>
      cumul_piecewise_linear_costs_;
  std::vector<int64_t> vehicle_total_slack_cost_coefficients_;
  bool has_nonzero_vehicle_total_slack_cost_coefficients_;
  const std::vector<int64_t> vehicle_capacities_;
  // node_index_to_precedences_[node_index] contains all NodePrecedence elements
  // with node_index as either "first_node" or "second_node".
  // This vector is empty if there are no precedences on the dimension_.
  const std::vector<std::vector<RoutingDimension::NodePrecedence>>
      node_index_to_precedences_;
  // Data reflecting information on paths and cumul variables for the solution
  // to which the filter was synchronized.
  SupportedPathCumul current_min_start_;
  SupportedPathCumul current_max_end_;
  PathTransits current_path_transits_;
  // Current min/max cumul values, indexed by node.
  std::vector<std::pair<int64_t, int64_t>> current_min_max_node_cumuls_;
  // Data reflecting information on paths and cumul variables for the "delta"
  // solution (aka neighbor solution) being examined.
  PathTransits delta_path_transits_;
  int64_t delta_max_end_cumul_;
  SparseBitset<int64_t> delta_nodes_with_precedences_and_changed_cumul_;
  absl::flat_hash_map<int64_t, std::pair<int64_t, int64_t>>
      node_with_precedence_to_delta_min_max_cumuls_;
  absl::btree_set<int> delta_paths_;
  const std::string name_;

  LocalDimensionCumulOptimizer* lp_optimizer_;
  LocalDimensionCumulOptimizer* mp_optimizer_;
  const std::function<int64_t(int64_t)> path_accessor_;
  const bool filter_objective_cost_;
  // This boolean indicates if the LP optimizer can be used if necessary to
  // optimize the dimension cumuls.
  const bool may_use_optimizers_;
  const bool propagate_own_objective_value_;

  std::vector<int64_t> min_path_cumuls_;
};

namespace {
template <typename T>
std::vector<T> SumOfVectors(const std::vector<T>& v1,
                            const std::vector<T>& v2) {
  DCHECK_EQ(v1.size(), v2.size());
  std::vector<T> sum(v1.size());
  absl::c_transform(v1, v2, sum.begin(), [](T a, T b) { return CapAdd(a, b); });
  return sum;
}
}  // namespace

std::vector<PathCumulFilter::InitialInterval>
PathCumulFilter::ExtractInitialCumulIntervals() {
  std::vector<InitialInterval> intervals;
  intervals.reserve(dimension_.cumuls().size());
  for (const IntVar* cumul : dimension_.cumuls()) {
    intervals.push_back({cumul->Min(), cumul->Max()});
  }
  return intervals;
}

std::vector<PathCumulFilter::InitialInterval>
PathCumulFilter::ExtractInitialSlackIntervals() {
  std::vector<InitialInterval> intervals;
  intervals.reserve(dimension_.slacks().size());
  for (const IntVar* slack : dimension_.slacks()) {
    intervals.push_back({slack->Min(), slack->Max()});
  }
  return intervals;
}

std::vector<PathCumulFilter::SoftBound>
PathCumulFilter::ExtractCumulSoftUpperBounds() const {
  const int num_cumuls = dimension_.cumuls().size();
  std::vector<SoftBound> bounds(num_cumuls,
                                {.bound = kint64max, .coefficient = 0});
  bool has_some_bound = false;
  for (int i = 0; i < num_cumuls; ++i) {
    if (!dimension_.HasCumulVarSoftUpperBound(i)) continue;
    const int64_t bound = dimension_.GetCumulVarSoftUpperBound(i);
    const int64_t coeff = dimension_.GetCumulVarSoftUpperBoundCoefficient(i);
    bounds[i] = {.bound = bound, .coefficient = coeff};
    has_some_bound |= bound < kint64max && coeff != 0;
  }
  if (!has_some_bound) bounds.clear();
  return bounds;
}

std::vector<PathCumulFilter::SoftBound>
PathCumulFilter::ExtractCumulSoftLowerBounds() const {
  const int num_cumuls = dimension_.cumuls().size();
  std::vector<SoftBound> bounds(num_cumuls, {.bound = 0, .coefficient = 0});
  bool has_some_bound = false;
  for (int i = 0; i < num_cumuls; ++i) {
    if (!dimension_.HasCumulVarSoftLowerBound(i)) continue;
    const int64_t bound = dimension_.GetCumulVarSoftLowerBound(i);
    const int64_t coeff = dimension_.GetCumulVarSoftLowerBoundCoefficient(i);
    bounds[i] = {.bound = bound, .coefficient = coeff};
    has_some_bound |= bound > 0 && coeff != 0;
  }
  if (!has_some_bound) bounds.clear();
  return bounds;
}

std::vector<const PiecewiseLinearFunction*>
PathCumulFilter::ExtractCumulPiecewiseLinearCosts() const {
  const int num_cumuls = dimension_.cumuls().size();
  std::vector<const PiecewiseLinearFunction*> costs(num_cumuls, nullptr);
  bool has_some_cost = false;
  for (int i = 0; i < dimension_.cumuls().size(); ++i) {
    if (!dimension_.HasCumulVarPiecewiseLinearCost(i)) continue;
    const PiecewiseLinearFunction* const cost =
        dimension_.GetCumulVarPiecewiseLinearCost(i);
    if (cost == nullptr) continue;
    has_some_cost = true;
    costs[i] = cost;
  }
  if (!has_some_cost) costs.clear();
  return costs;
}

std::vector<const RoutingModel::TransitCallback2*>
PathCumulFilter::ExtractEvaluators() const {
  const int num_paths = NumPaths();
  std::vector<const RoutingModel::TransitCallback2*> evaluators(num_paths);
  for (int i = 0; i < num_paths; ++i) {
    evaluators[i] = &dimension_.transit_evaluator(i);
  }
  return evaluators;
}

std::vector<std::vector<RoutingDimension::NodePrecedence>>
PathCumulFilter::ExtractNodeIndexToPrecedences() const {
  std::vector<std::vector<RoutingDimension::NodePrecedence>>
      node_index_to_precedences;
  const std::vector<RoutingDimension::NodePrecedence>& node_precedences =
      dimension_.GetNodePrecedences();
  if (!node_precedences.empty()) {
    node_index_to_precedences.resize(dimension_.cumuls().size());
    for (const auto& node_precedence : node_precedences) {
      node_index_to_precedences[node_precedence.first_node].push_back(
          node_precedence);
      node_index_to_precedences[node_precedence.second_node].push_back(
          node_precedence);
    }
  }
  return node_index_to_precedences;
}

PathCumulFilter::PathCumulFilter(const RoutingModel& routing_model,
                                 const RoutingDimension& dimension,
                                 bool propagate_own_objective_value,
                                 bool filter_objective_cost,
                                 bool may_use_optimizers)
    : BasePathFilter(routing_model.Nexts(), dimension.cumuls().size(),
                     routing_model.GetPathsMetadata()),
      routing_model_(routing_model),
      dimension_(dimension),
      initial_cumul_(ExtractInitialCumulIntervals()),
      initial_slack_(ExtractInitialSlackIntervals()),
      evaluators_(ExtractEvaluators()),
      vehicle_span_upper_bounds_(dimension.vehicle_span_upper_bounds()),
      has_vehicle_span_upper_bounds_(absl::c_any_of(
          vehicle_span_upper_bounds_,
          [](int64_t upper_bound) {
            return upper_bound != std::numeric_limits<int64_t>::max();
          })),
      total_current_cumul_cost_value_(0),
      synchronized_objective_value_(0),
      accepted_objective_value_(0),
      current_cumul_cost_values_(),
      cumul_cost_delta_(0),
      delta_path_cumul_cost_values_(routing_model.vehicles(),
                                    std::numeric_limits<int64_t>::min()),
      global_span_cost_coefficient_(dimension.global_span_cost_coefficient()),
      cumul_soft_upper_bounds_(ExtractCumulSoftUpperBounds()),
      cumul_soft_lower_bounds_(ExtractCumulSoftLowerBounds()),
      cumul_piecewise_linear_costs_(ExtractCumulPiecewiseLinearCosts()),
      vehicle_total_slack_cost_coefficients_(
          SumOfVectors(dimension.vehicle_span_cost_coefficients(),
                       dimension.vehicle_slack_cost_coefficients())),
      has_nonzero_vehicle_total_slack_cost_coefficients_(
          absl::c_any_of(vehicle_total_slack_cost_coefficients_,
                         [](int64_t coefficient) { return coefficient != 0; })),
      vehicle_capacities_(dimension.vehicle_capacities()),
      node_index_to_precedences_(ExtractNodeIndexToPrecedences()),
      delta_max_end_cumul_(0),
      delta_nodes_with_precedences_and_changed_cumul_(routing_model.Size()),
      name_(dimension.name()),
      lp_optimizer_(routing_model.GetMutableLocalCumulLPOptimizer(dimension)),
      mp_optimizer_(routing_model.GetMutableLocalCumulMPOptimizer(dimension)),
      path_accessor_([this](int64_t node) { return GetNext(node); }),
      filter_objective_cost_(filter_objective_cost),
      may_use_optimizers_(may_use_optimizers),
      propagate_own_objective_value_(propagate_own_objective_value) {
  bool has_cumul_hard_bounds = false;
  for (const InitialInterval& slack : initial_slack_) {
    if (slack.min > 0) {
      has_cumul_hard_bounds = true;
      break;
    }
  }
  for (const InitialInterval& cumul : initial_cumul_) {
    if (cumul.min > 0 || cumul.max < kint64max) {
      has_cumul_hard_bounds = true;
      break;
    }
  }
  if (!has_cumul_hard_bounds) {
    // Slacks don't need to be constrained if the cumuls don't have hard bounds;
    // therefore we can ignore the vehicle span/slack cost coefficient (note
    // that the transit part is already handled by the arc cost filters). This
    // doesn't concern the global span filter though.
    vehicle_total_slack_cost_coefficients_.assign(routing_model.vehicles(), 0);
    has_nonzero_vehicle_total_slack_cost_coefficients_ = false;
  }
  start_to_vehicle_.resize(Size(), -1);
  for (int i = 0; i < routing_model.vehicles(); ++i) {
    start_to_vehicle_[routing_model.Start(i)] = i;
  }

  const std::vector<RoutingDimension::NodePrecedence>& node_precedences =
      dimension.GetNodePrecedences();
  if (!node_precedences.empty()) {
    current_min_max_node_cumuls_.resize(initial_cumul_.size(), {-1, -1});
  }

#ifndef NDEBUG
  for (int vehicle = 0; vehicle < routing_model.vehicles(); vehicle++) {
    if (FilterWithDimensionCumulOptimizerForVehicle(vehicle)) {
      DCHECK_NE(lp_optimizer_, nullptr);
      DCHECK_NE(mp_optimizer_, nullptr);
    }
  }
#endif  // NDEBUG
}

int64_t PathCumulFilter::GetCumulSoftCost(int64_t node,
                                          int64_t cumul_value) const {
  if (node < cumul_soft_upper_bounds_.size()) {
    const int64_t bound = cumul_soft_upper_bounds_[node].bound;
    const int64_t coefficient = cumul_soft_upper_bounds_[node].coefficient;
    if (coefficient > 0 && bound < cumul_value) {
      return CapProd(CapSub(cumul_value, bound), coefficient);
    }
  }
  return 0;
}

int64_t PathCumulFilter::GetCumulPiecewiseLinearCost(
    int64_t node, int64_t cumul_value) const {
  if (node < cumul_piecewise_linear_costs_.size()) {
    const PiecewiseLinearFunction* cost = cumul_piecewise_linear_costs_[node];
    if (cost != nullptr) {
      return cost->Value(cumul_value);
    }
  }
  return 0;
}

int64_t PathCumulFilter::GetCumulSoftLowerBoundCost(int64_t node,
                                                    int64_t cumul_value) const {
  if (node < cumul_soft_lower_bounds_.size()) {
    const int64_t bound = cumul_soft_lower_bounds_[node].bound;
    const int64_t coefficient = cumul_soft_lower_bounds_[node].coefficient;
    if (coefficient > 0 && bound > cumul_value) {
      return CapProd(CapSub(bound, cumul_value), coefficient);
    }
  }
  return 0;
}

int64_t PathCumulFilter::GetPathCumulSoftLowerBoundCost(
    const PathTransits& path_transits, int path) const {
  int64_t node = path_transits.Node(path, path_transits.PathSize(path) - 1);
  int64_t cumul = initial_cumul_[node].max;
  int64_t current_cumul_cost_value = GetCumulSoftLowerBoundCost(node, cumul);
  for (int i = path_transits.PathSize(path) - 2; i >= 0; --i) {
    node = path_transits.Node(path, i);
    cumul = CapSub(cumul, path_transits.Transit(path, i));
    cumul = std::min(initial_cumul_[node].max, cumul);
    CapAddTo(GetCumulSoftLowerBoundCost(node, cumul),
             &current_cumul_cost_value);
  }
  return current_cumul_cost_value;
}

void PathCumulFilter::OnBeforeSynchronizePaths() {
  total_current_cumul_cost_value_ = 0;
  cumul_cost_delta_ = 0;
  current_cumul_cost_values_.clear();
  if (HasAnySyncedPath() &&
      (FilterSpanCost() || FilterCumulSoftBounds() || FilterSlackCost() ||
       FilterCumulSoftLowerBounds() || FilterCumulPiecewiseLinearCosts() ||
       FilterPrecedences() || FilterSoftSpanCost() ||
       FilterSoftSpanQuadraticCost())) {
    InitializeSupportedPathCumul(&current_min_start_,
                                 std::numeric_limits<int64_t>::max());
    InitializeSupportedPathCumul(&current_max_end_,
                                 std::numeric_limits<int64_t>::min());
    current_path_transits_.Clear();
    current_path_transits_.AddPaths(NumPaths());
    // For each path, compute the minimum end cumul and store the max of these.
    for (int r = 0; r < NumPaths(); ++r) {
      int64_t node = Start(r);
      if (!IsVarSynced(node)) {
        continue;
      }
      const int vehicle = start_to_vehicle_[Start(r)];
      // First pass: evaluating route length to reserve memory to store route
      // information.
      int number_of_route_arcs = 0;
      while (node < Size()) {
        ++number_of_route_arcs;
        node = Value(node);
      }
      current_path_transits_.ReserveTransits(r, number_of_route_arcs);
      // Second pass: update cumul, transit and cost values.
      node = Start(r);
      int64_t cumul = initial_cumul_[node].min;
      min_path_cumuls_.clear();
      min_path_cumuls_.push_back(cumul);

      int64_t current_cumul_cost_value = GetCumulSoftCost(node, cumul);
      CapAddTo(GetCumulPiecewiseLinearCost(node, cumul),
               &current_cumul_cost_value);

      int64_t total_transit = 0;
      while (node < Size()) {
        const int64_t next = Value(node);
        const int64_t transit = (*evaluators_[vehicle])(node, next);
        CapAddTo(transit, &total_transit);
        const int64_t transit_slack = CapAdd(transit, initial_slack_[node].min);
        current_path_transits_.PushTransit(r, node, next, transit_slack);
        CapAddTo(transit_slack, &cumul);
        cumul =
            dimension_.GetFirstPossibleGreaterOrEqualValueForNode(next, cumul);
        cumul = std::max(initial_cumul_[next].min, cumul);
        min_path_cumuls_.push_back(cumul);
        node = next;
        CapAddTo(GetCumulSoftCost(node, cumul), &current_cumul_cost_value);
        CapAddTo(GetCumulPiecewiseLinearCost(node, cumul),
                 &current_cumul_cost_value);
      }
      if (FilterPrecedences()) {
        StoreMinMaxCumulOfNodesOnPath(/*path=*/r, min_path_cumuls_,
                                      /*is_delta=*/false);
      }
      if (number_of_route_arcs == 1 &&
          !routing_model_.IsVehicleUsedWhenEmpty(vehicle)) {
        // This is an empty route (single start->end arc) which we don't take
        // into account for costs.
        current_cumul_cost_values_[Start(r)] = 0;
        current_path_transits_.ClearPath(r);
        continue;
      }
      if (FilterSlackCost() || FilterSoftSpanCost() ||
          FilterSoftSpanQuadraticCost()) {
        const int64_t start = ComputePathMaxStartFromEndCumul(
            current_path_transits_, r, Start(r), cumul);
        const int64_t span_lower_bound = CapSub(cumul, start);
        if (FilterSlackCost()) {
          CapAddTo(CapProd(vehicle_total_slack_cost_coefficients_[vehicle],
                           CapSub(span_lower_bound, total_transit)),
                   &current_cumul_cost_value);
        }
        if (FilterSoftSpanCost()) {
          const BoundCost bound_cost =
              dimension_.GetSoftSpanUpperBoundForVehicle(vehicle);
          if (bound_cost.bound < span_lower_bound) {
            const int64_t violation =
                CapSub(span_lower_bound, bound_cost.bound);
            CapAddTo(CapProd(bound_cost.cost, violation),
                     &current_cumul_cost_value);
          }
        }
        if (FilterSoftSpanQuadraticCost()) {
          const BoundCost bound_cost =
              dimension_.GetQuadraticCostSoftSpanUpperBoundForVehicle(vehicle);
          if (bound_cost.bound < span_lower_bound) {
            const int64_t violation =
                CapSub(span_lower_bound, bound_cost.bound);
            CapAddTo(CapProd(bound_cost.cost, CapProd(violation, violation)),
                     &current_cumul_cost_value);
          }
        }
      }
      if (FilterCumulSoftLowerBounds()) {
        CapAddTo(GetPathCumulSoftLowerBoundCost(current_path_transits_, r),
                 &current_cumul_cost_value);
      }
      if (FilterWithDimensionCumulOptimizerForVehicle(vehicle)) {
        // TODO(user): Return a status from the optimizer to detect failures
        // The only admissible failures here are because of LP timeout.
        int64_t lp_cumul_cost_value = 0;
        LocalDimensionCumulOptimizer* const optimizer =
            (FilterSoftSpanQuadraticCost(vehicle) || FilterBreakCost(vehicle))
                ? mp_optimizer_
                : lp_optimizer_;
        DCHECK(optimizer != nullptr);
        const DimensionSchedulingStatus status =
            optimizer->ComputeRouteCumulCostWithoutFixedTransits(
                vehicle, path_accessor_, /*resource=*/nullptr,
                filter_objective_cost_ ? &lp_cumul_cost_value : nullptr);
        switch (status) {
          case DimensionSchedulingStatus::INFEASIBLE:
            lp_cumul_cost_value = 0;
            break;
          case DimensionSchedulingStatus::RELAXED_OPTIMAL_ONLY:
            DCHECK(mp_optimizer_ != nullptr);
            if (mp_optimizer_->ComputeRouteCumulCostWithoutFixedTransits(
                    vehicle, path_accessor_, /*resource=*/nullptr,
                    filter_objective_cost_ ? &lp_cumul_cost_value : nullptr) ==
                DimensionSchedulingStatus::INFEASIBLE) {
              lp_cumul_cost_value = 0;
            }
            break;
          default:
            DCHECK(status == DimensionSchedulingStatus::OPTIMAL);
        }
        current_cumul_cost_value =
            std::max(current_cumul_cost_value, lp_cumul_cost_value);
      }
      current_cumul_cost_values_[Start(r)] = current_cumul_cost_value;
      current_max_end_.path_values[r] = cumul;
      if (current_max_end_.cumul_value < cumul) {
        current_max_end_.cumul_value = cumul;
        current_max_end_.cumul_value_support = r;
      }
      CapAddTo(current_cumul_cost_value, &total_current_cumul_cost_value_);
    }
    if (FilterPrecedences()) {
      // Update the min/max node cumuls of new unperformed nodes.
      for (int64_t node : GetNewSynchronizedUnperformedNodes()) {
        current_min_max_node_cumuls_[node] = {-1, -1};
      }
    }
    // Use the max of the path end cumul mins to compute the corresponding
    // maximum start cumul of each path; store the minimum of these.
    for (int r = 0; r < NumPaths(); ++r) {
      if (!IsVarSynced(Start(r))) {
        continue;
      }
      const int64_t start = ComputePathMaxStartFromEndCumul(
          current_path_transits_, r, Start(r), current_max_end_.cumul_value);
      current_min_start_.path_values[r] = start;
      if (current_min_start_.cumul_value > start) {
        current_min_start_.cumul_value = start;
        current_min_start_.cumul_value_support = r;
      }
    }
  }
  // Initialize this before considering any deltas (neighbor).
  delta_max_end_cumul_ = std::numeric_limits<int64_t>::min();

  DCHECK(global_span_cost_coefficient_ == 0 ||
         current_min_start_.cumul_value <= current_max_end_.cumul_value);
  synchronized_objective_value_ =
      CapAdd(total_current_cumul_cost_value_,
             CapProd(global_span_cost_coefficient_,
                     CapSub(current_max_end_.cumul_value,
                            current_min_start_.cumul_value)));
}

bool PathCumulFilter::AcceptPath(int64_t path_start, int64_t /*chain_start*/,
                                 int64_t /*chain_end*/) {
  int64_t node = path_start;
  int64_t cumul = initial_cumul_[node].min;
  int64_t cumul_cost_delta = 0;
  int64_t total_transit = 0;
  const int path = delta_path_transits_.AddPaths(1);
  const int vehicle = start_to_vehicle_[path_start];
  const int64_t capacity = vehicle_capacities_[vehicle];
  const bool filter_vehicle_costs =
      !routing_model_.IsEnd(GetNext(node)) ||
      routing_model_.IsVehicleUsedWhenEmpty(vehicle);
  if (filter_vehicle_costs) {
    cumul_cost_delta = CapAdd(GetCumulSoftCost(node, cumul),
                              GetCumulPiecewiseLinearCost(node, cumul));
  }
  // Evaluating route length to reserve memory to store transit information.
  int number_of_route_arcs = 0;
  while (node < Size()) {
    ++number_of_route_arcs;
    node = GetNext(node);
    DCHECK_NE(node, kUnassigned);
  }
  delta_path_transits_.ReserveTransits(path, number_of_route_arcs);
  min_path_cumuls_.clear();
  min_path_cumuls_.push_back(cumul);
  // Check that the path is feasible with regards to cumul bounds, scanning
  // the paths from start to end (caching path node sequences and transits
  // for further span cost filtering).
  node = path_start;
  while (node < Size()) {
    const int64_t next = GetNext(node);
    const int64_t transit = (*evaluators_[vehicle])(node, next);
    CapAddTo(transit, &total_transit);
    const int64_t transit_slack = CapAdd(transit, initial_slack_[node].min);
    delta_path_transits_.PushTransit(path, node, next, transit_slack);
    CapAddTo(transit_slack, &cumul);
    cumul = dimension_.GetFirstPossibleGreaterOrEqualValueForNode(next, cumul);
    if (cumul > std::min(capacity, initial_cumul_[next].max)) {
      return false;
    }
    cumul = std::max(initial_cumul_[next].min, cumul);
    min_path_cumuls_.push_back(cumul);
    node = next;
    if (filter_vehicle_costs) {
      CapAddTo(GetCumulSoftCost(node, cumul), &cumul_cost_delta);
      CapAddTo(GetCumulPiecewiseLinearCost(node, cumul), &cumul_cost_delta);
    }
  }
  const int64_t min_end = cumul;

  if (!PickupToDeliveryLimitsRespected(delta_path_transits_, path,
                                       min_path_cumuls_)) {
    return false;
  }
  if (FilterSlackCost() || FilterBreakCost(vehicle) ||
      FilterSoftSpanCost(vehicle) || FilterSoftSpanQuadraticCost(vehicle)) {
    int64_t slack_max = std::numeric_limits<int64_t>::max();
    if (vehicle_span_upper_bounds_[vehicle] <
        std::numeric_limits<int64_t>::max()) {
      const int64_t span_max = vehicle_span_upper_bounds_[vehicle];
      slack_max = std::min(slack_max, CapSub(span_max, total_transit));
    }
    const int64_t max_start_from_min_end = ComputePathMaxStartFromEndCumul(
        delta_path_transits_, path, path_start, min_end);
    const int64_t span_lb = CapSub(min_end, max_start_from_min_end);
    int64_t min_total_slack = CapSub(span_lb, total_transit);
    if (min_total_slack > slack_max) return false;

    if (dimension_.HasBreakConstraints()) {
      for (const auto [limit, min_break_duration] :
           dimension_.GetBreakDistanceDurationOfVehicle(vehicle)) {
        // Minimal number of breaks depends on total transit:
        // 0 breaks for 0 <= total transit <= limit,
        // 1 break for limit + 1 <= total transit <= 2 * limit,
        // i breaks for i * limit + 1 <= total transit <= (i+1) * limit, ...
        if (limit == 0 || total_transit == 0) continue;
        const int num_breaks_lb = (total_transit - 1) / limit;
        const int64_t slack_lb = CapProd(num_breaks_lb, min_break_duration);
        if (slack_lb > slack_max) return false;
        min_total_slack = std::max(min_total_slack, slack_lb);
      }
      // Compute a lower bound of the amount of break that must be made inside
      // the route. We compute a mandatory interval (might be empty)
      // [max_start, min_end[ during which the route will have to happen,
      // then the duration of break that must happen during this interval.
      int64_t min_total_break = 0;
      int64_t max_path_end = initial_cumul_[routing_model_.End(vehicle)].max;
      const int64_t max_start = ComputePathMaxStartFromEndCumul(
          delta_path_transits_, path, path_start, max_path_end);
      for (const IntervalVar* br :
           dimension_.GetBreakIntervalsOfVehicle(vehicle)) {
        if (!br->MustBePerformed()) continue;
        if (max_start < br->EndMin() && br->StartMax() < min_end) {
          CapAddTo(br->DurationMin(), &min_total_break);
        }
      }
      if (min_total_break > slack_max) return false;
      min_total_slack = std::max(min_total_slack, min_total_break);
    }
    if (filter_vehicle_costs) {
      CapAddTo(CapProd(vehicle_total_slack_cost_coefficients_[vehicle],
                       min_total_slack),
               &cumul_cost_delta);
      const int64_t span_lower_bound = CapAdd(total_transit, min_total_slack);
      if (FilterSoftSpanCost()) {
        const BoundCost bound_cost =
            dimension_.GetSoftSpanUpperBoundForVehicle(vehicle);
        if (bound_cost.bound < span_lower_bound) {
          const int64_t violation = CapSub(span_lower_bound, bound_cost.bound);
          CapAddTo(CapProd(bound_cost.cost, violation), &cumul_cost_delta);
        }
      }
      if (FilterSoftSpanQuadraticCost()) {
        const BoundCost bound_cost =
            dimension_.GetQuadraticCostSoftSpanUpperBoundForVehicle(vehicle);
        if (bound_cost.bound < span_lower_bound) {
          const int64_t violation = CapSub(span_lower_bound, bound_cost.bound);
          CapAddTo(CapProd(bound_cost.cost, CapProd(violation, violation)),
                   &cumul_cost_delta);
        }
      }
    }
    if (CapAdd(total_transit, min_total_slack) >
        vehicle_span_upper_bounds_[vehicle]) {
      return false;
    }
  }
  if (FilterCumulSoftLowerBounds() && filter_vehicle_costs) {
    CapAddTo(GetPathCumulSoftLowerBoundCost(delta_path_transits_, path),
             &cumul_cost_delta);
  }
  if (FilterPrecedences()) {
    StoreMinMaxCumulOfNodesOnPath(path, min_path_cumuls_, /*is_delta=*/true);
  }
  if (!filter_vehicle_costs) {
    // If this route's costs shouldn't be taken into account, reset the
    // cumul_cost_delta and delta_path_transits_ for this path.
    cumul_cost_delta = 0;
    delta_path_transits_.ClearPath(path);
  }
  if (FilterSpanCost() || FilterCumulSoftBounds() || FilterSlackCost() ||
      FilterCumulSoftLowerBounds() || FilterCumulPiecewiseLinearCosts() ||
      FilterSoftSpanCost(vehicle) || FilterSoftSpanQuadraticCost(vehicle)) {
    delta_paths_.insert(GetPath(path_start));
    delta_path_cumul_cost_values_[vehicle] = cumul_cost_delta;
    cumul_cost_delta =
        CapSub(cumul_cost_delta, current_cumul_cost_values_[path_start]);
    if (filter_vehicle_costs) {
      delta_max_end_cumul_ = std::max(delta_max_end_cumul_, min_end);
    }
  }
  CapAddTo(cumul_cost_delta, &cumul_cost_delta_);
  return true;
}

bool PathCumulFilter::FinalizeAcceptPath(int64_t /*objective_min*/,
                                         int64_t objective_max) {
  DCHECK(!lns_detected());
  if (!FilterSpanCost() && !FilterCumulSoftBounds() && !FilterSlackCost() &&
      !FilterCumulSoftLowerBounds() && !FilterCumulPiecewiseLinearCosts() &&
      !FilterPrecedences() && !FilterSoftSpanCost() &&
      !FilterSoftSpanQuadraticCost()) {
    return true;
  }
  if (FilterPrecedences()) {
    for (int64_t node : delta_nodes_with_precedences_and_changed_cumul_
                            .PositionsSetAtLeastOnce()) {
      const std::pair<int64_t, int64_t> node_min_max_cumul_in_delta =
          gtl::FindWithDefault(node_with_precedence_to_delta_min_max_cumuls_,
                               node, {-1, -1});
      // NOTE: This node was seen in delta, so its delta min/max cumul should be
      // stored in the map.
      DCHECK(node_min_max_cumul_in_delta.first >= 0 &&
             node_min_max_cumul_in_delta.second >= 0);
      for (const RoutingDimension::NodePrecedence& precedence :
           node_index_to_precedences_[node]) {
        const bool node_is_first = (precedence.first_node == node);
        const int64_t other_node =
            node_is_first ? precedence.second_node : precedence.first_node;
        if (GetNext(other_node) == kUnassigned ||
            GetNext(other_node) == other_node) {
          // The other node is unperformed, so the precedence constraint is
          // inactive.
          continue;
        }
        // max_cumul[second_node] should be greater or equal than
        // min_cumul[first_node] + offset.
        const std::pair<int64_t, int64_t>& other_min_max_cumul_in_delta =
            gtl::FindWithDefault(node_with_precedence_to_delta_min_max_cumuls_,
                                 other_node,
                                 current_min_max_node_cumuls_[other_node]);

        const int64_t first_min_cumul =
            node_is_first ? node_min_max_cumul_in_delta.first
                          : other_min_max_cumul_in_delta.first;
        const int64_t second_max_cumul =
            node_is_first ? other_min_max_cumul_in_delta.second
                          : node_min_max_cumul_in_delta.second;

        if (second_max_cumul < first_min_cumul + precedence.offset) {
          return false;
        }
      }
    }
  }
  int64_t new_max_end = delta_max_end_cumul_;
  int64_t new_min_start = std::numeric_limits<int64_t>::max();
  if (FilterSpanCost()) {
    if (new_max_end < current_max_end_.cumul_value) {
      // Delta max end is lower than the current solution one.
      // If the path supporting the current max end has been modified, we need
      // to check all paths to find the largest max end.
      if (!delta_paths_.contains(current_max_end_.cumul_value_support)) {
        new_max_end = current_max_end_.cumul_value;
      } else {
        for (int i = 0; i < current_max_end_.path_values.size(); ++i) {
          if (current_max_end_.path_values[i] > new_max_end &&
              !delta_paths_.contains(i)) {
            new_max_end = current_max_end_.path_values[i];
          }
        }
      }
    }
    // Now that the max end cumul has been found, compute the corresponding
    // min start cumul, first from the delta, then if the max end cumul has
    // changed, from the unchanged paths as well.
    for (int r = 0; r < delta_path_transits_.NumPaths(); ++r) {
      new_min_start =
          std::min(ComputePathMaxStartFromEndCumul(delta_path_transits_, r,
                                                   Start(r), new_max_end),
                   new_min_start);
    }
    if (new_max_end != current_max_end_.cumul_value) {
      for (int r = 0; r < NumPaths(); ++r) {
        if (delta_paths_.contains(r)) {
          continue;
        }
        new_min_start = std::min(new_min_start, ComputePathMaxStartFromEndCumul(
                                                    current_path_transits_, r,
                                                    Start(r), new_max_end));
      }
    } else if (new_min_start > current_min_start_.cumul_value) {
      // Delta min start is greater than the current solution one.
      // If the path supporting the current min start has been modified, we need
      // to check all paths to find the smallest min start.
      if (!delta_paths_.contains(current_min_start_.cumul_value_support)) {
        new_min_start = current_min_start_.cumul_value;
      } else {
        for (int i = 0; i < current_min_start_.path_values.size(); ++i) {
          if (current_min_start_.path_values[i] < new_min_start &&
              !delta_paths_.contains(i)) {
            new_min_start = current_min_start_.path_values[i];
          }
        }
      }
    }
  }

  // Filtering on objective value, calling LPs and MIPs if needed..
  accepted_objective_value_ =
      CapAdd(cumul_cost_delta_, CapProd(global_span_cost_coefficient_,
                                        CapSub(new_max_end, new_min_start)));

  if (may_use_optimizers_ && lp_optimizer_ != nullptr &&
      accepted_objective_value_ <= objective_max) {
    const size_t num_touched_paths = GetTouchedPathStarts().size();
    std::vector<int64_t> path_delta_cost_values(num_touched_paths, 0);
    std::vector<bool> requires_mp(num_touched_paths, false);
    for (int i = 0; i < num_touched_paths; ++i) {
      const int64_t start = GetTouchedPathStarts()[i];
      const int vehicle = start_to_vehicle_[start];
      if (!FilterWithDimensionCumulOptimizerForVehicle(vehicle)) {
        continue;
      }
      int64_t path_delta_cost_with_lp = 0;
      const DimensionSchedulingStatus status =
          lp_optimizer_->ComputeRouteCumulCostWithoutFixedTransits(
              vehicle, path_accessor_, /*resource=*/nullptr,
              filter_objective_cost_ ? &path_delta_cost_with_lp : nullptr);
      if (status == DimensionSchedulingStatus::INFEASIBLE) {
        return false;
      }
      DCHECK(delta_paths_.contains(GetPath(start)));
      const int64_t path_cost_diff_with_lp = CapSub(
          path_delta_cost_with_lp, delta_path_cumul_cost_values_[vehicle]);
      if (path_cost_diff_with_lp > 0) {
        path_delta_cost_values[i] = path_delta_cost_with_lp;
        CapAddTo(path_cost_diff_with_lp, &accepted_objective_value_);
        if (accepted_objective_value_ > objective_max) {
          return false;
        }
      } else {
        path_delta_cost_values[i] = delta_path_cumul_cost_values_[vehicle];
      }
      DCHECK_NE(mp_optimizer_, nullptr);
      requires_mp[i] =
          FilterBreakCost(vehicle) || FilterSoftSpanQuadraticCost(vehicle) ||
          (status == DimensionSchedulingStatus::RELAXED_OPTIMAL_ONLY);
    }

    DCHECK_LE(accepted_objective_value_, objective_max);

    for (int i = 0; i < num_touched_paths; ++i) {
      if (!requires_mp[i]) {
        continue;
      }
      const int64_t start = GetTouchedPathStarts()[i];
      const int vehicle = start_to_vehicle_[start];
      int64_t path_delta_cost_with_mp = 0;
      if (mp_optimizer_->ComputeRouteCumulCostWithoutFixedTransits(
              vehicle, path_accessor_, /*resource=*/nullptr,
              filter_objective_cost_ ? &path_delta_cost_with_mp : nullptr) ==
          DimensionSchedulingStatus::INFEASIBLE) {
        return false;
      }
      DCHECK(delta_paths_.contains(GetPath(start)));
      const int64_t path_cost_diff_with_mp =
          CapSub(path_delta_cost_with_mp, path_delta_cost_values[i]);
      if (path_cost_diff_with_mp > 0) {
        CapAddTo(path_cost_diff_with_mp, &accepted_objective_value_);
        if (accepted_objective_value_ > objective_max) {
          return false;
        }
      }
    }
  }

  return accepted_objective_value_ <= objective_max;
}

void PathCumulFilter::InitializeSupportedPathCumul(
    SupportedPathCumul* supported_cumul, int64_t default_value) {
  supported_cumul->cumul_value = default_value;
  supported_cumul->cumul_value_support = -1;
  supported_cumul->path_values.resize(NumPaths(), default_value);
}

bool PathCumulFilter::PickupToDeliveryLimitsRespected(
    const PathTransits& path_transits, int path,
    absl::Span<const int64_t> min_path_cumuls) const {
  if (!dimension_.HasPickupToDeliveryLimits()) {
    return true;
  }
  const int num_pairs = routing_model_.GetPickupAndDeliveryPairs().size();
  DCHECK_GT(num_pairs, 0);
  std::vector<std::pair<int, int64_t>> visited_delivery_and_min_cumul_per_pair(
      num_pairs, {-1, -1});

  const int path_size = path_transits.PathSize(path);
  CHECK_EQ(min_path_cumuls.size(), path_size);

  int64_t max_cumul = min_path_cumuls.back();
  for (int i = path_transits.PathSize(path) - 2; i >= 0; i--) {
    const int node_index = path_transits.Node(path, i);
    max_cumul = CapSub(max_cumul, path_transits.Transit(path, i));
    max_cumul = std::min(initial_cumul_[node_index].max, max_cumul);

    using PDPosition = RoutingModel::PickupDeliveryPosition;
    if (routing_model_.IsPickup(node_index)) {
      const std::optional<PDPosition> pickup_position =
          routing_model_.GetPickupPosition(node_index);
      DCHECK(pickup_position.has_value());
      const auto [pair_index, pickup_alternative_index] = *pickup_position;
      // Get the delivery visited for this pair.
      const int delivery_alternative_index =
          visited_delivery_and_min_cumul_per_pair[pair_index].first;
      if (delivery_alternative_index < 0) {
        // No delivery visited after this pickup for this pickup/delivery pair.
        continue;
      }
      const int64_t cumul_diff_limit =
          dimension_.GetPickupToDeliveryLimitForPair(
              pair_index, pickup_alternative_index, delivery_alternative_index);
      if (CapSub(visited_delivery_and_min_cumul_per_pair[pair_index].second,
                 max_cumul) > cumul_diff_limit) {
        return false;
      }
    } else if (routing_model_.IsDelivery(node_index)) {
      const std::optional<PDPosition> delivery_position =
          routing_model_.GetDeliveryPosition(node_index);
      DCHECK(delivery_position.has_value());
      const auto [pair_index, delivery_alternative_index] = *delivery_position;
      std::pair<int, int64_t>& delivery_index_and_cumul =
          visited_delivery_and_min_cumul_per_pair[pair_index];
      int& delivery_index = delivery_index_and_cumul.first;
      DCHECK_EQ(delivery_index, -1);
      delivery_index = delivery_alternative_index;
      delivery_index_and_cumul.second = min_path_cumuls[i];
    }
  }
  return true;
}

void PathCumulFilter::StoreMinMaxCumulOfNodesOnPath(
    int path, absl::Span<const int64_t> min_path_cumuls, bool is_delta) {
  const PathTransits& path_transits =
      is_delta ? delta_path_transits_ : current_path_transits_;

  const int path_size = path_transits.PathSize(path);
  DCHECK_EQ(min_path_cumuls.size(), path_size);

  int64_t max_cumul =
      initial_cumul_[path_transits.Node(path, path_size - 1)].max;
  for (int i = path_size - 1; i >= 0; i--) {
    const int node_index = path_transits.Node(path, i);

    if (i < path_size - 1) {
      max_cumul = CapSub(max_cumul, path_transits.Transit(path, i));
      max_cumul = std::min(initial_cumul_[node_index].max, max_cumul);
    }

    if (is_delta && node_index_to_precedences_[node_index].empty()) {
      // No need to update the delta cumul map for nodes without precedences.
      continue;
    }

    std::pair<int64_t, int64_t>& min_max_cumuls =
        is_delta ? node_with_precedence_to_delta_min_max_cumuls_[node_index]
                 : current_min_max_node_cumuls_[node_index];
    min_max_cumuls.first = min_path_cumuls[i];
    min_max_cumuls.second = max_cumul;

    if (is_delta && !routing_model_.IsEnd(node_index) &&
        (min_max_cumuls.first !=
             current_min_max_node_cumuls_[node_index].first ||
         max_cumul != current_min_max_node_cumuls_[node_index].second)) {
      delta_nodes_with_precedences_and_changed_cumul_.Set(node_index);
    }
  }
}

int64_t PathCumulFilter::ComputePathMaxStartFromEndCumul(
    const PathTransits& path_transits, int path, int64_t path_start,
    int64_t min_end_cumul) const {
  int64_t cumul_from_min_end = min_end_cumul;
  int64_t cumul_from_max_end =
      initial_cumul_[routing_model_.End(start_to_vehicle_[path_start])].max;
  for (int i = path_transits.PathSize(path) - 2; i >= 0; --i) {
    const int64_t transit = path_transits.Transit(path, i);
    const int64_t node = path_transits.Node(path, i);
    cumul_from_min_end =
        std::min(initial_cumul_[node].max, CapSub(cumul_from_min_end, transit));
    cumul_from_max_end = dimension_.GetLastPossibleLessOrEqualValueForNode(
        node, CapSub(cumul_from_max_end, transit));
  }
  return std::min(cumul_from_min_end, cumul_from_max_end);
}

}  // namespace

IntVarLocalSearchFilter* MakePathCumulFilter(const RoutingDimension& dimension,
                                             bool propagate_own_objective_value,
                                             bool filter_objective_cost,
                                             bool may_use_optimizers) {
  RoutingModel& model = *dimension.model();
  return model.solver()->RevAlloc(
      new PathCumulFilter(model, dimension, propagate_own_objective_value,
                          filter_objective_cost, may_use_optimizers));
}

namespace {

bool DimensionHasCumulCost(const RoutingDimension& dimension) {
  if (dimension.global_span_cost_coefficient() != 0) return true;
  if (dimension.HasSoftSpanUpperBounds()) return true;
  if (dimension.HasQuadraticCostSoftSpanUpperBounds()) return true;
  if (absl::c_any_of(dimension.vehicle_span_cost_coefficients(),
                     [](int64_t coefficient) { return coefficient != 0; })) {
    return true;
  }
  if (absl::c_any_of(dimension.vehicle_slack_cost_coefficients(),
                     [](int64_t coefficient) { return coefficient != 0; })) {
    return true;
  }
  for (int i = 0; i < dimension.cumuls().size(); ++i) {
    if (dimension.HasCumulVarSoftUpperBound(i)) return true;
    if (dimension.HasCumulVarSoftLowerBound(i)) return true;
    if (dimension.HasCumulVarPiecewiseLinearCost(i)) return true;
  }
  return false;
}

bool DimensionHasPathCumulConstraint(const RoutingDimension& dimension) {
  if (dimension.HasBreakConstraints()) return true;
  if (dimension.HasPickupToDeliveryLimits()) return true;
  if (absl::c_any_of(
          dimension.vehicle_span_upper_bounds(), [](int64_t upper_bound) {
            return upper_bound != std::numeric_limits<int64_t>::max();
          })) {
    return true;
  }
  if (absl::c_any_of(dimension.slacks(),
                     [](IntVar* slack) { return slack->Min() > 0; })) {
    return true;
  }
  const std::vector<IntVar*>& cumuls = dimension.cumuls();
  for (int i = 0; i < cumuls.size(); ++i) {
    IntVar* const cumul_var = cumuls[i];
    if (cumul_var->Min() > 0 &&
        cumul_var->Max() < std::numeric_limits<int64_t>::max() &&
        !dimension.model()->IsEnd(i)) {
      return true;
    }
    if (dimension.forbidden_intervals()[i].NumIntervals() > 0) return true;
  }
  return false;
}

}  // namespace

void AppendLightWeightDimensionFilters(
    const PathState* path_state,
    const std::vector<RoutingDimension*>& dimensions,
    std::vector<LocalSearchFilterManager::FilterEvent>* filters) {
  using Interval = DimensionChecker::Interval;
  // For every dimension that fits, add a DimensionChecker.
  // Add a DimensionChecker for every dimension.
  for (const RoutingDimension* dimension : dimensions) {
    // Fill path capacities and classes.
    const int num_vehicles = dimension->model()->vehicles();
    std::vector<Interval> path_capacity(num_vehicles);
    std::vector<int> path_class(num_vehicles);
    for (int v = 0; v < num_vehicles; ++v) {
      const auto& vehicle_capacities = dimension->vehicle_capacities();
      path_capacity[v] = {0, vehicle_capacities[v]};
      path_class[v] = dimension->vehicle_to_class(v);
    }
    // For each class, retrieve the demands of each node.
    // Dimension store evaluators with a double indirection for compacity:
    // vehicle -> vehicle_class -> evaluator_index.
    // We replicate this in DimensionChecker,
    // except we expand evaluator_index to an array of values for all nodes.
    const int num_vehicle_classes =
        1 + *std::max_element(path_class.begin(), path_class.end());
    const int num_cumuls = dimension->cumuls().size();
    const int num_slacks = dimension->slacks().size();
    std::vector<std::function<Interval(int64_t, int64_t)>> transits(
        num_vehicle_classes, nullptr);
    for (int vehicle = 0; vehicle < num_vehicles; ++vehicle) {
      const int vehicle_class = path_class[vehicle];
      if (transits[vehicle_class] != nullptr) continue;
      const auto& unary_evaluator =
          dimension->GetUnaryTransitEvaluator(vehicle);
      if (unary_evaluator != nullptr) {
        transits[vehicle_class] = [&unary_evaluator, dimension, num_slacks](
                                      int64_t node, int64_t) -> Interval {
          if (node >= num_slacks) return {0, 0};
          const int64_t min_transit = unary_evaluator(node);
          const int64_t max_transit =
              CapAdd(min_transit, dimension->SlackVar(node)->Max());
          return {min_transit, max_transit};
        };
      } else {
        const auto& binary_evaluator =
            dimension->GetBinaryTransitEvaluator(vehicle);

        transits[vehicle_class] = [&binary_evaluator, dimension, num_slacks](
                                      int64_t node, int64_t next) -> Interval {
          if (node >= num_slacks) return {0, 0};
          const int64_t min_transit = binary_evaluator(node, next);
          const int64_t max_transit =
              CapAdd(min_transit, dimension->SlackVar(node)->Max());
          return {min_transit, max_transit};
        };
      }
    }
    // Fill node capacities.
    std::vector<Interval> node_capacity(num_cumuls);
    for (int node = 0; node < num_cumuls; ++node) {
      const IntVar* cumul = dimension->CumulVar(node);
      node_capacity[node] = {cumul->Min(), cumul->Max()};
    }
    // Make the dimension checker and pass ownership to the filter.
    auto checker = std::make_unique<DimensionChecker>(
        path_state, std::move(path_capacity), std::move(path_class),
        std::move(transits), std::move(node_capacity));
    const auto kAccept = LocalSearchFilterManager::FilterEventType::kAccept;
    LocalSearchFilter* filter = MakeDimensionFilter(
        dimension->model()->solver(), std::move(checker), dimension->name());
    filters->push_back({filter, kAccept});
  }
}

void AppendDimensionCumulFilters(
    const std::vector<RoutingDimension*>& dimensions,
    const RoutingSearchParameters& parameters, bool filter_objective_cost,
    bool use_chain_cumul_filter,
    std::vector<LocalSearchFilterManager::FilterEvent>* filters) {
  const auto kAccept = LocalSearchFilterManager::FilterEventType::kAccept;
  // Filter priority depth increases with complexity of filtering.
  // - Dimensions without any cumul-related costs or constraints will have a
  //   ChainCumulFilter, lowest priority depth.
  // - Dimensions with cumul costs or constraints, but no global span cost
  //   and/or precedences will have a PathCumulFilter.
  // - Dimensions with a global span cost coefficient and/or precedences will
  //   have a global LP filter.
  const int num_dimensions = dimensions.size();

  const bool has_dimension_optimizers =
      !parameters.disable_scheduling_beware_this_may_degrade_performance();
  std::vector<bool> use_path_cumul_filter(num_dimensions);
  std::vector<bool> use_cumul_bounds_propagator_filter(num_dimensions);
  std::vector<bool> use_global_lp_filter(num_dimensions);
  std::vector<bool> use_resource_assignment_filter(num_dimensions);
  for (int d = 0; d < num_dimensions; d++) {
    const RoutingDimension& dimension = *dimensions[d];
    const bool has_cumul_cost = DimensionHasCumulCost(dimension);
    use_path_cumul_filter[d] =
        has_cumul_cost || DimensionHasPathCumulConstraint(dimension);

    const int num_dimension_resource_groups =
        dimension.model()->GetDimensionResourceGroupIndices(&dimension).size();
    const bool can_use_cumul_bounds_propagator_filter =
        !dimension.HasBreakConstraints() &&
        num_dimension_resource_groups == 0 &&
        (!filter_objective_cost || !has_cumul_cost);
    const bool has_precedences = !dimension.GetNodePrecedences().empty();
    use_global_lp_filter[d] =
        has_dimension_optimizers &&
        ((has_precedences && !can_use_cumul_bounds_propagator_filter) ||
         (filter_objective_cost &&
          dimension.global_span_cost_coefficient() > 0) ||
         num_dimension_resource_groups > 1);

    use_cumul_bounds_propagator_filter[d] =
        has_precedences && !use_global_lp_filter[d];

    use_resource_assignment_filter[d] =
        has_dimension_optimizers && num_dimension_resource_groups > 0;
  }

  for (int d = 0; d < num_dimensions; d++) {
    const RoutingDimension& dimension = *dimensions[d];
    const RoutingModel& model = *dimension.model();
    // NOTE: We always add the [Chain|Path]CumulFilter to filter each route's
    // feasibility separately to try and cut bad decisions earlier in the
    // search, but we don't propagate the computed cost if the LPCumulFilter is
    // already doing it.
    const bool use_global_lp = use_global_lp_filter[d];
    const bool filter_resource_assignment = use_resource_assignment_filter[d];
    if (use_path_cumul_filter[d]) {
      PathCumulFilter* filter = model.solver()->RevAlloc(new PathCumulFilter(
          model, dimension, /*propagate_own_objective_value*/ !use_global_lp &&
                                !filter_resource_assignment,
          filter_objective_cost, has_dimension_optimizers));
      const int priority = filter->UsesDimensionOptimizers() ? 1 : 0;
      filters->push_back({filter, kAccept, priority});
    } else if (use_chain_cumul_filter) {
      filters->push_back(
          {model.solver()->RevAlloc(new ChainCumulFilter(model, dimension)),
           kAccept, /*priority*/ 0});
    }

    if (use_cumul_bounds_propagator_filter[d]) {
      DCHECK(!use_global_lp);
      DCHECK(!filter_resource_assignment);
      filters->push_back({MakeCumulBoundsPropagatorFilter(dimension), kAccept,
                          /*priority*/ 2});
    }

    if (filter_resource_assignment) {
      filters->push_back({MakeResourceAssignmentFilter(
                              model.GetMutableLocalCumulLPOptimizer(dimension),
                              model.GetMutableLocalCumulMPOptimizer(dimension),
                              /*propagate_own_objective_value*/ !use_global_lp,
                              filter_objective_cost),
                          kAccept, /*priority*/ 3});
    }

    if (use_global_lp) {
      filters->push_back({MakeGlobalLPCumulFilter(
                              model.GetMutableGlobalCumulLPOptimizer(dimension),
                              model.GetMutableGlobalCumulMPOptimizer(dimension),
                              filter_objective_cost),
                          kAccept, /*priority*/ 4});
    }
  }
}

namespace {

// Filter for pickup/delivery precedences.
class PickupDeliveryFilter : public BasePathFilter {
 public:
  PickupDeliveryFilter(const std::vector<IntVar*>& nexts, int next_domain_size,
                       const PathsMetadata& paths_metadata,
                       const std::vector<PickupDeliveryPair>& pairs,
                       const std::vector<RoutingModel::PickupAndDeliveryPolicy>&
                           vehicle_policies);
  ~PickupDeliveryFilter() override = default;
  bool AcceptPath(int64_t path_start, int64_t chain_start,
                  int64_t chain_end) override;
  std::string DebugString() const override { return "PickupDeliveryFilter"; }

 private:
  bool AcceptPathDefault(int64_t path_start);
  template <bool lifo>
  bool AcceptPathOrdered(int64_t path_start);

  std::vector<int> pair_firsts_;
  std::vector<int> pair_seconds_;
  const std::vector<PickupDeliveryPair> pairs_;
  SparseBitset<> visited_;
  std::deque<int> visited_deque_;
  const std::vector<RoutingModel::PickupAndDeliveryPolicy> vehicle_policies_;
};

PickupDeliveryFilter::PickupDeliveryFilter(
    const std::vector<IntVar*>& nexts, int next_domain_size,
    const PathsMetadata& paths_metadata,
    const std::vector<PickupDeliveryPair>& pairs,
    const std::vector<RoutingModel::PickupAndDeliveryPolicy>& vehicle_policies)
    : BasePathFilter(nexts, next_domain_size, paths_metadata),
      pair_firsts_(next_domain_size, kUnassigned),
      pair_seconds_(next_domain_size, kUnassigned),
      pairs_(pairs),
      visited_(Size()),
      vehicle_policies_(vehicle_policies) {
  for (int i = 0; i < pairs.size(); ++i) {
    const auto& index_pair = pairs[i];
    for (int first : index_pair.pickup_alternatives) {
      pair_firsts_[first] = i;
    }
    for (int second : index_pair.delivery_alternatives) {
      pair_seconds_[second] = i;
    }
  }
}

bool PickupDeliveryFilter::AcceptPath(int64_t path_start,
                                      int64_t /*chain_start*/,
                                      int64_t /*chain_end*/) {
  switch (vehicle_policies_[GetPath(path_start)]) {
    case RoutingModel::PICKUP_AND_DELIVERY_NO_ORDER:
      return AcceptPathDefault(path_start);
    case RoutingModel::PICKUP_AND_DELIVERY_LIFO:
      return AcceptPathOrdered<true>(path_start);
    case RoutingModel::PICKUP_AND_DELIVERY_FIFO:
      return AcceptPathOrdered<false>(path_start);
    default:
      return true;
  }
}

bool PickupDeliveryFilter::AcceptPathDefault(int64_t path_start) {
  visited_.ClearAll();
  int64_t node = path_start;
  int64_t path_length = 1;
  while (node < Size()) {
    // Detect sub-cycles (path is longer than longest possible path).
    if (path_length > Size()) {
      return false;
    }
    if (pair_firsts_[node] != kUnassigned) {
      // Checking on pair firsts is not actually necessary (inconsistencies
      // will get caught when checking pair seconds); doing it anyway to
      // cut checks early.
      for (int second : pairs_[pair_firsts_[node]].delivery_alternatives) {
        if (visited_[second]) {
          return false;
        }
      }
    }
    if (pair_seconds_[node] != kUnassigned) {
      bool found_first = false;
      bool some_synced = false;
      for (int first : pairs_[pair_seconds_[node]].pickup_alternatives) {
        if (visited_[first]) {
          found_first = true;
          break;
        }
        if (IsVarSynced(first)) {
          some_synced = true;
        }
      }
      if (!found_first && some_synced) {
        return false;
      }
    }
    visited_.Set(node);
    const int64_t next = GetNext(node);
    if (next == kUnassigned) {
      // LNS detected, return true since path was ok up to now.
      return true;
    }
    node = next;
    ++path_length;
  }
  for (const int64_t node : visited_.PositionsSetAtLeastOnce()) {
    if (pair_firsts_[node] != kUnassigned) {
      bool found_second = false;
      bool some_synced = false;
      for (int second : pairs_[pair_firsts_[node]].delivery_alternatives) {
        if (visited_[second]) {
          found_second = true;
          break;
        }
        if (IsVarSynced(second)) {
          some_synced = true;
        }
      }
      if (!found_second && some_synced) {
        return false;
      }
    }
  }
  return true;
}

template <bool lifo>
bool PickupDeliveryFilter::AcceptPathOrdered(int64_t path_start) {
  visited_deque_.clear();
  int64_t node = path_start;
  int64_t path_length = 1;
  while (node < Size()) {
    // Detect sub-cycles (path is longer than longest possible path).
    if (path_length > Size()) {
      return false;
    }
    if (pair_firsts_[node] != kUnassigned) {
      if (lifo) {
        visited_deque_.push_back(node);
      } else {
        visited_deque_.push_front(node);
      }
    }
    if (pair_seconds_[node] != kUnassigned) {
      bool found_first = false;
      bool some_synced = false;
      for (int first : pairs_[pair_seconds_[node]].pickup_alternatives) {
        if (!visited_deque_.empty() && visited_deque_.back() == first) {
          found_first = true;
          break;
        }
        if (IsVarSynced(first)) {
          some_synced = true;
        }
      }
      if (!found_first && some_synced) {
        return false;
      } else if (!visited_deque_.empty()) {
        visited_deque_.pop_back();
      }
    }
    const int64_t next = GetNext(node);
    if (next == kUnassigned) {
      // LNS detected, return true since path was ok up to now.
      return true;
    }
    node = next;
    ++path_length;
  }
  while (!visited_deque_.empty()) {
    for (int second :
         pairs_[pair_firsts_[visited_deque_.back()]].delivery_alternatives) {
      if (IsVarSynced(second)) {
        return false;
      }
    }
    visited_deque_.pop_back();
  }
  return true;
}

}  // namespace

IntVarLocalSearchFilter* MakePickupDeliveryFilter(
    const RoutingModel& routing_model,
    const std::vector<PickupDeliveryPair>& pairs,
    const std::vector<RoutingModel::PickupAndDeliveryPolicy>&
        vehicle_policies) {
  return routing_model.solver()->RevAlloc(new PickupDeliveryFilter(
      routing_model.Nexts(), routing_model.Size() + routing_model.vehicles(),
      routing_model.GetPathsMetadata(), pairs, vehicle_policies));
}

namespace {

// Vehicle variable filter
class VehicleVarFilter : public BasePathFilter {
 public:
  explicit VehicleVarFilter(const RoutingModel& routing_model);
  ~VehicleVarFilter() override = default;
  bool AcceptPath(int64_t path_start, int64_t chain_start,
                  int64_t chain_end) override;
  std::string DebugString() const override { return "VehicleVariableFilter"; }

 private:
  bool DisableFiltering() const override;
  bool IsVehicleVariableConstrained(int index) const;

  std::vector<int64_t> start_to_vehicle_;
  std::vector<IntVar*> vehicle_vars_;
  const int64_t unconstrained_vehicle_var_domain_size_;
  SparseBitset<int> touched_;
};

VehicleVarFilter::VehicleVarFilter(const RoutingModel& routing_model)
    : BasePathFilter(routing_model.Nexts(),
                     routing_model.Size() + routing_model.vehicles(),
                     routing_model.GetPathsMetadata()),
      vehicle_vars_(routing_model.VehicleVars()),
      unconstrained_vehicle_var_domain_size_(routing_model.vehicles()),
      touched_(routing_model.Nexts().size()) {
  start_to_vehicle_.resize(Size(), -1);
  for (int i = 0; i < routing_model.vehicles(); ++i) {
    start_to_vehicle_[routing_model.Start(i)] = i;
  }
}

bool VehicleVarFilter::AcceptPath(int64_t path_start, int64_t chain_start,
                                  int64_t chain_end) {
  touched_.SparseClearAll();
  const int64_t vehicle = start_to_vehicle_[path_start];
  int64_t node = chain_start;
  while (node != chain_end) {
    if (touched_[node] || !vehicle_vars_[node]->Contains(vehicle)) {
      return false;
    }
    touched_.Set(node);
    node = GetNext(node);
  }
  return vehicle_vars_[node]->Contains(vehicle);
}

bool VehicleVarFilter::DisableFiltering() const {
  for (int i = 0; i < vehicle_vars_.size(); ++i) {
    if (IsVehicleVariableConstrained(i)) return false;
  }
  return true;
}

bool VehicleVarFilter::IsVehicleVariableConstrained(int index) const {
  const IntVar* const vehicle_var = vehicle_vars_[index];
  // If vehicle variable contains -1 (optional node), then we need to
  // add it to the "unconstrained" domain. Impact we don't filter mandatory
  // nodes made inactive here, but it is covered by other filters.
  const int adjusted_unconstrained_vehicle_var_domain_size =
      vehicle_var->Min() >= 0 ? unconstrained_vehicle_var_domain_size_
                              : unconstrained_vehicle_var_domain_size_ + 1;
  return vehicle_var->Size() != adjusted_unconstrained_vehicle_var_domain_size;
}

}  // namespace

IntVarLocalSearchFilter* MakeVehicleVarFilter(
    const RoutingModel& routing_model) {
  return routing_model.solver()->RevAlloc(new VehicleVarFilter(routing_model));
}

namespace {

class CumulBoundsPropagatorFilter : public IntVarLocalSearchFilter {
 public:
  explicit CumulBoundsPropagatorFilter(const RoutingDimension& dimension);
  bool Accept(const Assignment* delta, const Assignment* deltadelta,
              int64_t objective_min, int64_t objective_max) override;
  std::string DebugString() const override {
    return "CumulBoundsPropagatorFilter(" + propagator_.dimension().name() +
           ")";
  }

 private:
  CumulBoundsPropagator propagator_;
  const int64_t cumul_offset_;
  SparseBitset<int64_t> delta_touched_;
  std::vector<int64_t> delta_nexts_;
};

CumulBoundsPropagatorFilter::CumulBoundsPropagatorFilter(
    const RoutingDimension& dimension)
    : IntVarLocalSearchFilter(dimension.model()->Nexts()),
      propagator_(&dimension),
      cumul_offset_(dimension.GetGlobalOptimizerOffset()),
      delta_touched_(Size()),
      delta_nexts_(Size()) {}

bool CumulBoundsPropagatorFilter::Accept(const Assignment* delta,
                                         const Assignment* /*deltadelta*/,
                                         int64_t /*objective_min*/,
                                         int64_t /*objective_max*/) {
  delta_touched_.ClearAll();
  for (const IntVarElement& delta_element :
       delta->IntVarContainer().elements()) {
    int64_t index = -1;
    if (FindIndex(delta_element.Var(), &index)) {
      if (!delta_element.Bound()) {
        // LNS detected
        return true;
      }
      delta_touched_.Set(index);
      delta_nexts_[index] = delta_element.Value();
    }
  }
  const auto& next_accessor = [this](int64_t index) {
    return delta_touched_[index] ? delta_nexts_[index] : Value(index);
  };

  return propagator_.PropagateCumulBounds(next_accessor, cumul_offset_);
}

}  // namespace

IntVarLocalSearchFilter* MakeCumulBoundsPropagatorFilter(
    const RoutingDimension& dimension) {
  return dimension.model()->solver()->RevAlloc(
      new CumulBoundsPropagatorFilter(dimension));
}

namespace {

class LPCumulFilter : public IntVarLocalSearchFilter {
 public:
  LPCumulFilter(const std::vector<IntVar*>& nexts,
                GlobalDimensionCumulOptimizer* optimizer,
                GlobalDimensionCumulOptimizer* mp_optimizer,
                bool filter_objective_cost);
  bool Accept(const Assignment* delta, const Assignment* deltadelta,
              int64_t objective_min, int64_t objective_max) override;
  int64_t GetAcceptedObjectiveValue() const override;
  void OnSynchronize(const Assignment* delta) override;
  int64_t GetSynchronizedObjectiveValue() const override;
  std::string DebugString() const override {
    return "LPCumulFilter(" + lp_optimizer_.dimension()->name() + ")";
  }

 private:
  GlobalDimensionCumulOptimizer& lp_optimizer_;
  GlobalDimensionCumulOptimizer& mp_optimizer_;
  const bool filter_objective_cost_;
  int64_t synchronized_cost_without_transit_;
  int64_t delta_cost_without_transit_;
  SparseBitset<int64_t> delta_touched_;
  std::vector<int64_t> delta_nexts_;
};

LPCumulFilter::LPCumulFilter(const std::vector<IntVar*>& nexts,
                             GlobalDimensionCumulOptimizer* lp_optimizer,
                             GlobalDimensionCumulOptimizer* mp_optimizer,
                             bool filter_objective_cost)
    : IntVarLocalSearchFilter(nexts),
      lp_optimizer_(*lp_optimizer),
      mp_optimizer_(*mp_optimizer),
      filter_objective_cost_(filter_objective_cost),
      synchronized_cost_without_transit_(-1),
      delta_cost_without_transit_(-1),
      delta_touched_(Size()),
      delta_nexts_(Size()) {}

bool LPCumulFilter::Accept(const Assignment* delta,
                           const Assignment* /*deltadelta*/,
                           int64_t /*objective_min*/, int64_t objective_max) {
  delta_touched_.ClearAll();
  for (const IntVarElement& delta_element :
       delta->IntVarContainer().elements()) {
    int64_t index = -1;
    if (FindIndex(delta_element.Var(), &index)) {
      if (!delta_element.Bound()) {
        // LNS detected
        return true;
      }
      delta_touched_.Set(index);
      delta_nexts_[index] = delta_element.Value();
    }
  }
  const auto& next_accessor = [this](int64_t index) {
    return delta_touched_[index] ? delta_nexts_[index] : Value(index);
  };

  if (!filter_objective_cost_) {
    // No need to compute the cost of the LP, only verify its feasibility.
    delta_cost_without_transit_ = 0;
    const DimensionSchedulingStatus status = lp_optimizer_.ComputeCumuls(
        next_accessor, {}, nullptr, nullptr, nullptr);
    if (status == DimensionSchedulingStatus::OPTIMAL) return true;
    if (status == DimensionSchedulingStatus::RELAXED_OPTIMAL_ONLY &&
        mp_optimizer_.ComputeCumuls(next_accessor, {}, nullptr, nullptr,
                                    nullptr) ==
            DimensionSchedulingStatus::OPTIMAL) {
      return true;
    }
    return false;
  }

  const DimensionSchedulingStatus status =
      lp_optimizer_.ComputeCumulCostWithoutFixedTransits(
          next_accessor, &delta_cost_without_transit_);
  if (status == DimensionSchedulingStatus::INFEASIBLE) {
    delta_cost_without_transit_ = std::numeric_limits<int64_t>::max();
    return false;
  }
  if (delta_cost_without_transit_ > objective_max) return false;

  if (status == DimensionSchedulingStatus::RELAXED_OPTIMAL_ONLY &&
      mp_optimizer_.ComputeCumulCostWithoutFixedTransits(
          next_accessor, &delta_cost_without_transit_) !=
          DimensionSchedulingStatus::OPTIMAL) {
    delta_cost_without_transit_ = std::numeric_limits<int64_t>::max();
    return false;
  }
  return delta_cost_without_transit_ <= objective_max;
}

int64_t LPCumulFilter::GetAcceptedObjectiveValue() const {
  return delta_cost_without_transit_;
}

void LPCumulFilter::OnSynchronize(const Assignment* /*delta*/) {
  // TODO(user): Try to optimize this so the LP is not called when the last
  // computed delta cost corresponds to the solution being synchronized.
  const RoutingModel& model = *lp_optimizer_.dimension()->model();
  const auto& next_accessor = [this, &model](int64_t index) {
    return IsVarSynced(index)     ? Value(index)
           : model.IsStart(index) ? model.End(model.VehicleIndex(index))
                                  : index;
  };

  if (!filter_objective_cost_) {
    synchronized_cost_without_transit_ = 0;
  }
  DimensionSchedulingStatus status =
      filter_objective_cost_
          ? lp_optimizer_.ComputeCumulCostWithoutFixedTransits(
                next_accessor, &synchronized_cost_without_transit_)
          : lp_optimizer_.ComputeCumuls(next_accessor, {}, nullptr, nullptr,
                                        nullptr);
  if (status == DimensionSchedulingStatus::INFEASIBLE) {
    // TODO(user): This should only happen if the LP solver times out.
    // DCHECK the fail wasn't due to an infeasible model.
    synchronized_cost_without_transit_ = 0;
  }
  if (status == DimensionSchedulingStatus::RELAXED_OPTIMAL_ONLY) {
    status = filter_objective_cost_
                 ? mp_optimizer_.ComputeCumulCostWithoutFixedTransits(
                       next_accessor, &synchronized_cost_without_transit_)
                 : mp_optimizer_.ComputeCumuls(next_accessor, {}, nullptr,
                                               nullptr, nullptr);
    if (status != DimensionSchedulingStatus::OPTIMAL) {
      // TODO(user): This should only happen if the MP solver times out.
      // DCHECK the fail wasn't due to an infeasible model.
      synchronized_cost_without_transit_ = 0;
    }
  }
}

int64_t LPCumulFilter::GetSynchronizedObjectiveValue() const {
  return synchronized_cost_without_transit_;
}

}  // namespace

IntVarLocalSearchFilter* MakeGlobalLPCumulFilter(
    GlobalDimensionCumulOptimizer* lp_optimizer,
    GlobalDimensionCumulOptimizer* mp_optimizer, bool filter_objective_cost) {
  DCHECK_NE(lp_optimizer, nullptr);
  DCHECK_NE(mp_optimizer, nullptr);
  const RoutingModel& model = *lp_optimizer->dimension()->model();
  return model.solver()->RevAlloc(new LPCumulFilter(
      model.Nexts(), lp_optimizer, mp_optimizer, filter_objective_cost));
}

namespace {

using ResourceGroup = RoutingModel::ResourceGroup;

class ResourceGroupAssignmentFilter : public BasePathFilter {
 public:
  ResourceGroupAssignmentFilter(const std::vector<IntVar*>& nexts,
                                const ResourceGroup* resource_group,
                                LocalDimensionCumulOptimizer* lp_optimizer,
                                LocalDimensionCumulOptimizer* mp_optimizer,
                                bool filter_objective_cost);
  bool InitializeAcceptPath() override;
  bool AcceptPath(int64_t path_start, int64_t, int64_t) override;
  bool FinalizeAcceptPath(int64_t objective_min,
                          int64_t objective_max) override;
  void OnBeforeSynchronizePaths() override;
  void OnSynchronizePathFromStart(int64_t start) override;
  void OnAfterSynchronizePaths() override;

  int64_t GetAcceptedObjectiveValue() const override {
    return lns_detected() ? 0 : delta_cost_without_transit_;
  }
  int64_t GetSynchronizedObjectiveValue() const override {
    return synchronized_cost_without_transit_;
  }
  std::string DebugString() const override {
    return "ResourceGroupAssignmentFilter(" + dimension_.name() + ")";
  }

 private:
  using RCIndex = RoutingModel::ResourceClassIndex;

  bool VehicleRequiresResourceAssignment(
      int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
      bool* is_infeasible);
  int64_t ComputeRouteCumulCostWithoutResourceAssignment(
      int vehicle, const std::function<int64_t(int64_t)>& next_accessor) const;

  const RoutingModel& model_;
  const RoutingDimension& dimension_;
  const ResourceGroup& resource_group_;
  LocalDimensionCumulOptimizer* lp_optimizer_;
  LocalDimensionCumulOptimizer* mp_optimizer_;
  const bool filter_objective_cost_;
  bool current_synch_failed_;
  int64_t synchronized_cost_without_transit_;
  int64_t delta_cost_without_transit_;
  std::vector<std::vector<int64_t>> vehicle_to_resource_class_assignment_costs_;
  std::vector<int> vehicles_requiring_resource_assignment_;
  std::vector<bool> vehicle_requires_resource_assignment_;
  std::vector<std::vector<int64_t>>
      delta_vehicle_to_resource_class_assignment_costs_;
  std::vector<int> delta_vehicles_requiring_resource_assignment_;
  std::vector<bool> delta_vehicle_requires_resource_assignment_;

  std::vector<int> bound_resource_index_of_vehicle_;
  util_intops::StrongVector<RCIndex, absl::flat_hash_set<int>>
      ignored_resources_per_class_;
};

ResourceGroupAssignmentFilter::ResourceGroupAssignmentFilter(
    const std::vector<IntVar*>& nexts, const ResourceGroup* resource_group,
    LocalDimensionCumulOptimizer* lp_optimizer,
    LocalDimensionCumulOptimizer* mp_optimizer, bool filter_objective_cost)
    : BasePathFilter(nexts, lp_optimizer->dimension()->cumuls().size(),
                     lp_optimizer->dimension()->model()->GetPathsMetadata()),
      model_(*lp_optimizer->dimension()->model()),
      dimension_(*lp_optimizer->dimension()),
      resource_group_(*resource_group),
      lp_optimizer_(lp_optimizer),
      mp_optimizer_(mp_optimizer),
      filter_objective_cost_(filter_objective_cost),
      current_synch_failed_(false),
      synchronized_cost_without_transit_(-1),
      delta_cost_without_transit_(-1) {
  vehicle_to_resource_class_assignment_costs_.resize(model_.vehicles());
  delta_vehicle_to_resource_class_assignment_costs_.resize(model_.vehicles());
}

bool ResourceGroupAssignmentFilter::InitializeAcceptPath() {
  delta_vehicle_to_resource_class_assignment_costs_.assign(model_.vehicles(),
                                                           {});
  if (current_synch_failed_) {
    return true;
  }
  // TODO(user): Keep track of num_used_vehicles internally and compute its
  // new value here by only going through the touched_paths_.
  int num_used_vehicles = 0;
  const int num_resources = resource_group_.Size();
  for (int v : resource_group_.GetVehiclesRequiringAResource()) {
    if (GetNext(model_.Start(v)) != model_.End(v) ||
        model_.IsVehicleUsedWhenEmpty(v)) {
      if (++num_used_vehicles > num_resources) {
        return false;
      }
    }
  }
  delta_vehicle_requires_resource_assignment_ =
      vehicle_requires_resource_assignment_;
  return true;
}

bool ResourceGroupAssignmentFilter::AcceptPath(int64_t path_start, int64_t,
                                               int64_t) {
  if (current_synch_failed_) {
    return true;
  }
  const int vehicle = model_.VehicleIndex(path_start);
  bool is_infeasible = false;
  delta_vehicle_requires_resource_assignment_[vehicle] =
      VehicleRequiresResourceAssignment(
          vehicle, [this](int64_t n) { return GetNext(n); }, &is_infeasible);
  return !is_infeasible;
}

bool ResourceGroupAssignmentFilter::FinalizeAcceptPath(
    int64_t /*objective_min*/, int64_t objective_max) {
  delta_cost_without_transit_ = 0;
  if (current_synch_failed_) {
    return true;
  }
  const auto& next_accessor = [this](int64_t index) { return GetNext(index); };
  delta_vehicles_requiring_resource_assignment_.clear();
  // First sum the costs of the routes not requiring resource assignment
  // (cheaper computations).
  for (int v = 0; v < model_.vehicles(); ++v) {
    if (delta_vehicle_requires_resource_assignment_[v]) {
      delta_vehicles_requiring_resource_assignment_.push_back(v);
      continue;
    }
    int64_t route_cost = 0;
    int64_t start = model_.Start(v);
    if (PathStartTouched(start)) {
      route_cost =
          ComputeRouteCumulCostWithoutResourceAssignment(v, next_accessor);
      if (route_cost < 0) {
        return false;
      }
    } else if (IsVarSynced(start)) {
      DCHECK_EQ(vehicle_to_resource_class_assignment_costs_[v].size(), 1);
      route_cost = vehicle_to_resource_class_assignment_costs_[v][0];
    }
    CapAddTo(route_cost, &delta_cost_without_transit_);
    if (delta_cost_without_transit_ > objective_max) {
      return false;
    }
  }
  // Recompute the assignment costs to resources for touched paths requiring
  // resource assignment.
  for (int64_t start : GetTouchedPathStarts()) {
    const int vehicle = model_.VehicleIndex(start);
    if (!delta_vehicle_requires_resource_assignment_[vehicle]) {
      // Already handled above.
      continue;
    }
    if (!ComputeVehicleToResourceClassAssignmentCosts(
            vehicle, resource_group_, ignored_resources_per_class_,
            next_accessor, dimension_.transit_evaluator(vehicle),
            filter_objective_cost_, lp_optimizer_, mp_optimizer_,
            &delta_vehicle_to_resource_class_assignment_costs_[vehicle],
            nullptr, nullptr)) {
      return false;
    }
  }
  const int64_t assignment_cost = ComputeBestVehicleToResourceAssignment(
      delta_vehicles_requiring_resource_assignment_,
      resource_group_.GetResourceIndicesPerClass(),
      ignored_resources_per_class_,
      /*vehicle_to_resource_class_assignment_costs=*/
      [this](int v) {
        return PathStartTouched(model_.Start(v))
                   ? &delta_vehicle_to_resource_class_assignment_costs_[v]
                   : &vehicle_to_resource_class_assignment_costs_[v];
      },
      nullptr);
  CapAddTo(assignment_cost, &delta_cost_without_transit_);
  return assignment_cost >= 0 && delta_cost_without_transit_ <= objective_max;
}

void ResourceGroupAssignmentFilter::OnBeforeSynchronizePaths() {
  if (!HasAnySyncedPath()) {
    vehicle_to_resource_class_assignment_costs_.assign(model_.vehicles(), {});
  }
  bound_resource_index_of_vehicle_.assign(model_.vehicles(), -1);
  vehicles_requiring_resource_assignment_.clear();
  vehicles_requiring_resource_assignment_.reserve(
      resource_group_.GetVehiclesRequiringAResource().size());
  vehicle_requires_resource_assignment_.assign(model_.vehicles(), false);
  ignored_resources_per_class_.assign(resource_group_.GetResourceClassesCount(),
                                      absl::flat_hash_set<int>());

  for (int v : resource_group_.GetVehiclesRequiringAResource()) {
    const int64_t start = model_.Start(v);
    if (!IsVarSynced(start)) {
      continue;
    }
    vehicle_requires_resource_assignment_[v] =
        VehicleRequiresResourceAssignment(
            v, [this](int64_t n) { return Value(n); }, &current_synch_failed_);
    if (vehicle_requires_resource_assignment_[v]) {
      vehicles_requiring_resource_assignment_.push_back(v);
    }
    if (current_synch_failed_) {
      return;
    }
  }
  synchronized_cost_without_transit_ = 0;
}

void ResourceGroupAssignmentFilter::OnSynchronizePathFromStart(int64_t start) {
  if (current_synch_failed_) return;
  DCHECK(IsVarSynced(start));
  const int v = model_.VehicleIndex(start);
  const auto& next_accessor = [this](int64_t index) { return Value(index); };
  if (!vehicle_requires_resource_assignment_[v]) {
    const int64_t route_cost =
        ComputeRouteCumulCostWithoutResourceAssignment(v, next_accessor);
    if (route_cost < 0) {
      current_synch_failed_ = true;
      return;
    }
    CapAddTo(route_cost, &synchronized_cost_without_transit_);
    vehicle_to_resource_class_assignment_costs_[v] = {route_cost};
    return;
  }
  // NOTE(user): Even if filter_objective_cost_ is false, we
  // still need to call ComputeVehicleToResourceClassAssignmentCosts() for every
  // vehicle requiring resource assignment to keep track of whether or not a
  // given vehicle-to-resource-class assignment is possible by storing 0 or -1
  // in vehicle_to_resource_class_assignment_costs_.
  if (!ComputeVehicleToResourceClassAssignmentCosts(
          v, resource_group_, ignored_resources_per_class_, next_accessor,
          dimension_.transit_evaluator(v), filter_objective_cost_,
          lp_optimizer_, mp_optimizer_,
          &vehicle_to_resource_class_assignment_costs_[v], nullptr, nullptr)) {
    vehicle_to_resource_class_assignment_costs_[v].assign(
        resource_group_.GetResourceClassesCount(), -1);
    current_synch_failed_ = true;
  }
}

void ResourceGroupAssignmentFilter::OnAfterSynchronizePaths() {
  if (current_synch_failed_) {
    synchronized_cost_without_transit_ = 0;
    return;
  }
  if (!filter_objective_cost_) {
    DCHECK_EQ(synchronized_cost_without_transit_, 0);
    return;
  }
  const int64_t assignment_cost = ComputeBestVehicleToResourceAssignment(
      vehicles_requiring_resource_assignment_,
      resource_group_.GetResourceIndicesPerClass(),
      ignored_resources_per_class_,
      [this](int v) { return &vehicle_to_resource_class_assignment_costs_[v]; },
      nullptr);
  if (assignment_cost < 0) {
    synchronized_cost_without_transit_ = 0;
    current_synch_failed_ = true;
  } else {
    DCHECK_GE(synchronized_cost_without_transit_, 0);
    CapAddTo(assignment_cost, &synchronized_cost_without_transit_);
  }
}

bool ResourceGroupAssignmentFilter::VehicleRequiresResourceAssignment(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor,
    bool* is_infeasible) {
  *is_infeasible = false;
  if (!resource_group_.VehicleRequiresAResource(vehicle)) return false;
  const IntVar* res_var = model_.ResourceVar(vehicle, resource_group_.Index());
  if (!model_.IsVehicleUsedWhenEmpty(vehicle) &&
      next_accessor(model_.Start(vehicle)) == model_.End(vehicle)) {
    if (res_var->Bound() && res_var->Value() >= 0) {
      // Vehicle with a resource (force-)assigned to it cannot be unused.
      *is_infeasible = true;
    }
    return false;
  }
  // Vehicle is used.
  if (res_var->Bound()) {
    // No need to do resource assignment for this vehicle.
    const int res = res_var->Value();
    if (res < 0) {
      // Vehicle has a negative resource index enforced but is used.
      *is_infeasible = true;
    } else {
      bound_resource_index_of_vehicle_[vehicle] = res;
      const RCIndex rc = resource_group_.GetResourceClassIndex(res);
      ignored_resources_per_class_[rc].insert(res);
    }
    return false;
  }
  // Vehicle is used and res_var isn't bound.
  return true;
}

int64_t
ResourceGroupAssignmentFilter::ComputeRouteCumulCostWithoutResourceAssignment(
    int vehicle, const std::function<int64_t(int64_t)>& next_accessor) const {
  if (next_accessor(model_.Start(vehicle)) == model_.End(vehicle) &&
      !model_.IsVehicleUsedWhenEmpty(vehicle)) {
    return 0;
  }
  using Resource = RoutingModel::ResourceGroup::Resource;
  const Resource* resource = nullptr;
  if (resource_group_.VehicleRequiresAResource(vehicle)) {
    DCHECK_GE(bound_resource_index_of_vehicle_[vehicle], 0);
    resource =
        &resource_group_.GetResource(bound_resource_index_of_vehicle_[vehicle]);
  }
  int64_t route_cost = 0;
  const DimensionSchedulingStatus status =
      lp_optimizer_->ComputeRouteCumulCostWithoutFixedTransits(
          vehicle, next_accessor, resource,
          filter_objective_cost_ ? &route_cost : nullptr);
  switch (status) {
    case DimensionSchedulingStatus::INFEASIBLE:
      return -1;
    case DimensionSchedulingStatus::RELAXED_OPTIMAL_ONLY:
      if (mp_optimizer_->ComputeRouteCumulCostWithoutFixedTransits(
              vehicle, next_accessor, resource,
              filter_objective_cost_ ? &route_cost : nullptr) ==
          DimensionSchedulingStatus::INFEASIBLE) {
        return -1;
      }
      break;
    default:
      DCHECK(status == DimensionSchedulingStatus::OPTIMAL);
  }
  return route_cost;
}

// ResourceAssignmentFilter
class ResourceAssignmentFilter : public LocalSearchFilter {
 public:
  ResourceAssignmentFilter(const std::vector<IntVar*>& nexts,
                           LocalDimensionCumulOptimizer* optimizer,
                           LocalDimensionCumulOptimizer* mp_optimizer,
                           bool propagate_own_objective_value,
                           bool filter_objective_cost);
  bool Accept(const Assignment* delta, const Assignment* deltadelta,
              int64_t objective_min, int64_t objective_max) override;
  void Synchronize(const Assignment* assignment,
                   const Assignment* delta) override;

  int64_t GetAcceptedObjectiveValue() const override {
    return propagate_own_objective_value_ ? delta_cost_ : 0;
  }
  int64_t GetSynchronizedObjectiveValue() const override {
    return propagate_own_objective_value_ ? synchronized_cost_ : 0;
  }
  std::string DebugString() const override {
    return "ResourceAssignmentFilter(" + dimension_name_ + ")";
  }

 private:
  std::vector<IntVarLocalSearchFilter*> resource_group_assignment_filters_;
  int64_t synchronized_cost_;
  int64_t delta_cost_;
  const bool propagate_own_objective_value_;
  const std::string dimension_name_;
};

ResourceAssignmentFilter::ResourceAssignmentFilter(
    const std::vector<IntVar*>& nexts,
    LocalDimensionCumulOptimizer* lp_optimizer,
    LocalDimensionCumulOptimizer* mp_optimizer,
    bool propagate_own_objective_value, bool filter_objective_cost)
    : propagate_own_objective_value_(propagate_own_objective_value),
      dimension_name_(lp_optimizer->dimension()->name()) {
  const RoutingModel& model = *lp_optimizer->dimension()->model();
  for (const auto& resource_group : model.GetResourceGroups()) {
    resource_group_assignment_filters_.push_back(
        model.solver()->RevAlloc(new ResourceGroupAssignmentFilter(
            nexts, resource_group.get(), lp_optimizer, mp_optimizer,
            filter_objective_cost)));
  }
}

bool ResourceAssignmentFilter::Accept(const Assignment* delta,
                                      const Assignment* deltadelta,
                                      int64_t objective_min,
                                      int64_t objective_max) {
  delta_cost_ = 0;
  for (LocalSearchFilter* group_filter : resource_group_assignment_filters_) {
    if (!group_filter->Accept(delta, deltadelta, objective_min,
                              objective_max)) {
      return false;
    }
    delta_cost_ =
        std::max(delta_cost_, group_filter->GetAcceptedObjectiveValue());
    DCHECK_LE(delta_cost_, objective_max)
        << "ResourceGroupAssignmentFilter should return false when the "
           "objective_max is exceeded.";
  }
  return true;
}

void ResourceAssignmentFilter::Synchronize(const Assignment* assignment,
                                           const Assignment* delta) {
  synchronized_cost_ = 0;
  for (LocalSearchFilter* group_filter : resource_group_assignment_filters_) {
    group_filter->Synchronize(assignment, delta);
    synchronized_cost_ = std::max(
        synchronized_cost_, group_filter->GetSynchronizedObjectiveValue());
  }
}

}  // namespace

LocalSearchFilter* MakeResourceAssignmentFilter(
    LocalDimensionCumulOptimizer* lp_optimizer,
    LocalDimensionCumulOptimizer* mp_optimizer,
    bool propagate_own_objective_value, bool filter_objective_cost) {
  const RoutingModel& model = *lp_optimizer->dimension()->model();
  DCHECK_NE(lp_optimizer, nullptr);
  DCHECK_NE(mp_optimizer, nullptr);
  return model.solver()->RevAlloc(new ResourceAssignmentFilter(
      model.Nexts(), lp_optimizer, mp_optimizer, propagate_own_objective_value,
      filter_objective_cost));
}

namespace {

// This filter accepts deltas for which the assignment satisfies the
// constraints of the Solver. This is verified by keeping an internal copy of
// the assignment with all Next vars and their updated values, and calling
// RestoreAssignment() on the assignment+delta.
// TODO(user): Also call the solution finalizer on variables, with the
// exception of Next Vars (woud fail on large instances).
// WARNING: In the case of mandatory nodes, when all vehicles are currently
// being used in the solution but uninserted nodes still remain, this filter
// will reject the solution, even if the node could be inserted on one of these
// routes, because all Next vars of vehicle starts are already instantiated.
// TODO(user): Avoid such false negatives.

class CPFeasibilityFilter : public IntVarLocalSearchFilter {
 public:
  explicit CPFeasibilityFilter(RoutingModel* routing_model);
  ~CPFeasibilityFilter() override = default;
  std::string DebugString() const override { return "CPFeasibilityFilter"; }
  bool Accept(const Assignment* delta, const Assignment* deltadelta,
              int64_t objective_min, int64_t objective_max) override;
  void OnSynchronize(const Assignment* delta) override;

 private:
  void AddDeltaToAssignment(const Assignment* delta, Assignment* assignment);

  static const int64_t kUnassigned;
  const RoutingModel* const model_;
  Solver* const solver_;
  Assignment* const assignment_;
  Assignment* const temp_assignment_;
  DecisionBuilder* const restore_;
  SearchLimit* const limit_;
};

const int64_t CPFeasibilityFilter::kUnassigned = -1;

CPFeasibilityFilter::CPFeasibilityFilter(RoutingModel* routing_model)
    : IntVarLocalSearchFilter(routing_model->Nexts()),
      model_(routing_model),
      solver_(routing_model->solver()),
      assignment_(solver_->MakeAssignment()),
      temp_assignment_(solver_->MakeAssignment()),
      restore_(solver_->MakeRestoreAssignment(temp_assignment_)),
      limit_(solver_->MakeCustomLimit(
          [routing_model]() { return routing_model->CheckLimit(); })) {
  assignment_->Add(routing_model->Nexts());
}

bool CPFeasibilityFilter::Accept(const Assignment* delta,
                                 const Assignment* /*deltadelta*/,
                                 int64_t /*objective_min*/,
                                 int64_t /*objective_max*/) {
  temp_assignment_->Copy(assignment_);
  AddDeltaToAssignment(delta, temp_assignment_);

  return solver_->Solve(restore_, limit_);
}

void CPFeasibilityFilter::OnSynchronize(const Assignment* delta) {
  AddDeltaToAssignment(delta, assignment_);
}

void CPFeasibilityFilter::AddDeltaToAssignment(const Assignment* delta,
                                               Assignment* assignment) {
  if (delta == nullptr) {
    return;
  }
  Assignment::IntContainer* const container =
      assignment->MutableIntVarContainer();
  for (const IntVarElement& delta_element :
       delta->IntVarContainer().elements()) {
    IntVar* const var = delta_element.Var();
    int64_t index = kUnassigned;
    // Ignoring variables found in the delta which are not next variables, such
    // as vehicle variables.
    if (!FindIndex(var, &index)) continue;
    DCHECK_EQ(var, Var(index));
    const int64_t value = delta_element.Value();

    container->AddAtPosition(var, index)->SetValue(value);
    if (model_->IsStart(index)) {
      if (model_->IsEnd(value)) {
        // Do not restore unused routes.
        container->MutableElement(index)->Deactivate();
      } else {
        // Re-activate the route's start in case it was deactivated before.
        container->MutableElement(index)->Activate();
      }
    }
  }
}

}  // namespace

IntVarLocalSearchFilter* MakeCPFeasibilityFilter(RoutingModel* routing_model) {
  return routing_model->solver()->RevAlloc(
      new CPFeasibilityFilter(routing_model));
}

PathState::PathState(int num_nodes, std::vector<int> path_start,
                     std::vector<int> path_end)
    : num_nodes_(num_nodes),
      num_paths_(path_start.size()),
      num_nodes_threshold_(std::max(16, 4 * num_nodes_))  // Arbitrary value.
{
  DCHECK_EQ(path_start.size(), num_paths_);
  DCHECK_EQ(path_end.size(), num_paths_);
  for (int p = 0; p < num_paths_; ++p) {
    path_start_end_.push_back({path_start[p], path_end[p]});
  }
  // Initial state is all unperformed: paths go from start to end directly.
  committed_index_.assign(num_nodes_, -1);
  committed_paths_.assign(num_nodes_, -1);
  committed_nodes_.assign(2 * num_paths_, -1);
  chains_.assign(num_paths_ + 1, {-1, -1});  // Reserve 1 more for sentinel.
  paths_.assign(num_paths_, {-1, -1});
  for (int path = 0; path < num_paths_; ++path) {
    const int index = 2 * path;
    const auto& [start, end] = path_start_end_[path];
    committed_index_[start] = index;
    committed_index_[end] = index + 1;

    committed_nodes_[index] = start;
    committed_nodes_[index + 1] = end;

    committed_paths_[start] = path;
    committed_paths_[end] = path;

    chains_[path] = {index, index + 2};
    paths_[path] = {path, path + 1};
  }
  chains_[num_paths_] = {0, 0};  // Sentinel.
  // Nodes that are not starts or ends are loops.
  for (int node = 0; node < num_nodes_; ++node) {
    if (committed_index_[node] != -1) continue;  // node is start or end.
    committed_index_[node] = committed_nodes_.size();
    committed_nodes_.push_back(node);
  }
}

PathState::ChainRange PathState::Chains(int path) const {
  const PathBounds bounds = paths_[path];
  return PathState::ChainRange(chains_.data() + bounds.begin_index,
                               chains_.data() + bounds.end_index,
                               committed_nodes_.data());
}

PathState::NodeRange PathState::Nodes(int path) const {
  const PathBounds bounds = paths_[path];
  return PathState::NodeRange(chains_.data() + bounds.begin_index,
                              chains_.data() + bounds.end_index,
                              committed_nodes_.data());
}

void PathState::ChangePath(int path, absl::Span<const ChainBounds> chains) {
  changed_paths_.push_back(path);
  const int path_begin_index = chains_.size();
  chains_.insert(chains_.end(), chains.begin(), chains.end());
  const int path_end_index = chains_.size();
  paths_[path] = {path_begin_index, path_end_index};
  chains_.emplace_back(0, 0);  // Sentinel.
}

void PathState::ChangeLoops(absl::Span<const int> new_loops) {
  for (const int loop : new_loops) {
    if (Path(loop) == -1) continue;
    changed_loops_.push_back(loop);
  }
}

void PathState::Commit() {
  DCHECK(!IsInvalid());
  if (committed_nodes_.size() < num_nodes_threshold_) {
    IncrementalCommit();
  } else {
    FullCommit();
  }
}

void PathState::Revert() {
  is_invalid_ = false;
  chains_.resize(num_paths_ + 1);  // One per path + sentinel.
  for (const int path : changed_paths_) {
    paths_[path] = {path, path + 1};
  }
  changed_paths_.clear();
  changed_loops_.clear();
}

void PathState::CopyNewPathAtEndOfNodes(int path) {
  // Copy path's nodes, chain by chain.
  const PathBounds path_bounds = paths_[path];
  for (int i = path_bounds.begin_index; i < path_bounds.end_index; ++i) {
    const ChainBounds chain_bounds = chains_[i];
    committed_nodes_.insert(committed_nodes_.end(),
                            committed_nodes_.data() + chain_bounds.begin_index,
                            committed_nodes_.data() + chain_bounds.end_index);
    if (committed_paths_[committed_nodes_.back()] == path) continue;
    for (int i = chain_bounds.begin_index; i < chain_bounds.end_index; ++i) {
      const int node = committed_nodes_[i];
      committed_paths_[node] = path;
    }
  }
}

// TODO(user): Instead of copying paths at the end systematically,
// reuse some of the memory when possible.
void PathState::IncrementalCommit() {
  const int new_nodes_begin = committed_nodes_.size();
  for (const int path : ChangedPaths()) {
    const int chain_begin = committed_nodes_.size();
    CopyNewPathAtEndOfNodes(path);
    const int chain_end = committed_nodes_.size();
    chains_[path] = {chain_begin, chain_end};
  }
  // Re-index all copied nodes.
  const int new_nodes_end = committed_nodes_.size();
  for (int i = new_nodes_begin; i < new_nodes_end; ++i) {
    const int node = committed_nodes_[i];
    committed_index_[node] = i;
  }
  // New loops stay in place: only change their path to -1,
  // committed_index_ does not change.
  for (const int loop : ChangedLoops()) {
    committed_paths_[loop] = -1;
  }
  // Committed part of the state is set up, erase incremental changes.
  Revert();
}

void PathState::FullCommit() {
  // Copy all paths at the end of committed_nodes_,
  // then remove all old committed_nodes_.
  const int old_num_nodes = committed_nodes_.size();
  for (int path = 0; path < num_paths_; ++path) {
    const int new_path_begin = committed_nodes_.size() - old_num_nodes;
    CopyNewPathAtEndOfNodes(path);
    const int new_path_end = committed_nodes_.size() - old_num_nodes;
    chains_[path] = {new_path_begin, new_path_end};
  }
  committed_nodes_.erase(committed_nodes_.begin(),
                         committed_nodes_.begin() + old_num_nodes);

  // Reindex path nodes, then loop nodes.
  constexpr int kUnindexed = -1;
  committed_index_.assign(num_nodes_, kUnindexed);
  int index = 0;
  for (const int node : committed_nodes_) {
    committed_index_[node] = index++;
  }
  for (int node = 0; node < num_nodes_; ++node) {
    if (committed_index_[node] != kUnindexed) continue;
    committed_index_[node] = index++;
    committed_nodes_.push_back(node);
    committed_paths_[node] = -1;
  }
  // Committed part of the state is set up, erase incremental changes.
  Revert();
}

namespace {

class PathStateFilter : public LocalSearchFilter {
 public:
  std::string DebugString() const override { return "PathStateFilter"; }
  PathStateFilter(std::unique_ptr<PathState> path_state,
                  const std::vector<IntVar*>& nexts);
  void Relax(const Assignment* delta, const Assignment* deltadelta) override;
  bool Accept(const Assignment*, const Assignment*, int64_t, int64_t) override {
    return true;
  }
  void Synchronize(const Assignment*, const Assignment*) override {};
  void Commit(const Assignment* assignment, const Assignment* delta) override;
  void Revert() override;
  void Reset() override;

 private:
  // Used in arc to chain translation, see below.
  struct TailHeadIndices {
    int tail_index;
    int head_index;
  };
  struct IndexArc {
    int index;
    int arc;
    bool operator<(const IndexArc& other) const { return index < other.index; }
  };

  // Translate changed_arcs_ to chains, pass to underlying PathState.
  void CutChains();
  // From changed_paths_ and changed_arcs_, fill chains_ and paths_.
  // Selection-based algorithm in O(n^2), to use for small change sets.
  void MakeChainsFromChangedPathsAndArcsWithSelectionAlgorithm();
  // From changed_paths_ and changed_arcs_, fill chains_ and paths_.
  // Generic algorithm in O(sort(n)+n), to use for larger change sets.
  void MakeChainsFromChangedPathsAndArcsWithGenericAlgorithm();

  const std::unique_ptr<PathState> path_state_;
  // Map IntVar* index to node, offset by the min index in nexts.
  std::vector<int> variable_index_to_node_;
  int index_offset_;
  // Used only in Reset(), class member status avoids reallocations.
  std::vector<bool> node_is_assigned_;
  std::vector<int> loops_;

  // Used in CutChains(), class member status avoids reallocations.
  std::vector<int> changed_paths_;
  std::vector<bool> path_has_changed_;
  std::vector<std::pair<int, int>> changed_arcs_;
  std::vector<int> changed_loops_;
  std::vector<TailHeadIndices> tail_head_indices_;
  std::vector<IndexArc> arcs_by_tail_index_;
  std::vector<IndexArc> arcs_by_head_index_;
  std::vector<int> next_arc_;
  std::vector<PathState::ChainBounds> path_chains_;
};

PathStateFilter::PathStateFilter(std::unique_ptr<PathState> path_state,
                                 const std::vector<IntVar*>& nexts)
    : path_state_(std::move(path_state)) {
  {
    int min_index = std::numeric_limits<int>::max();
    int max_index = std::numeric_limits<int>::min();
    for (const IntVar* next : nexts) {
      const int index = next->index();
      min_index = std::min<int>(min_index, index);
      max_index = std::max<int>(max_index, index);
    }
    variable_index_to_node_.resize(max_index - min_index + 1, -1);
    index_offset_ = min_index;
  }

  for (int node = 0; node < nexts.size(); ++node) {
    const int index = nexts[node]->index() - index_offset_;
    variable_index_to_node_[index] = node;
  }
  path_has_changed_.assign(path_state_->NumPaths(), false);
}

void PathStateFilter::Relax(const Assignment* delta, const Assignment*) {
  path_state_->Revert();
  changed_arcs_.clear();
  for (const IntVarElement& var_value : delta->IntVarContainer().elements()) {
    if (var_value.Var() == nullptr) continue;
    const int index = var_value.Var()->index() - index_offset_;
    if (index < 0 || variable_index_to_node_.size() <= index) continue;
    const int node = variable_index_to_node_[index];
    if (node == -1) continue;
    if (var_value.Bound()) {
      changed_arcs_.emplace_back(node, var_value.Value());
    } else {
      path_state_->Revert();
      path_state_->SetInvalid();
      return;
    }
  }
  CutChains();
}

void PathStateFilter::Reset() {
  path_state_->Revert();
  // Set all paths of path state to empty start -> end paths,
  // and all nonstart/nonend nodes to node -> node loops.
  const int num_nodes = path_state_->NumNodes();
  node_is_assigned_.assign(num_nodes, false);
  loops_.clear();
  const int num_paths = path_state_->NumPaths();
  for (int path = 0; path < num_paths; ++path) {
    const auto [start_index, end_index] = path_state_->CommittedPathRange(path);
    path_state_->ChangePath(
        path, {{start_index, start_index + 1}, {end_index - 1, end_index}});
    node_is_assigned_[path_state_->Start(path)] = true;
    node_is_assigned_[path_state_->End(path)] = true;
  }
  for (int node = 0; node < num_nodes; ++node) {
    if (!node_is_assigned_[node]) loops_.push_back(node);
  }
  path_state_->ChangeLoops(loops_);
  path_state_->Commit();
}

// The solver does not guarantee that a given Commit() corresponds to
// the previous Relax() (or that there has been a call to Relax()),
// so we replay the full change call sequence.
void PathStateFilter::Commit(const Assignment* assignment,
                             const Assignment* delta) {
  path_state_->Revert();
  if (delta == nullptr || delta->Empty()) {
    Relax(assignment, nullptr);
  } else {
    Relax(delta, nullptr);
  }
  path_state_->Commit();
}

void PathStateFilter::Revert() { path_state_->Revert(); }

void PathStateFilter::CutChains() {
  // Filter out unchanged arcs from changed_arcs_,
  // translate changed arcs to changed arc indices.
  // Fill changed_paths_ while we hold node_path.
  for (const int path : changed_paths_) path_has_changed_[path] = false;
  changed_paths_.clear();
  tail_head_indices_.clear();
  changed_loops_.clear();
  int num_changed_arcs = 0;
  for (const auto [node, next] : changed_arcs_) {
    const int node_index = path_state_->CommittedIndex(node);
    const int next_index = path_state_->CommittedIndex(next);
    const int node_path = path_state_->Path(node);
    if (next != node &&
        (next_index != node_index + 1 || node_path == -1)) {  // New arc.
      tail_head_indices_.push_back({node_index, next_index});
      changed_arcs_[num_changed_arcs++] = {node, next};
      if (node_path != -1 && !path_has_changed_[node_path]) {
        path_has_changed_[node_path] = true;
        changed_paths_.push_back(node_path);
      }
    } else if (node == next && node_path != -1) {  // New loop.
      changed_loops_.push_back(node);
    }
  }
  changed_arcs_.resize(num_changed_arcs);

  path_state_->ChangeLoops(changed_loops_);
  if (tail_head_indices_.size() + changed_paths_.size() <= 8) {
    MakeChainsFromChangedPathsAndArcsWithSelectionAlgorithm();
  } else {
    MakeChainsFromChangedPathsAndArcsWithGenericAlgorithm();
  }
}

void PathStateFilter::
    MakeChainsFromChangedPathsAndArcsWithSelectionAlgorithm() {
  int num_visited_changed_arcs = 0;
  const int num_changed_arcs = tail_head_indices_.size();
  // For every path, find all its chains.
  for (const int path : changed_paths_) {
    path_chains_.clear();
    const auto [start_index, end_index] = path_state_->CommittedPathRange(path);
    int current_index = start_index;
    while (true) {
      // Look for smallest non-visited tail_index that is no smaller than
      // current_index.
      int selected_arc = -1;
      int selected_tail_index = std::numeric_limits<int>::max();
      for (int i = num_visited_changed_arcs; i < num_changed_arcs; ++i) {
        const int tail_index = tail_head_indices_[i].tail_index;
        if (current_index <= tail_index && tail_index < selected_tail_index) {
          selected_arc = i;
          selected_tail_index = tail_index;
        }
      }
      // If there is no such tail index, or more generally if the next chain
      // would be cut by end of path,
      // stack {current_index, end_index + 1} in chains_, and go to next path.
      // Otherwise, stack {current_index, tail_index+1} in chains_,
      // set current_index = head_index, set pair to visited.
      if (start_index <= current_index && current_index < end_index &&
          end_index <= selected_tail_index) {
        path_chains_.emplace_back(current_index, end_index);
        break;
      } else {
        path_chains_.emplace_back(current_index, selected_tail_index + 1);
        current_index = tail_head_indices_[selected_arc].head_index;
        std::swap(tail_head_indices_[num_visited_changed_arcs],
                  tail_head_indices_[selected_arc]);
        ++num_visited_changed_arcs;
      }
    }
    path_state_->ChangePath(path, path_chains_);
  }
}

void PathStateFilter::MakeChainsFromChangedPathsAndArcsWithGenericAlgorithm() {
  // TRICKY: For each changed path, we want to generate a sequence of chains
  // that represents the path in the changed state.
  // First, notice that if we add a fake end->start arc for each changed path,
  // then all chains will be from the head of an arc to the tail of an arc.
  // A way to generate the changed chains and paths would be, for each path,
  // to start from a fake arc's head (the path start), go down the path until
  // the tail of an arc, and go to the next arc until we return on the fake arc,
  // enqueuing the [head, tail] chains as we go.
  // In turn, to do that, we need to know which arc to go to.
  // If we sort all heads and tails by index in two separate arrays,
  // the head_index and tail_index at the same rank are such that
  // [head_index, tail_index] is a chain. Moreover, the arc that must be visited
  // after head_index's arc is tail_index's arc.

  // Add a fake end->start arc for each path.
  for (const int path : changed_paths_) {
    const auto [start_index, end_index] = path_state_->CommittedPathRange(path);
    tail_head_indices_.push_back({end_index - 1, start_index});
  }

  // Generate pairs (tail_index, arc) and (head_index, arc) for all arcs,
  // sort those pairs by index.
  const int num_arc_indices = tail_head_indices_.size();
  arcs_by_tail_index_.resize(num_arc_indices);
  arcs_by_head_index_.resize(num_arc_indices);
  for (int i = 0; i < num_arc_indices; ++i) {
    arcs_by_tail_index_[i] = {tail_head_indices_[i].tail_index, i};
    arcs_by_head_index_[i] = {tail_head_indices_[i].head_index, i};
  }
  std::sort(arcs_by_tail_index_.begin(), arcs_by_tail_index_.end());
  std::sort(arcs_by_head_index_.begin(), arcs_by_head_index_.end());
  // Generate the map from arc to next arc in path.
  next_arc_.resize(num_arc_indices);
  for (int i = 0; i < num_arc_indices; ++i) {
    next_arc_[arcs_by_head_index_[i].arc] = arcs_by_tail_index_[i].arc;
  }

  // Generate chains: for every changed path, start from its fake arc,
  // jump to next_arc_ until going back to fake arc,
  // enqueuing chains as we go.
  const int first_fake_arc = num_arc_indices - changed_paths_.size();
  for (int fake_arc = first_fake_arc; fake_arc < num_arc_indices; ++fake_arc) {
    path_chains_.clear();
    int32_t arc = fake_arc;
    do {
      const int chain_begin = tail_head_indices_[arc].head_index;
      arc = next_arc_[arc];
      const int chain_end = tail_head_indices_[arc].tail_index + 1;
      path_chains_.emplace_back(chain_begin, chain_end);
    } while (arc != fake_arc);
    const int path = changed_paths_[fake_arc - first_fake_arc];
    path_state_->ChangePath(path, path_chains_);
  }
}

}  // namespace

LocalSearchFilter* MakePathStateFilter(Solver* solver,
                                       std::unique_ptr<PathState> path_state,
                                       const std::vector<IntVar*>& nexts) {
  PathStateFilter* filter = new PathStateFilter(std::move(path_state), nexts);
  return solver->RevAlloc(filter);
}

namespace {
using EInterval = DimensionChecker::ExtendedInterval;

constexpr int64_t kint64min = std::numeric_limits<int64_t>::min();
constexpr int64_t kint64max = std::numeric_limits<int64_t>::max();

EInterval operator&(const EInterval& i1, const EInterval& i2) {
  return {std::max(i1.num_negative_infinity == 0 ? i1.min : kint64min,
                   i2.num_negative_infinity == 0 ? i2.min : kint64min),
          std::min(i1.num_positive_infinity == 0 ? i1.max : kint64max,
                   i2.num_positive_infinity == 0 ? i2.max : kint64max),
          std::min(i1.num_negative_infinity, i2.num_negative_infinity),
          std::min(i1.num_positive_infinity, i2.num_positive_infinity)};
}

EInterval operator&=(EInterval& i1, const EInterval& i2) {
  i1 = i1 & i2;
  return i1;
}

bool IsEmpty(const EInterval& interval) {
  const int64_t minimum_value =
      interval.num_negative_infinity == 0 ? interval.min : kint64min;
  const int64_t maximum_value =
      interval.num_positive_infinity == 0 ? interval.max : kint64max;
  return minimum_value > maximum_value;
}

EInterval operator+(const EInterval& i1, const EInterval& i2) {
  return {CapAdd(i1.min, i2.min), CapAdd(i1.max, i2.max),
          i1.num_negative_infinity + i2.num_negative_infinity,
          i1.num_positive_infinity + i2.num_positive_infinity};
}

EInterval& operator+=(EInterval& i1, const EInterval& i2) {
  i1 = i1 + i2;
  return i1;
}

EInterval operator-(const EInterval& i1, const EInterval& i2) {
  return {CapSub(i1.min, i2.max), CapSub(i1.max, i2.min),
          i1.num_negative_infinity + i2.num_positive_infinity,
          i1.num_positive_infinity + i2.num_negative_infinity};
}

// Return the interval delta such that from + delta = to.
// Note that the result is not the same as "to + (-from)".
EInterval Delta(const EInterval& from, const EInterval& to) {
  return {CapSub(to.min, from.min), CapSub(to.max, from.max),
          to.num_negative_infinity - from.num_negative_infinity,
          to.num_positive_infinity - from.num_positive_infinity};
}

EInterval ToExtendedInterval(DimensionChecker::Interval interval) {
  const bool is_neg_infinity = interval.min == kint64min;
  const bool is_pos_infinity = interval.max == kint64max;
  return {is_neg_infinity ? 0 : interval.min,
          is_pos_infinity ? 0 : interval.max, is_neg_infinity ? 1 : 0,
          is_pos_infinity ? 1 : 0};
}

std::vector<EInterval> ToExtendedIntervals(
    absl::Span<const DimensionChecker::Interval> intervals) {
  std::vector<EInterval> extended_intervals;
  extended_intervals.reserve(intervals.size());
  for (const auto& interval : intervals) {
    extended_intervals.push_back(ToExtendedInterval(interval));
  }
  return extended_intervals;
}
}  // namespace

DimensionChecker::DimensionChecker(
    const PathState* path_state, std::vector<Interval> path_capacity,
    std::vector<int> path_class,
    std::vector<std::function<Interval(int64_t, int64_t)>>
        demand_per_path_class,
    std::vector<Interval> node_capacity, int min_range_size_for_riq)
    : path_state_(path_state),
      path_capacity_(ToExtendedIntervals(path_capacity)),
      path_class_(std::move(path_class)),
      demand_per_path_class_(std::move(demand_per_path_class)),
      node_capacity_(ToExtendedIntervals(node_capacity)),
      index_(path_state_->NumNodes(), 0),
      maximum_riq_layer_size_(std::max(
          16, 4 * path_state_->NumNodes())),  // 16 and 4 are arbitrary.
      min_range_size_for_riq_(min_range_size_for_riq) {
  const int num_nodes = path_state_->NumNodes();
  cached_demand_.resize(num_nodes);
  const int num_paths = path_state_->NumPaths();
  DCHECK_EQ(num_paths, path_capacity_.size());
  DCHECK_EQ(num_paths, path_class_.size());
  const int maximum_riq_exponent = MostSignificantBitPosition32(num_nodes);
  riq_.resize(maximum_riq_exponent + 1);
  FullCommit();
}

bool DimensionChecker::Check() const {
  if (path_state_->IsInvalid()) return true;
  for (const int path : path_state_->ChangedPaths()) {
    const EInterval path_capacity = path_capacity_[path];
    const int path_class = path_class_[path];
    // Loop invariant: except for the first chain, cumul represents the cumul
    // state of the last node of the previous chain, and it is nonempty.
    int prev_node = path_state_->Start(path);
    EInterval cumul = node_capacity_[prev_node] & path_capacity;
    if (IsEmpty(cumul)) return false;

    for (const auto chain : path_state_->Chains(path)) {
      const int first_node = chain.First();
      const int last_node = chain.Last();

      if (prev_node != first_node) {
        // Bring cumul state from last node of previous chain to first node of
        // current chain.
        const EInterval demand = ToExtendedInterval(
            demand_per_path_class_[path_class](prev_node, first_node));
        cumul += demand;
        cumul &= path_capacity;
        cumul &= node_capacity_[first_node];
        if (IsEmpty(cumul)) return false;
        prev_node = first_node;
      }

      // Bring cumul state from first node to last node of the current chain.
      const int first_index = index_[first_node];
      const int last_index = index_[last_node];
      const int chain_path = path_state_->Path(first_node);
      const int chain_path_class =
          chain_path == -1 ? -1 : path_class_[chain_path];
      // Use a RIQ if the chain size is large enough;
      // the optimal size was found with the associated benchmark in tests,
      // in particular BM_DimensionChecker<ChangeSparsity::kSparse, *>.
      const bool chain_is_cached = chain_path_class == path_class;
      if (last_index - first_index > min_range_size_for_riq_ &&
          chain_is_cached) {
        UpdateCumulUsingChainRIQ(first_index, last_index, path_capacity, cumul);
        if (IsEmpty(cumul)) return false;
        prev_node = chain.Last();
      } else {
        for (const int node : chain.WithoutFirstNode()) {
          const EInterval demand =
              chain_is_cached
                  ? cached_demand_[prev_node]
                  : ToExtendedInterval(
                        demand_per_path_class_[path_class](prev_node, node));
          cumul += demand;
          cumul &= node_capacity_[node];
          cumul &= path_capacity;
          if (IsEmpty(cumul)) return false;
          prev_node = node;
        }
      }
    }
  }
  return true;
}

void DimensionChecker::Commit() {
  const int current_layer_size = riq_[0].size();
  int change_size = path_state_->ChangedPaths().size();
  for (const int path : path_state_->ChangedPaths()) {
    for (const auto chain : path_state_->Chains(path)) {
      change_size += chain.NumNodes();
    }
  }
  if (current_layer_size + change_size <= maximum_riq_layer_size_) {
    IncrementalCommit();
  } else {
    FullCommit();
  }
}

void DimensionChecker::IncrementalCommit() {
  for (const int path : path_state_->ChangedPaths()) {
    const int begin_index = riq_[0].size();
    AppendPathDemandsToSums(path);
    UpdateRIQStructure(begin_index, riq_[0].size());
  }
}

void DimensionChecker::FullCommit() {
  // Clear all structures.
  for (auto& layer : riq_) layer.clear();
  // Append all paths.
  const int num_paths = path_state_->NumPaths();
  for (int path = 0; path < num_paths; ++path) {
    const int begin_index = riq_[0].size();
    AppendPathDemandsToSums(path);
    UpdateRIQStructure(begin_index, riq_[0].size());
  }
}

void DimensionChecker::AppendPathDemandsToSums(int path) {
  // Value of forwards_demand_sums_riq_ at node_index must be the sum
  // of all demands of nodes from start of path to node.
  const int path_class = path_class_[path];
  EInterval demand_sum = {0, 0, 0, 0};
  int prev = path_state_->Start(path);
  int index = riq_[0].size();
  for (const int node : path_state_->Nodes(path)) {
    // Transition to current node.
    const EInterval demand =
        prev == node ? EInterval{0, 0, 0, 0}
                     : ToExtendedInterval(
                           demand_per_path_class_[path_class](prev, node));
    demand_sum += demand;
    cached_demand_[prev] = demand;
    prev = node;
    // Store all data of current node.
    index_[node] = index++;
    riq_[0].push_back({.cumuls_to_fst = node_capacity_[node],
                       .tightest_tsum = demand_sum,
                       .cumuls_to_lst = node_capacity_[node],
                       .tsum_at_fst = demand_sum,
                       .tsum_at_lst = demand_sum});
  }
  cached_demand_[path_state_->End(path)] = {0, 0, 0, 0};
}

void DimensionChecker::UpdateRIQStructure(int begin_index, int end_index) {
  // The max layer is the one used by Range Intersection Query functions on
  // (begin_index, end_index - 1).
  const int max_layer =
      MostSignificantBitPosition32(end_index - begin_index - 1);
  for (int layer = 1, half_window = 1; layer <= max_layer;
       ++layer, half_window *= 2) {
    riq_[layer].resize(end_index);
    for (int i = begin_index + 2 * half_window - 1; i < end_index; ++i) {
      // The window covered by riq_[layer][i] goes from
      // first = i - 2 * half_window + 1 to last = i, inclusive.
      // Values are computed from two half-windows of the layer below,
      // the F-window = (i - 2 * half_window, i - half_window], and
      // the L-window - (i - half_window, i].
      const RIQNode& fw = riq_[layer - 1][i - half_window];
      const RIQNode& lw = riq_[layer - 1][i];
      const EInterval lst_to_lst = Delta(fw.tsum_at_lst, lw.tsum_at_lst);
      const EInterval fst_to_fst = Delta(fw.tsum_at_fst, lw.tsum_at_fst);

      riq_[layer][i] = {
          .cumuls_to_fst = fw.cumuls_to_fst & lw.cumuls_to_fst - fst_to_fst,
          .tightest_tsum = fw.tightest_tsum & lw.tightest_tsum,
          .cumuls_to_lst = fw.cumuls_to_lst + lst_to_lst & lw.cumuls_to_lst,
          .tsum_at_fst = fw.tsum_at_fst,
          .tsum_at_lst = lw.tsum_at_lst};
    }
  }
}

// The RIQ schema decomposes the request into two windows:
// - the F window covers indices [first_index, first_index + window)
// - the L window covers indices (last_index - window, last_index]
// The decomposition uses the first and last nodes of these windows.
void DimensionChecker::UpdateCumulUsingChainRIQ(
    int first_index, int last_index, const ExtendedInterval& path_capacity,
    ExtendedInterval& cumul) const {
  DCHECK_LE(0, first_index);
  DCHECK_LT(first_index, last_index);
  DCHECK_LT(last_index, riq_[0].size());
  const int layer = MostSignificantBitPosition32(last_index - first_index);
  const int window = 1 << layer;
  const RIQNode& fw = riq_[layer][first_index + window - 1];
  const RIQNode& lw = riq_[layer][last_index];

  // Compute the set of cumul values that can reach the last node.
  cumul &= fw.cumuls_to_fst;
  cumul &= lw.cumuls_to_fst - Delta(fw.tsum_at_fst, lw.tsum_at_fst);
  cumul &= path_capacity -
           Delta(fw.tsum_at_fst, fw.tightest_tsum & lw.tightest_tsum);

  // We need to check for emptiness before widening the interval with transit.
  if (IsEmpty(cumul)) return;

  // Transit to last node.
  cumul += Delta(fw.tsum_at_fst, lw.tsum_at_lst);

  // Compute the set of cumul values that are reached from first node.
  cumul &= fw.cumuls_to_lst + Delta(fw.tsum_at_lst, lw.tsum_at_lst);
  cumul &= lw.cumuls_to_lst;
}

namespace {

class DimensionFilter : public LocalSearchFilter {
 public:
  std::string DebugString() const override { return name_; }
  DimensionFilter(std::unique_ptr<DimensionChecker> checker,
                  absl::string_view dimension_name)
      : checker_(std::move(checker)),
        name_(absl::StrCat("DimensionFilter(", dimension_name, ")")) {}

  bool Accept(const Assignment*, const Assignment*, int64_t, int64_t) override {
    return checker_->Check();
  }

  void Synchronize(const Assignment*, const Assignment*) override {
    checker_->Commit();
  }

 private:
  std::unique_ptr<DimensionChecker> checker_;
  const std::string name_;
};

}  // namespace

LocalSearchFilter* MakeDimensionFilter(
    Solver* solver, std::unique_ptr<DimensionChecker> checker,
    absl::string_view dimension_name) {
  DimensionFilter* filter =
      new DimensionFilter(std::move(checker), dimension_name);
  return solver->RevAlloc(filter);
}

LightVehicleBreaksChecker::LightVehicleBreaksChecker(
    PathState* path_state, std::vector<PathData> path_data)
    : path_state_(path_state), path_data_(std::move(path_data)) {}

void LightVehicleBreaksChecker::Relax() const {
  for (const int path : path_state_->ChangedPaths()) {
    path_data_[path].start_cumul.Relax();
    path_data_[path].end_cumul.Relax();
    path_data_[path].span.Relax();
  }
}

bool LightVehicleBreaksChecker::Check() const {
  for (const int path : path_state_->ChangedPaths()) {
    if (!path_data_[path].span.Exists()) continue;
    const PathData& data = path_data_[path];
    const int64_t total_transit = data.total_transit.Min();
    int64_t lb_span = data.span.Min();
    // Improve bounds on span/start max/end min using time windows: breaks that
    // must occur inside the path have their duration accumulated into
    // lb_span_tw, they also widen [start_max, end_min).
    int64_t lb_span_tw = total_transit;
    int64_t start_max = data.start_cumul.Max();
    int64_t end_min = data.end_cumul.Min();
    for (const auto& br : data.vehicle_breaks) {
      if (!br.is_performed_min) continue;
      if (br.start_max < end_min && start_max < br.end_min) {
        CapAddTo(br.duration_min, &lb_span_tw);
        start_max = std::min(start_max, br.start_max);
        end_min = std::max(end_min, br.end_min);
      }
    }
    lb_span = std::max({lb_span, lb_span_tw, CapSub(end_min, start_max)});
    // Compute num_feasible_breaks = number of breaks that may fit into route,
    // and [breaks_start_min, breaks_end_max) = max coverage of breaks.
    int64_t break_start_min = kint64max;
    int64_t break_end_max = kint64min;
    int64_t start_min = data.start_cumul.Min();
    start_min = std::max(start_min, CapSub(end_min, data.span.Max()));
    int64_t end_max = data.end_cumul.Max();
    end_max = std::min(end_max, CapAdd(start_max, data.span.Max()));
    int num_feasible_breaks = 0;
    for (const auto& br : data.vehicle_breaks) {
      if (start_min <= br.start_max && br.end_min <= end_max) {
        break_start_min = std::min(break_start_min, br.start_min);
        break_end_max = std::max(break_end_max, br.end_max);
        ++num_feasible_breaks;
      }
    }
    // Improve span/start min/end max using interbreak limits: there must be
    // enough breaks inside the path, so that for each limit, the union of
    // [br.start - max_interbreak, br.end + max_interbreak) covers [start, end),
    // or [start, end) is shorter than max_interbreak.
    for (const auto& [max_interbreak, min_break_duration] :
         data.interbreak_limits) {
      // Minimal number of breaks depends on total transit:
      // 0 breaks for 0 <= total transit <= limit,
      // 1 break for limit + 1 <= total transit <= 2 * limit,
      // i breaks for i * limit + 1 <= total transit <= (i+1) * limit, ...
      if (max_interbreak == 0) {
        if (total_transit > 0) return false;
        continue;
      }
      int64_t min_num_breaks =
          std::max<int64_t>(0, (total_transit - 1) / max_interbreak);
      if (lb_span > max_interbreak) {
        min_num_breaks = std::max<int64_t>(min_num_breaks, 1);
      }
      if (min_num_breaks > num_feasible_breaks) return false;
      lb_span = std::max(
          lb_span,
          CapAdd(total_transit, CapProd(min_num_breaks, min_break_duration)));
      if (min_num_breaks > 0) {
        if (!data.start_cumul.SetMin(CapSub(break_start_min, max_interbreak))) {
          return false;
        }
        if (!data.end_cumul.SetMax(CapAdd(break_end_max, max_interbreak))) {
          return false;
        }
      }
    }
    if (!data.span.SetMin(lb_span)) return false;
    // Merge span lb information directly in start/end variables.
    start_max = std::min(start_max, CapSub(end_max, lb_span));
    if (!data.start_cumul.SetMax(start_max)) return false;
    end_min = std::max(end_min, CapAdd(start_min, lb_span));
    if (!data.end_cumul.SetMin(end_min)) return false;
  }
  return true;
}

namespace {

class LightVehicleBreaksFilter : public LocalSearchFilter {
 public:
  LightVehicleBreaksFilter(std::unique_ptr<LightVehicleBreaksChecker> checker,
                           absl::string_view dimension_name)
      : checker_(std::move(checker)),
        name_(absl::StrCat("LightVehicleBreaksFilter(", dimension_name, ")")) {}

  std::string DebugString() const override { return name_; }

  void Relax(const Assignment*, const Assignment*) override {
    checker_->Relax();
  }

  bool Accept(const Assignment*, const Assignment*, int64_t, int64_t) override {
    return checker_->Check();
  }

  void Synchronize(const Assignment*, const Assignment*) override {
    checker_->Check();
  }

 private:
  std::unique_ptr<LightVehicleBreaksChecker> checker_;
  const std::string name_;
};

}  // namespace

LocalSearchFilter* MakeLightVehicleBreaksFilter(
    Solver* solver, std::unique_ptr<LightVehicleBreaksChecker> checker,
    absl::string_view dimension_name) {
  LightVehicleBreaksFilter* filter =
      new LightVehicleBreaksFilter(std::move(checker), dimension_name);
  return solver->RevAlloc(filter);
}

void WeightedWaveletTree::Clear() {
  elements_.clear();
  tree_location_.clear();
  nodes_.clear();
  for (auto& layer : tree_layers_) layer.clear();
}

void WeightedWaveletTree::MakeTreeFromNewElements() {
  // New elements are elements_[i] for i in [begin_index, end_index).
  const int begin_index = tree_location_.size();
  const int end_index = elements_.size();
  DCHECK_LE(begin_index, end_index);
  if (begin_index >= end_index) return;
  // Gather all heights, sort and unique them, this makes up the list of
  // pivot heights of the underlying tree, with an inorder traversal.
  // TODO(user): investigate whether balancing the tree using the
  // number of occurrences of each height would be beneficial.
  // TODO(user): use a heap-like encoding for the binary search tree:
  // children of i at 2*i and 2*i+1. Better cache line utilization.
  const int old_node_size = nodes_.size();
  for (int i = begin_index; i < end_index; ++i) {
    nodes_.push_back({.pivot_height = elements_[i].height, .pivot_index = -1});
  }
  std::sort(nodes_.begin() + old_node_size, nodes_.end());
  nodes_.erase(std::unique(nodes_.begin() + old_node_size, nodes_.end()),
               nodes_.end());

  // Remember location of the tree representation for this range of elements.
  // tree_location_ may be smaller than elements_, extend it if needed.
  const int new_node_size = nodes_.size();
  tree_location_.resize(end_index, {.node_begin = old_node_size,
                                    .node_end = new_node_size,
                                    .sequence_first = begin_index});

  // Add and extend layers if needed.
  // The amount of layers needed is 1 + ceil(log(sequence size)).
  const int num_layers =
      2 + MostSignificantBitPosition32(new_node_size - old_node_size - 1);
  if (tree_layers_.size() <= num_layers) tree_layers_.resize(num_layers);
  for (int l = 0; l < num_layers; ++l) {
    tree_layers_[l].resize(end_index,
                           {.prefix_sum = 0, .left_index = -1, .is_left = 0});
  }

  // Fill all relevant locations of the tree, and record tree navigation
  // information. This recursive function has at most num_layers call depth.
  const auto fill_subtree = [this](auto& fill_subtree, int layer,
                                   int node_begin, int node_end,
                                   int range_begin, int range_end) {
    DCHECK_LT(node_begin, node_end);
    DCHECK_LT(range_begin, range_end);
    // Precompute prefix sums of range [range_begin, range_end).
    int64_t sum = 0;
    for (int i = range_begin; i < range_end; ++i) {
      sum += elements_[i].weight;
      tree_layers_[layer][i].prefix_sum = sum;
    }
    if (node_begin + 1 == node_end) return;
    // Range has more than one height, partition it.
    // Record layer l -> l+1 sequence index mapping:
    // - if height < pivot, record where this element will be in layer l+1.
    // - if height >= pivot, record where next <= pivot will be in layer l+1.
    const int node_mid = node_begin + (node_end - node_begin) / 2;
    const int64_t pivot_height = nodes_[node_mid].pivot_height;
    int pivot_index = range_begin;
    for (int i = range_begin; i < range_end; ++i) {
      tree_layers_[layer][i].left_index = pivot_index;
      tree_layers_[layer][i].is_left = elements_[i].height < pivot_height;
      if (elements_[i].height < pivot_height) ++pivot_index;
    }
    nodes_[node_mid].pivot_index = pivot_index;
    // TODO(user): stable_partition allocates memory,
    // find a way to fill layers without this.
    std::stable_partition(
        elements_.begin() + range_begin, elements_.begin() + range_end,
        [pivot_height](const auto& el) { return el.height < pivot_height; });

    fill_subtree(fill_subtree, layer + 1, node_begin, node_mid, range_begin,
                 pivot_index);
    fill_subtree(fill_subtree, layer + 1, node_mid, node_end, pivot_index,
                 range_end);
  };
  fill_subtree(fill_subtree, 0, old_node_size, new_node_size, begin_index,
               end_index);
}

int64_t WeightedWaveletTree::RangeSumWithThreshold(int64_t threshold_height,
                                                   int begin_index,
                                                   int end_index) const {
  DCHECK_LE(begin_index, end_index);  // Range can be empty, but not reversed.
  DCHECK_LE(end_index, tree_location_.size());
  DCHECK_EQ(tree_location_.size(), elements_.size());  // No pending elements.
  if (begin_index >= end_index) return 0;
  auto [node_begin, node_end, sequence_first_index] =
      tree_location_[begin_index];
  DCHECK_EQ(tree_location_[end_index - 1].sequence_first,
            sequence_first_index);  // Range is included in a single sequence.
  ElementRange range{
      .range_first_index = begin_index,
      .range_last_index = end_index - 1,
      .range_first_is_node_first = begin_index == sequence_first_index};
  // Answer in O(1) for the common case where max(heights) < threshold.
  if (nodes_[node_end - 1].pivot_height < threshold_height) return 0;

  int64_t sum = 0;
  int64_t min_height_of_current_node = nodes_[node_begin].pivot_height;
  for (int l = 0; !range.Empty(); ++l) {
    const ElementInfo* elements = tree_layers_[l].data();
    if (threshold_height <= min_height_of_current_node) {
      // Query or subquery threshold covers all elements of this node.
      // This allows to be O(1) when the query's threshold is <= min(heights).
      sum += range.Sum(elements);
      return sum;
    } else if (node_begin + 1 == node_end) {
      // This node is a leaf, its height is < threshold, stop descent here.
      return sum;
    }

    const int node_mid = node_begin + (node_end - node_begin) / 2;
    const auto [pivot_height, pivot_index] = nodes_[node_mid];
    const ElementRange right = range.RightSubRange(elements, pivot_index);
    if (threshold_height < pivot_height) {
      // All elements of the right child have their height above the threshold,
      // we can project the range to the right child and add the whole subrange.
      if (!right.Empty()) sum += right.Sum(tree_layers_[l + 1].data());
      // Go to the left child.
      range = range.LeftSubRange(elements);
      node_end = node_mid;
    } else {
      // Go to the right child.
      range = right;
      node_begin = node_mid;
      min_height_of_current_node = pivot_height;
    }
  }
  return sum;
}

PathEnergyCostChecker::PathEnergyCostChecker(
    const PathState* path_state, std::vector<int64_t> force_start_min,
    std::vector<int64_t> force_end_min, std::vector<int> force_class,
    std::vector<const std::function<int64_t(int64_t)>*> force_per_class,
    std::vector<int> distance_class,
    std::vector<const std::function<int64_t(int64_t, int64_t)>*>
        distance_per_class,
    std::vector<EnergyCost> path_energy_cost,
    std::vector<bool> path_has_cost_when_empty)
    : path_state_(path_state),
      force_start_min_(std::move(force_start_min)),
      force_end_min_(std::move(force_end_min)),
      force_class_(std::move(force_class)),
      distance_class_(std::move(distance_class)),
      force_per_class_(std::move(force_per_class)),
      distance_per_class_(std::move(distance_per_class)),
      path_energy_cost_(std::move(path_energy_cost)),
      path_has_cost_when_empty_(std::move(path_has_cost_when_empty)),
      maximum_range_query_size_(4 * path_state_->NumNodes()),
      force_rmq_index_of_node_(path_state_->NumNodes()),
      threshold_query_index_of_node_(path_state_->NumNodes()) {
  const int num_nodes = path_state_->NumNodes();
  cached_force_.resize(num_nodes);
  cached_distance_.resize(num_nodes);
  FullCacheAndPrecompute();
  committed_total_cost_ = 0;
  committed_path_cost_.assign(path_state_->NumPaths(), 0);
  const int num_paths = path_state_->NumPaths();
  for (int path = 0; path < num_paths; ++path) {
    committed_path_cost_[path] = ComputePathCost(path);
    CapAddTo(committed_path_cost_[path], &committed_total_cost_);
  }
  accepted_total_cost_ = committed_total_cost_;
}

bool PathEnergyCostChecker::Check() {
  if (path_state_->IsInvalid()) return true;
  accepted_total_cost_ = committed_total_cost_;
  for (const int path : path_state_->ChangedPaths()) {
    accepted_total_cost_ =
        CapSub(accepted_total_cost_, committed_path_cost_[path]);
    CapAddTo(ComputePathCost(path), &accepted_total_cost_);
    if (accepted_total_cost_ == kint64max) return false;
  }
  return true;
}

void PathEnergyCostChecker::CacheAndPrecomputeRangeQueriesOfPath(int path) {
  // Cache force and distance evaluations,
  // precompute force RMQ, energy/distance threshold queries.
  const auto& force_evaluator = *force_per_class_[force_class_[path]];
  const auto& distance_evaluator = *distance_per_class_[distance_class_[path]];
  int force_index = force_rmq_.TableSize();
  int threshold_index = energy_query_.TreeSize();
  int64_t total_force = 0;

  const int start_node = path_state_->Start(path);
  int prev_node = start_node;

  for (const int node : path_state_->Nodes(path)) {
    if (prev_node != node) {
      const int64_t distance = distance_evaluator(prev_node, node);
      cached_distance_[prev_node] = distance;
      energy_query_.PushBack(total_force, total_force * distance);
      distance_query_.PushBack(total_force, distance);
      prev_node = node;
    }
    threshold_query_index_of_node_[node] = threshold_index++;
    force_rmq_.PushBack(total_force);
    force_rmq_index_of_node_[node] = force_index++;
    const int64_t force = force_evaluator(node);
    cached_force_[node] = force;
    total_force += force;
  }
  force_rmq_.MakeTableFromNewElements();
  energy_query_.MakeTreeFromNewElements();
  distance_query_.MakeTreeFromNewElements();
}

void PathEnergyCostChecker::IncrementalCacheAndPrecompute() {
  for (const int path : path_state_->ChangedPaths()) {
    CacheAndPrecomputeRangeQueriesOfPath(path);
  }
}

void PathEnergyCostChecker::FullCacheAndPrecompute() {
  force_rmq_.Clear();
  // Rebuild all paths.
  const int num_paths = path_state_->NumPaths();
  for (int path = 0; path < num_paths; ++path) {
    CacheAndPrecomputeRangeQueriesOfPath(path);
  }
}

void PathEnergyCostChecker::Commit() {
  int change_size = path_state_->ChangedPaths().size();
  for (const int path : path_state_->ChangedPaths()) {
    for (const auto chain : path_state_->Chains(path)) {
      change_size += chain.NumNodes();
    }
    committed_total_cost_ =
        CapSub(committed_total_cost_, committed_path_cost_[path]);
    committed_path_cost_[path] = ComputePathCost(path);
    CapAddTo(committed_path_cost_[path], &committed_total_cost_);
  }

  const int current_layer_size = force_rmq_.TableSize();
  if (current_layer_size + change_size <= maximum_range_query_size_) {
    IncrementalCacheAndPrecompute();
  } else {
    FullCacheAndPrecompute();
  }
}

int64_t PathEnergyCostChecker::ComputePathCost(int64_t path) const {
  const int path_force_class = force_class_[path];
  const auto& force_evaluator = *force_per_class_[path_force_class];

  // Find minimal force at which to start.
  int64_t total_force = force_start_min_[path];
  int64_t min_force = total_force;
  int num_path_nodes = 0;
  int prev_node = path_state_->Start(path);
  for (const auto chain : path_state_->Chains(path)) {
    num_path_nodes += chain.NumNodes();
    // Add force needed to go from prev_node to chain.First() if needed.
    if (chain.First() != prev_node) {
      const int64_t force_to_node = force_evaluator(prev_node);
      CapAddTo(force_to_node, &total_force);
      min_force = std::min(min_force, total_force);
      prev_node = chain.First();
    }

    // Add force needed to go from chain.First() to chain.Last().
    const int chain_path = path_state_->Path(chain.First());
    const int chain_force_class =
        chain_path == -1 ? -1 : force_class_[chain_path];
    const bool force_is_cached = chain_force_class == path_force_class;
    if (force_is_cached && chain.NumNodes() >= 2) {
      const int first_index = force_rmq_index_of_node_[chain.First()];
      const int last_index = force_rmq_index_of_node_[chain.Last()];
      // Get total force at first, last and lowest point of the chain.
      const int64_t first_total_force = force_rmq_.array()[first_index];
      const int64_t last_total_force = force_rmq_.array()[last_index];
      const int64_t min_total_force =
          force_rmq_.RangeMinimum(first_index, last_index);
      // Compute running minimum total force and total force at chain.Last().
      min_force = std::min(min_force,
                           total_force - first_total_force + min_total_force);
      CapAddTo(last_total_force - first_total_force, &total_force);
      prev_node = chain.Last();
    } else {
      for (const int node : chain.WithoutFirstNode()) {
        const int64_t force = force_is_cached ? cached_force_[prev_node]
                                              : force_evaluator(prev_node);
        CapAddTo(force, &total_force);
        min_force = std::min(min_force, total_force);
        prev_node = node;
      }
    }
  }
  if (num_path_nodes == 2 && !path_has_cost_when_empty_[path]) return 0;
  // Force must be offset in order to be all of:
  // - >= force_start_min_[path] at start
  // - >= force_end_min_[path] at end
  // - >= 0 at all intermediate nodes
  // We set the accumulator to the minimal offset that allows this.
  total_force = std::max<int64_t>(
      {0, CapOpp(min_force), CapSub(force_end_min_[path], total_force)});
  CapAddTo(force_start_min_[path], &total_force);

  // Compute energy, below and above threshold.
  const int path_distance_class = distance_class_[path];
  const auto& distance_evaluator = *distance_per_class_[path_distance_class];
  const EnergyCost& cost = path_energy_cost_[path];
  int64_t energy_below = 0;
  int64_t energy_above = 0;
  prev_node = path_state_->Start(path);
  for (const auto chain : path_state_->Chains(path)) {
    // Bring cost computation to first node of the chain.
    if (chain.First() != prev_node) {
      const int64_t distance = distance_evaluator(prev_node, chain.First());
      CapAddTo(force_evaluator(prev_node), &total_force);
      CapAddTo(CapProd(std::min(cost.threshold, total_force), distance),
               &energy_below);
      const int64_t force_above =
          std::max<int64_t>(0, CapSub(total_force, cost.threshold));
      CapAddTo(CapProd(force_above, distance), &energy_above);
      prev_node = chain.First();
    }

    // Inside chain, try to reuse cached forces and distances instead of more
    // costly calls to evaluators.
    const int chain_path = path_state_->Path(chain.First());
    const int chain_force_class =
        chain_path == -1 ? -1 : force_class_[chain_path];
    const int chain_distance_class =
        chain_path == -1 ? -1 : distance_class_[chain_path];
    const bool force_is_cached = chain_force_class == path_force_class;
    const bool distance_is_cached = chain_distance_class == path_distance_class;

    if (force_is_cached && distance_is_cached && chain.NumNodes() >= 2) {
      const int first_index = threshold_query_index_of_node_[chain.First()];
      const int last_index = threshold_query_index_of_node_[chain.Last()];

      const int64_t zero_total_energy = energy_query_.RangeSumWithThreshold(
          kint64min, first_index, last_index);
      const int64_t total_distance = distance_query_.RangeSumWithThreshold(
          kint64min, first_index, last_index);

      // In the following, zero_ values are those computed with the hypothesis
      // that the force at the start node is zero.
      // The total_force at chain.First() is in general not the same in the
      // candidate path and in the zero_ case. We can still query the energy and
      // distance totals incurred by transitions above the actual threshold
      // during the chain, by offsetting the queries to zero_threshold.
      const int64_t zero_total_force_first =
          force_rmq_.array()[force_rmq_index_of_node_[chain.First()]];
      const int64_t zero_threshold =
          CapSub(cost.threshold, CapSub(total_force, zero_total_force_first));
      // "High" transitions are those that occur with a force at or above the
      // threshold. "High" energy is the sum of energy values during high
      // transitions, same for "high" distance.
      const int64_t zero_high_energy = energy_query_.RangeSumWithThreshold(
          zero_threshold, first_index, last_index);
      const int64_t zero_high_distance = distance_query_.RangeSumWithThreshold(
          zero_threshold, first_index, last_index);
      // "Above" energy is the energy caused by total_force above the threshold.
      // Since "above" energy is only incurred during "high" transitions, it can
      // be computed from "high" energy knowing distance and threshold.
      const int64_t zero_energy_above =
          CapSub(zero_high_energy, CapProd(zero_high_distance, zero_threshold));
      // To compute the energy values of the candidate, the force dimension
      // must be offset back to the candidate's total force.
      // Only the "below" energy is changed by the offset, the zero_ energy
      // above the zero_ threshold was computed to be the same as the candidate
      // energy above the actual threshold.
      CapAddTo(zero_energy_above, &energy_above);
      CapAddTo(CapAdd(CapSub(zero_total_energy, zero_energy_above),
                      CapProd(total_distance,
                              CapSub(cost.threshold, zero_threshold))),
               &energy_below);
      // We reuse the partial sum  of the force query to compute the sum of
      // forces incurred by the chain,
      const int64_t zero_total_force_last =
          force_rmq_.array()[force_rmq_index_of_node_[chain.Last()]];
      CapAddTo(CapSub(zero_total_force_last, zero_total_force_first),
               &total_force);
      prev_node = chain.Last();
    } else {
      for (const int node : chain.WithoutFirstNode()) {
        const int64_t force = force_is_cached ? cached_force_[prev_node]
                                              : force_evaluator(prev_node);
        const int64_t distance = distance_is_cached
                                     ? cached_distance_[prev_node]
                                     : distance_evaluator(prev_node, node);
        CapAddTo(force, &total_force);
        CapAddTo(CapProd(std::min(cost.threshold, total_force), distance),
                 &energy_below);
        const int64_t force_above =
            std::max<int64_t>(0, CapSub(total_force, cost.threshold));
        CapAddTo(CapProd(force_above, distance), &energy_above);
        prev_node = node;
      }
    }
  }

  return CapAdd(CapProd(energy_below, cost.cost_per_unit_below_threshold),
                CapProd(energy_above, cost.cost_per_unit_above_threshold));
}

namespace {

class PathEnergyCostFilter : public LocalSearchFilter {
 public:
  std::string DebugString() const override { return name_; }
  PathEnergyCostFilter(std::unique_ptr<PathEnergyCostChecker> checker,
                       absl::string_view energy_name)
      : checker_(std::move(checker)),
        name_(absl::StrCat("PathEnergyCostFilter(", energy_name, ")")) {}

  bool Accept(const Assignment*, const Assignment*, int64_t objective_min,
              int64_t objective_max) override {
    if (objective_max > kint64max / 2) return true;
    if (!checker_->Check()) return false;
    const int64_t cost = checker_->AcceptedCost();
    return objective_min <= cost && cost <= objective_max;
  }

  void Synchronize(const Assignment*, const Assignment*) override {
    checker_->Commit();
  }

  int64_t GetSynchronizedObjectiveValue() const override {
    return checker_->CommittedCost();
  }
  int64_t GetAcceptedObjectiveValue() const override {
    return checker_->AcceptedCost();
  }

 private:
  std::unique_ptr<PathEnergyCostChecker> checker_;
  const std::string name_;
};

}  // namespace

LocalSearchFilter* MakePathEnergyCostFilter(
    Solver* solver, std::unique_ptr<PathEnergyCostChecker> checker,
    absl::string_view dimension_name) {
  PathEnergyCostFilter* filter =
      new PathEnergyCostFilter(std::move(checker), dimension_name);
  return solver->RevAlloc(filter);
}

// TODO(user): Implement same-vehicle filter. Could be merged with node
// precedence filter.

}  // namespace operations_research::routing
