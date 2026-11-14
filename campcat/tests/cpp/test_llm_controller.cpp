#include "app/llm_controller.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

namespace {

using campcat::llm_controller;
using namespace std::chrono_literals;

struct blocked_work {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    bool unblocked = false;

    ~blocked_work() { unblock(); }
    void unblock() {
        if (!unblocked) {
            release.set_value();
            unblocked = true;
        }
    }
    campcat::llm_result run(const campcat::device_task_lease::owner &) {
        entered.set_value();
        released.wait();
        campcat::llm_result result;
        result.ok = true;
        result.text = "first";
        return result;
    }
};

bool wait_until_idle(llm_controller &controller) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (controller.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return !controller.running();
}

TEST(LlmController, StopKeepsTheDeviceOwnedUntilTheWorkerAcknowledgesIt) {
    campcat::llm_host host;
    campcat::device_task_lease tasks;
    llm_controller controller(host);
    blocked_work work;
    llm_controller::request request;
    request.work = [&](const auto &owner) {
        EXPECT_TRUE(tasks.owns(owner)); // nested tools reuse this owner
        EXPECT_FALSE(tasks.try_acquire());
        return work.run(owner);
    };
    ASSERT_TRUE(controller.start(tasks, std::move(request)));
    ASSERT_EQ(work.entered.get_future().wait_for(2s), std::future_status::ready);

    EXPECT_FALSE(tasks.try_acquire());
    controller.request_stop();
    EXPECT_TRUE(controller.stopping());
    EXPECT_TRUE(controller.running());
    EXPECT_FALSE(tasks.try_acquire());
    EXPECT_FALSE(controller.queue_prompt("must not restart"));

    work.unblock();
    ASSERT_TRUE(wait_until_idle(controller));
    EXPECT_FALSE(tasks.busy());
    EXPECT_TRUE(tasks.try_acquire());
}

TEST(LlmController, TheLatestQueuedPromptRunsOnTheSameLeaseAfterTheFirstTurn) {
    campcat::llm_host host;
    campcat::device_task_lease tasks;
    llm_controller controller(host);
    blocked_work work;
    llm_controller::request request;
    request.work = [&](const auto &owner) { return work.run(owner); };
    request.followup = [&](const std::string &prompt, const auto &owner) {
        EXPECT_TRUE(tasks.owns(owner));
        campcat::llm_result result;
        result.ok = true;
        result.text = prompt;
        return result;
    };
    ASSERT_TRUE(controller.start(tasks, std::move(request)));
    ASSERT_EQ(work.entered.get_future().wait_for(2s), std::future_status::ready);
    bool replaced = true;
    EXPECT_TRUE(controller.queue_prompt("superseded", &replaced));
    EXPECT_FALSE(replaced);
    EXPECT_TRUE(controller.queue_prompt("latest", &replaced));
    EXPECT_TRUE(replaced);
    work.unblock();
    ASSERT_TRUE(wait_until_idle(controller));

    auto first = controller.take_completion();
    auto promotion = controller.take_completion();
    auto second = controller.take_completion();
    ASSERT_TRUE(first);
    ASSERT_TRUE(promotion);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->kind, llm_controller::completion_kind::turn);
    EXPECT_EQ(promotion->kind, llm_controller::completion_kind::promotion);
    EXPECT_EQ(first->result.text, "first");
    EXPECT_EQ(second->result.text, "latest");
    EXPECT_FALSE(controller.take_completion());
}

TEST(LlmController, StoppingCancelsTheQueuedTurnRatherThanRunningItAtShutdown) {
    campcat::llm_host host;
    campcat::device_task_lease tasks;
    llm_controller controller(host);
    blocked_work work;
    llm_controller::request request;
    request.work = [&](const auto &owner) { return work.run(owner); };
    request.followup = [&](const std::string &, const auto &) {
        ADD_FAILURE() << "cancelled follow-up ran";
        return campcat::llm_result{};
    };
    ASSERT_TRUE(controller.start(tasks, std::move(request)));
    ASSERT_EQ(work.entered.get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(controller.queue_prompt("cancelled"));
    EXPECT_TRUE(controller.request_stop());
    work.unblock();
    controller.begin_shutdown();
    controller.finish_shutdown();

    EXPECT_FALSE(controller.running());
    EXPECT_FALSE(tasks.busy());
    ASSERT_TRUE(controller.take_completion());
    EXPECT_FALSE(controller.take_completion());
    llm_controller::request later;
    later.work = [](const auto &) { return campcat::llm_result{}; };
    EXPECT_FALSE(controller.start(tasks, std::move(later)));
}

TEST(LlmController, ResetRejectsQueuedMessagesAndReturnsAnExplicitResetCompletion) {
    campcat::llm_host host;
    campcat::device_task_lease tasks;
    llm_controller controller(host);
    blocked_work work;
    llm_controller::request request;
    request.kind = llm_controller::completion_kind::reset;
    request.work = [&](const auto &owner) { return work.run(owner); };
    ASSERT_TRUE(controller.start(tasks, std::move(request)));
    ASSERT_EQ(work.entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(controller.resetting());
    EXPECT_FALSE(controller.queue_prompt("would be cleared"));
    work.unblock();
    ASSERT_TRUE(wait_until_idle(controller));
    auto done = controller.take_completion();
    ASSERT_TRUE(done);
    EXPECT_EQ(done->kind, llm_controller::completion_kind::reset);
    EXPECT_FALSE(controller.resetting());
}

TEST(LlmController, AnInterruptedMemoryPassPromotesItsQueuedMessage) {
    campcat::llm_host host;
    campcat::device_task_lease tasks;
    llm_controller controller(host);
    blocked_work work;
    llm_controller::request request;
    request.kind = llm_controller::completion_kind::memory;
    request.work = [&](const auto &owner) { return work.run(owner); };
    request.followup = [&](const std::string &prompt, const auto &) {
        campcat::llm_result result;
        result.ok = true;
        result.text = prompt;
        return result;
    };
    ASSERT_TRUE(controller.start(tasks, std::move(request)));
    ASSERT_EQ(work.entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(controller.memorizing());
    EXPECT_TRUE(controller.queue_prompt("new task"));
    work.unblock();
    ASSERT_TRUE(wait_until_idle(controller));
    auto promotion = controller.take_completion();
    ASSERT_TRUE(promotion);
    EXPECT_EQ(promotion->kind, llm_controller::completion_kind::promotion);
    auto done = controller.take_completion();
    ASSERT_TRUE(done);
    EXPECT_EQ(done->kind, llm_controller::completion_kind::turn);
    EXPECT_EQ(done->result.text, "new task");
    EXPECT_FALSE(controller.take_completion());
    EXPECT_FALSE(controller.memorizing());
}

TEST(LlmController, AFailedJobReleasesTheLeaseAndReportsFailure) {
    campcat::llm_host host;
    campcat::device_task_lease tasks;
    llm_controller controller(host);
    llm_controller::request request;
    request.work = [](const auto &) -> campcat::llm_result {
        throw std::runtime_error("fake worker failure");
    };
    ASSERT_TRUE(controller.start(tasks, std::move(request)));
    ASSERT_TRUE(wait_until_idle(controller));
    EXPECT_FALSE(tasks.busy());
    auto done = controller.take_completion();
    ASSERT_TRUE(done);
    EXPECT_FALSE(done->result.ok);
    EXPECT_EQ(done->result.error, "fake worker failure");
}

TEST(LlmController, AManualOwnerRefusesAnLlmStartBeforeLaunchingItsWorker) {
    campcat::llm_host host;
    campcat::device_task_lease tasks;
    llm_controller controller(host);
    auto manual = tasks.try_acquire();
    ASSERT_TRUE(manual);
    llm_controller::request request;
    request.work = [](const auto &) {
        ADD_FAILURE() << "worker ran without owning the device";
        return campcat::llm_result{};
    };
    EXPECT_FALSE(controller.start(tasks, std::move(request)));
    EXPECT_FALSE(controller.running());
    EXPECT_FALSE(controller.take_completion());
}

TEST(LlmController, ShutdownStartsTheFinalPassWithoutWaitingAndFinishJoinsIt) {
    campcat::llm_host host;
    llm_controller controller(host);
    blocked_work final;
    std::atomic<bool> saved{false};
    controller.begin_shutdown([&] {
        final.entered.set_value();
        final.released.wait();
        saved.store(true);
    });
    ASSERT_EQ(final.entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(saved.load());
    final.unblock();
    controller.finish_shutdown();
    EXPECT_TRUE(saved.load());
}

TEST(LlmController, AMessageAfterPromotionCreatesANewTurnRatherThanReplacingIt) {
    campcat::llm_host host;
    campcat::device_task_lease tasks;
    llm_controller controller(host);
    blocked_work first;
    blocked_work second;
    llm_controller::request request;
    request.work = [&](const auto &owner) { return first.run(owner); };
    request.followup = [&](const std::string &prompt, const auto &owner) {
        if (prompt == "second") {
            return second.run(owner);
        }
        campcat::llm_result result;
        result.ok = true;
        result.text = prompt;
        return result;
    };
    ASSERT_TRUE(controller.start(tasks, std::move(request)));
    ASSERT_EQ(first.entered.get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(controller.queue_prompt("second"));
    first.unblock();
    ASSERT_EQ(second.entered.get_future().wait_for(2s), std::future_status::ready);
    bool replaced = true;
    EXPECT_TRUE(controller.queue_prompt("third", &replaced));
    EXPECT_FALSE(replaced);
    second.unblock();
    ASSERT_TRUE(wait_until_idle(controller));

    ASSERT_TRUE(controller.take_completion());
    auto promote_second = controller.take_completion();
    ASSERT_TRUE(promote_second);
    EXPECT_EQ(promote_second->kind, llm_controller::completion_kind::promotion);
    ASSERT_TRUE(controller.take_completion());
    auto promote_third = controller.take_completion();
    ASSERT_TRUE(promote_third);
    EXPECT_EQ(promote_third->kind, llm_controller::completion_kind::promotion);
    auto third = controller.take_completion();
    ASSERT_TRUE(third);
    EXPECT_EQ(third->result.text, "third");
    EXPECT_FALSE(controller.take_completion());
}

} // namespace
