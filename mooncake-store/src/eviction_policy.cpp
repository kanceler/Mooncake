#include "eviction_policy.h"

#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace mooncake {

size_t ObjectDataTypeIndex(ObjectDataType data_type) {
    const auto index = static_cast<size_t>(data_type);
    if (index < kObjectDataTypeCount) {
        return index;
    }
    return static_cast<size_t>(ObjectDataType::UNKNOWN);
}

bool ValidateObjectTypeEvictionScorePolicy(
    const ObjectTypeEvictionScorePolicy& policy) {
    if (!std::isfinite(policy.reuse_scale) || policy.reuse_scale <= 0.0) {
        LOG(WARNING) << "reuse_scale must be finite and greater than 0";
        return false;
    }
    if (!std::isfinite(policy.soft_pin_weight) ||
        policy.soft_pin_weight < 0.0) {
        LOG(WARNING) << "soft_pin_weight must be finite and non-negative";
        return false;
    }
    if (policy.eviction_grace_ms < 0) {
        LOG(WARNING) << "eviction_grace_ms must be non-negative";
        return false;
    }
    return true;
}

bool ValidateObjectTypeEvictionPolicy(const ObjectTypeEvictionPolicy& policy) {
    if (!std::isfinite(policy.budget_ratio) || policy.budget_ratio < 0.0 ||
        policy.budget_ratio > 1.0) {
        LOG(WARNING) << "budget_ratio must be finite and between 0.0 and 1.0";
        return false;
    }
    return true;
}

EvictionScope EvictionScope::All(const EvictionCandidateView& view) {
    EvictionScope scope;
    scope.refs.resize(view.candidates.size());
    std::iota(scope.refs.begin(), scope.refs.end(), 0);
    scope.eviction_base = view.total_eviction_base;
    return scope;
}

TypeAwareEvictionPolicy::TypeAwareEvictionPolicy() = default;

TypeAwareEvictionPolicy::TypeAwareEvictionPolicy(
    std::array<ObjectTypeEvictionScorePolicy, kObjectDataTypeCount>
        score_policies,
    std::array<ObjectTypeEvictionPolicy, kObjectDataTypeCount> policies)
    : score_policies_(std::move(score_policies)),
      policies_(std::move(policies)) {
    for (size_t i = 0; i < kObjectDataTypeCount; ++i) {
        if (!ValidateObjectTypeEvictionScorePolicy(score_policies_[i])) {
            score_policies_[i] = ObjectTypeEvictionScorePolicy{};
        }
        if (!ValidateObjectTypeEvictionPolicy(policies_[i])) {
            policies_[i] = ObjectTypeEvictionPolicy{};
        }
    }
}

std::vector<EvictionAssignment> TypeAwareEvictionPolicy::Assign(
    const EvictionCandidateView& view, const EvictionScope& parent,
    const EvictionGoal& goal) const {
    std::vector<EvictionAssignment> assignments;
    if (goal.total_memory_capacity <= 0) {
        return assignments;
    }

    std::array<std::vector<size_t>, kObjectDataTypeCount> refs_by_type;
    for (size_t ref : parent.refs) {
        if (ref >= view.candidates.size()) {
            continue;
        }
        const auto type_idx =
            ObjectDataTypeIndex(view.candidates[ref].data_type);
        refs_by_type[type_idx].push_back(ref);
    }

    for (size_t type_idx = 0; type_idx < kObjectDataTypeCount; ++type_idx) {
        const uint64_t used_bytes = view.used_bytes_by_type[type_idx];
        const double budget_ratio = policies_[type_idx].budget_ratio;
        const double budget_bytes =
            budget_ratio * static_cast<double>(goal.total_memory_capacity);
        if (static_cast<double>(used_bytes) <= budget_bytes ||
            refs_by_type[type_idx].empty()) {
            continue;
        }

        const double over_budget_ratio =
            static_cast<double>(used_bytes) /
                static_cast<double>(goal.total_memory_capacity) -
            budget_ratio;
        const double type_evict_ratio =
            std::min(goal.evict_ratio_target, over_budget_ratio);
        long target_count =
            std::ceil(view.eviction_base_by_type[type_idx] * type_evict_ratio);
        target_count =
            std::min(target_count, static_cast<long>(refs_by_type[type_idx].size()));
        if (target_count <= 0) {
            continue;
        }

        EvictionScope scope;
        scope.refs = std::move(refs_by_type[type_idx]);
        scope.eviction_base = view.eviction_base_by_type[type_idx];
        scope.partition = static_cast<uint8_t>(type_idx);

        EvictionAssignment assignment;
        assignment.scope = std::move(scope);
        assignment.target_count = target_count;
        assignments.push_back(std::move(assignment));
    }
    return assignments;
}

EvictionStage TypeAwareEvictionPolicy::Evict(
    const EvictionCandidateView& view, const EvictionScope& scope,
    long target_count, bool allow_soft_pinned,
    std::chrono::system_clock::time_point now) const {
    EvictionStage stage{
        .target_count = std::max<long>(target_count, 0),
        .allow_soft_pinned = allow_soft_pinned,
    };
    if (stage.target_count <= 0) {
        return stage;
    }

    for (size_t ref : scope.refs) {
        if (ref >= view.candidates.size()) {
            continue;
        }
        const auto& candidate = view.candidates[ref];
        if (!allow_soft_pinned && candidate.soft_pinned) {
            continue;
        }
        stage.refs.push_back(ref);
    }

    std::sort(stage.refs.begin(), stage.refs.end(), [&](size_t lhs, size_t rhs) {
        return Score(view.candidates[lhs], now) >
               Score(view.candidates[rhs], now);
    });
    return stage;
}

EvictionStage TypeAwareEvictionPolicy::Fallback(
    const EvictionCandidateView& view, const EvictionScope& parent,
    long target_count, bool allow_soft_pinned,
    std::chrono::system_clock::time_point now) const {
    return Evict(view, parent, target_count, allow_soft_pinned, now);
}

int64_t TypeAwareEvictionPolicy::Score(
    const EvictionCandidate& candidate,
    std::chrono::system_clock::time_point now) const {
    const auto type_idx = ObjectDataTypeIndex(candidate.data_type);
    const auto& policy = score_policies_[type_idx];
    const int64_t expired_age = std::max<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - candidate.lease_timeout)
                .count() -
            policy.eviction_grace_ms,
        0);
    const double weight =
        candidate.soft_pinned ? policy.soft_pin_weight : 1.0;
    const double score = expired_age * weight / policy.reuse_scale;
    if (!std::isfinite(score) ||
        score >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(score);
}

}  // namespace mooncake
