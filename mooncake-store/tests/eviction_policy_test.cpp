#include "eviction_policy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace mooncake::test {
namespace {

using Clock = std::chrono::system_clock;

EvictionCandidate Candidate(size_t index, ObjectDataType type,
                            Clock::time_point lease_timeout,
                            bool soft_pinned = false) {
    return EvictionCandidate{
        .index = index,
        .data_type = type,
        .lease_timeout = lease_timeout,
        .soft_pinned = soft_pinned,
    };
}

EvictionCandidateView MakeView(std::vector<EvictionCandidate> candidates) {
    EvictionCandidateView view;
    view.candidates = std::move(candidates);
    view.total_eviction_base = static_cast<long>(view.candidates.size());
    for (const auto& candidate : view.candidates) {
        const auto type_idx = ObjectDataTypeIndex(candidate.data_type);
        view.eviction_base_by_type[type_idx]++;
    }
    return view;
}

}  // namespace

TEST(TypeAwareEvictionPolicyTest, ObjectDataTypeIndexMapsUnknownFutureTypes) {
    EXPECT_EQ(kObjectDataTypeCount,
              static_cast<size_t>(ObjectDataType::HIDDEN_STATE) + 1);
    EXPECT_EQ(ObjectDataTypeIndex(ObjectDataType::HIDDEN_STATE),
              static_cast<size_t>(ObjectDataType::HIDDEN_STATE));
    EXPECT_EQ(ObjectDataTypeIndex(static_cast<ObjectDataType>(200)),
              ObjectDataTypeIndex(ObjectDataType::UNKNOWN));
}

TEST(TypeAwareEvictionPolicyTest, AssignPartitionsOverBudgetTypeScopes) {
    const auto now = Clock::time_point{} + std::chrono::seconds(100);
    auto view = MakeView({
        Candidate(0, ObjectDataType::KVCACHE, now - std::chrono::seconds(10)),
        Candidate(1, ObjectDataType::HIDDEN_STATE,
                  now - std::chrono::seconds(20)),
        Candidate(2, ObjectDataType::HIDDEN_STATE,
                  now - std::chrono::seconds(30)),
    });
    view.used_bytes_by_type[ObjectDataTypeIndex(ObjectDataType::KVCACHE)] =
        100;
    view.used_bytes_by_type[ObjectDataTypeIndex(ObjectDataType::HIDDEN_STATE)] =
        600;

    std::array<ObjectTypeEvictionPolicy, kObjectDataTypeCount> budget{};
    budget[ObjectDataTypeIndex(ObjectDataType::HIDDEN_STATE)].budget_ratio =
        0.2;
    TypeAwareEvictionPolicy policy({}, budget);

    auto assignments = policy.Assign(
        view, EvictionScope::All(view),
        EvictionGoal{.evict_ratio_target = 0.5,
                     .evict_ratio_lowerbound = 0.25,
                     .total_memory_capacity = 1000});

    ASSERT_EQ(assignments.size(), 1u);
    EXPECT_EQ(assignments[0].scope.partition,
              static_cast<uint8_t>(
                  ObjectDataTypeIndex(ObjectDataType::HIDDEN_STATE)));
    EXPECT_EQ(assignments[0].scope.refs, (std::vector<size_t>{1, 2}));
    EXPECT_EQ(assignments[0].target_count, 1);
}

TEST(TypeAwareEvictionPolicyTest, DefaultBudgetsDoNotAssignTypeScopes) {
    const auto now = Clock::time_point{} + std::chrono::seconds(100);
    auto view = MakeView({
        Candidate(0, ObjectDataType::KVCACHE, now - std::chrono::seconds(10)),
        Candidate(1, ObjectDataType::HIDDEN_STATE,
                  now - std::chrono::seconds(20)),
    });
    view.used_bytes_by_type[ObjectDataTypeIndex(ObjectDataType::KVCACHE)] =
        400;
    view.used_bytes_by_type[ObjectDataTypeIndex(ObjectDataType::HIDDEN_STATE)] =
        400;

    TypeAwareEvictionPolicy policy;

    auto assignments = policy.Assign(
        view, EvictionScope::All(view),
        EvictionGoal{.evict_ratio_target = 0.5,
                     .evict_ratio_lowerbound = 0.25,
                     .total_memory_capacity = 1000});

    EXPECT_TRUE(assignments.empty());
}

TEST(TypeAwareEvictionPolicyTest, RejectsInvalidPolicyConfig) {
    ObjectTypeEvictionScorePolicy score;
    score.reuse_scale = 0.0;
    EXPECT_FALSE(ValidateObjectTypeEvictionScorePolicy(score));

    score.reuse_scale = 1.0;
    score.eviction_grace_ms = -1;
    EXPECT_FALSE(ValidateObjectTypeEvictionScorePolicy(score));

    ObjectTypeEvictionPolicy budget;
    budget.budget_ratio = 1.5;
    EXPECT_FALSE(ValidateObjectTypeEvictionPolicy(budget));
}

TEST(TypeAwareEvictionPolicyTest, EvictRanksWithinScopeByTypePolicyScore) {
    const auto now = Clock::time_point{} + std::chrono::seconds(100);
    auto view = MakeView({
        Candidate(0, ObjectDataType::KVCACHE, now - std::chrono::seconds(20)),
        Candidate(1, ObjectDataType::HIDDEN_STATE,
                  now - std::chrono::seconds(100)),
    });

    std::array<ObjectTypeEvictionScorePolicy, kObjectDataTypeCount> score{};
    score[ObjectDataTypeIndex(ObjectDataType::HIDDEN_STATE)].reuse_scale =
        10.0;
    TypeAwareEvictionPolicy policy(score, {});

    auto stage =
        policy.Fallback(view, EvictionScope::All(view), 2,
                        /*allow_soft_pinned=*/false, now);

    ASSERT_EQ(stage.refs.size(), 2u);
    EXPECT_EQ(stage.refs[0], 0u);
    EXPECT_EQ(stage.refs[1], 1u);
    EXPECT_EQ(stage.target_count, 2);
}

TEST(TypeAwareEvictionPolicyTest, FallbackCanIncludeSoftPinnedCandidates) {
    const auto now = Clock::time_point{} + std::chrono::seconds(100);
    auto view = MakeView({
        Candidate(0, ObjectDataType::KVCACHE, now - std::chrono::seconds(10),
                  true),
        Candidate(1, ObjectDataType::KVCACHE, now - std::chrono::seconds(20)),
    });
    TypeAwareEvictionPolicy policy;

    auto no_soft =
        policy.Fallback(view, EvictionScope::All(view), 2,
                        /*allow_soft_pinned=*/false, now);
    EXPECT_EQ(no_soft.refs, (std::vector<size_t>{1}));

    auto with_soft =
        policy.Fallback(view, EvictionScope::All(view), 2,
                        /*allow_soft_pinned=*/true, now);
    EXPECT_EQ(with_soft.refs, (std::vector<size_t>{1, 0}));
}

}  // namespace mooncake::test
