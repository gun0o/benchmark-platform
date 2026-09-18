// SpinBarrier: a reusable sense-reversing barrier whose waiters spin instead of sleeping.
//
// Why not std::barrier: its waiters park on a futex (after a ~16-iteration spin that ends
// in sched_yield). Waking N parked threads is sequential in the kernel and, inside a VM,
// means un-parking vCPUs. Measured on WSL2 with 8 workers: start spreads with a median of
// 85-1100 us and maxima of 3-10 ms. A benchmark trial needs every worker to begin within
// microseconds of the release, so the workers must never sleep between trials.
//
// The completion function runs in the last arriver at the instant the phase completes,
// which is where t_release / t_done are stamped. Workers spin with PAUSE; the main thread
// (not a worker) can use arrive_and_wait_polling so it does not burn a core.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace bench {

class SpinBarrier {
public:
    using Completion = std::function<void()>; // must not throw

    explicit SpinBarrier(int participants, Completion completion = {})
        : n_(participants), count_(participants), completion_(std::move(completion)) {}

    SpinBarrier(const SpinBarrier&) = delete;
    SpinBarrier& operator=(const SpinBarrier&) = delete;

    // Arrive; the last arriver runs the completion, resets the count and bumps the
    // generation. Everyone else spins on the generation with PAUSE.
    void arrive_and_wait() noexcept {
        const std::uint64_t gen = arrive();
        while (generation_.load(std::memory_order_acquire) == gen)
            __builtin_ia32_pause();
    }

    // Same, but sleeps between polls. For a coordinator whose wake-up latency does not
    // affect any measurement (stamps are taken by the last arriver, not by waiters).
    void arrive_and_wait_polling(std::chrono::microseconds poll) noexcept {
        const std::uint64_t gen = arrive();
        while (generation_.load(std::memory_order_acquire) == gen)
            std::this_thread::sleep_for(poll);
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

private:
    // Returns the generation to wait for the end of. If this was the last arrival, the
    // phase is already complete on return.
    std::uint64_t arrive() noexcept {
        const std::uint64_t gen = generation_.load(std::memory_order_acquire);
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (completion_)
                completion_();
            count_.store(
                n_, std::memory_order_relaxed); // before the bump: next phase sees a full count
            generation_.fetch_add(1, std::memory_order_release);
        }
        return gen;
    }

    const int n_;
    alignas(64) std::atomic<int> count_;
    alignas(64) std::atomic<std::uint64_t> generation_{0};
    Completion completion_;
};

} // namespace bench
