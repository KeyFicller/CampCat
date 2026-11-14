#include "app/device_task_lease.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

TEST(DeviceTaskLease, OneOwnerBlocksEveryCompetingStartUntilItFinishes) {
    campcat::device_task_lease tasks;
    auto owner = tasks.try_acquire();
    ASSERT_TRUE(owner);
    EXPECT_TRUE(tasks.busy());
    EXPECT_TRUE(tasks.owns(*owner));
    EXPECT_FALSE(tasks.try_acquire());

    auto moved = std::move(*owner);
    owner.reset();
    EXPECT_TRUE(tasks.busy());
    EXPECT_TRUE(tasks.owns(moved));
    campcat::device_task_lease other_device;
    EXPECT_FALSE(other_device.owns(moved));
}

TEST(DeviceTaskLease, CompletionReleasesOwnershipForTheNextTask) {
    campcat::device_task_lease tasks;
    {
        auto owner = tasks.try_acquire();
        ASSERT_TRUE(owner);
    }
    EXPECT_FALSE(tasks.busy());
    EXPECT_TRUE(tasks.try_acquire());
}

TEST(DeviceTaskLease, ConcurrentStartsHaveExactlyOneWinner) {
    campcat::device_task_lease tasks;
    std::atomic<int> winners{0};
    std::atomic<int> attempted{0};
    std::vector<std::thread> starters;
    for (int i = 0; i < 8; ++i) {
        starters.emplace_back([&] {
            auto owner = tasks.try_acquire();
            if (owner) {
                ++winners;
            }
            ++attempted;
            while (attempted.load() < 8) {
                std::this_thread::yield();
            }
        });
    }
    for (auto &starter : starters) {
        starter.join();
    }
    EXPECT_EQ(winners.load(), 1);
    EXPECT_FALSE(tasks.busy());
}

} // namespace
