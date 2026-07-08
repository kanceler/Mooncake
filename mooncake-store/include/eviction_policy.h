#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.h"

namespace mooncake {

static constexpr size_t kObjectDataTypeCount =
    static_cast<size_t>(ObjectDataType::HIDDEN_STATE) + 1;

size_t ObjectDataTypeIndex(ObjectDataType data_type);

struct ObjectTypeEvictionScorePolicy {
    double reuse_scale{1.0};
    double soft_pin_weight{1.0};
    int64_t eviction_grace_ms{0};
};

struct ObjectTypeEvictionPolicy {
    double budget_ratio{1.0};
};

bool ValidateObjectTypeEvictionScorePolicy(
    const ObjectTypeEvictionScorePolicy& policy);
bool ValidateObjectTypeEvictionPolicy(const ObjectTypeEvictionPolicy& policy);

struct EvictionCandidate {
    // Index into EvictionCandidateView::candidates. MasterService uses this as
    // an opaque ref and revalidates the real metadata before mutation.
    size_t index{0};
    ObjectDataType data_type{ObjectDataType::UNKNOWN};
    std::chrono::system_clock::time_point lease_timeout{};
    bool soft_pinned{false};
};

struct EvictionCandidateView {
    std::vector<EvictionCandidate> candidates;
    std::array<long, kObjectDataTypeCount> eviction_base_by_type{};
    std::array<uint64_t, kObjectDataTypeCount> used_bytes_by_type{};
    long total_eviction_base{0};
};

struct EvictionGoal {
    double evict_ratio_target{0.0};
    double evict_ratio_lowerbound{0.0};
    int64_t total_memory_capacity{0};
};

struct EvictionScope {
    std::vector<size_t> refs;
    long eviction_base{0};
    uint8_t partition{0};

    static EvictionScope All(const EvictionCandidateView& view);
};

struct EvictionAssignment {
    EvictionScope scope;
    long target_count{0};
};

struct EvictionStage {
    std::vector<size_t> refs;
    long target_count{0};
    bool allow_soft_pinned{false};
};

class EvictionPolicy {
   public:
    virtual ~EvictionPolicy() = default;

    virtual std::vector<EvictionAssignment> Assign(
        const EvictionCandidateView& view, const EvictionScope& parent,
        const EvictionGoal& goal) const = 0;

    virtual EvictionStage Evict(
        const EvictionCandidateView& view, const EvictionScope& scope,
        long target_count, bool allow_soft_pinned,
        std::chrono::system_clock::time_point now) const = 0;

    virtual EvictionStage Fallback(
        const EvictionCandidateView& view, const EvictionScope& parent,
        long target_count, bool allow_soft_pinned,
        std::chrono::system_clock::time_point now) const = 0;
};

class TypeAwareEvictionPolicy : public EvictionPolicy {
   public:
    TypeAwareEvictionPolicy();
    TypeAwareEvictionPolicy(
        std::array<ObjectTypeEvictionScorePolicy, kObjectDataTypeCount>
            score_policies,
        std::array<ObjectTypeEvictionPolicy, kObjectDataTypeCount> policies);

    std::vector<EvictionAssignment> Assign(
        const EvictionCandidateView& view, const EvictionScope& parent,
        const EvictionGoal& goal) const override;

    EvictionStage Evict(const EvictionCandidateView& view,
                        const EvictionScope& scope, long target_count,
                        bool allow_soft_pinned,
                        std::chrono::system_clock::time_point now)
        const override;

    EvictionStage Fallback(const EvictionCandidateView& view,
                           const EvictionScope& parent, long target_count,
                           bool allow_soft_pinned,
                           std::chrono::system_clock::time_point now)
        const override;

   private:
    EvictionStage SelectVictimsInScope(
        const EvictionCandidateView& view, const EvictionScope& scope,
        long target_count, bool allow_soft_pinned,
        std::chrono::system_clock::time_point now) const;

    int64_t Score(const EvictionCandidate& candidate,
                  std::chrono::system_clock::time_point now) const;

    std::array<ObjectTypeEvictionScorePolicy, kObjectDataTypeCount>
        score_policies_{};
    std::array<ObjectTypeEvictionPolicy, kObjectDataTypeCount> policies_{};
};

}  // namespace mooncake
