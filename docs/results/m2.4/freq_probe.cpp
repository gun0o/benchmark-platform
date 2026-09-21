// Effective-frequency probe for the M2.4 write-up. Not part of the engine.
//
// Why this exists: nothing inside WSL2 reports a usable core frequency. /proc/cpuinfo
// reports the same synthetic 3071.998 MHz on all 22 vCPUs (impossible on a hybrid part
// under turbo), /sys/devices/system/cpu/cpu0/cpufreq does not exist, and Windows reports
// only the 2.3 GHz nominal. So the frequency is measured instead.
//
// Method: time a chain of instructions that must execute one after another, where the
// latency in cycles is known. N dependent instructions of latency L take N*L cycles, and
// nothing the out-of-order engine does can overlap them, so frequency = N*L / seconds.
//
// Two instructions with *different* known latencies are used, because a single chain
// cannot tell you whether you measured the clock or measured the CPU quietly optimising
// your chain away. `add reg,reg` has latency 1 and `imul reg,reg` has latency 3, so their
// rates must differ by exactly 3x. If they do, both chains were real.
//
// The first version of this probe used `add $1, %rax` and reported 25 GHz. That is not a
// clock: recent Intel cores collapse chains of add-with-immediate to the same register in
// the renamer, so the chain was not a chain at all. It is kept here as a third row,
// because it is a good illustration of why one chain is never enough.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

template <class F> static std::vector<double> rates(F f, std::uint64_t ops_per_rep, int reps) {
    std::vector<double> r;
    for (int i = 0; i < reps; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        r.push_back(static_cast<double>(ops_per_rep) /
                    std::chrono::duration<double>(t1 - t0).count() / 1e9);
    }
    std::sort(r.begin(), r.end());
    return r;
}

int main() {
    constexpr std::uint64_t N = 2'000'000, U = 100;
    constexpr int kReps = 15;
    auto report = [](const char* what, const std::vector<double>& r, int latency) {
        std::printf("%-22s min %6.3f  median %6.3f  max %6.3f G/s", what, r.front(),
                    r[r.size() / 2], r.back());
        if (latency)
            std::printf("   => %.3f GHz (latency %d)", r.back() * latency, latency);
        std::printf("\n");
    };
    const auto a_imm = rates([&] { std::uint64_t x = 1;
        for (std::uint64_t i = 0; i < N; ++i)
            asm volatile(".rept 100\n\tadd $1, %0\n\t.endr\n\t" : "+r"(x) : :); }, N * U, kReps);
    const auto a_reg = rates([&] { std::uint64_t x = 1, y = 3;
        for (std::uint64_t i = 0; i < N; ++i)
            asm volatile(".rept 100\n\tadd %1, %0\n\t.endr\n\t" : "+r"(x) : "r"(y) :); }, N * U, kReps);
    const auto m_reg = rates([&] { std::uint64_t x = 1, y = 3;
        for (std::uint64_t i = 0; i < N; ++i)
            asm volatile(".rept 100\n\timul %1, %0\n\t.endr\n\t" : "+r"(x) : "r"(y) :); }, N * U, kReps);

    std::printf("effective core frequency, 1 thread, %d reps of %llu dependent instructions\n\n",
                kReps, (unsigned long long)(N * U));
    report("dependent add reg,reg", a_reg, 1);
    report("dependent imul reg,reg", m_reg, 3);
    report("dependent add $1,reg", a_imm, 0);
    const double f_add = a_reg.back(), f_mul = m_reg.back() * 3;
    std::printf("\ncross-check: add-chain %.3f GHz vs imul-chain %.3f GHz  ->  %.2f%% apart\n",
                f_add, f_mul, 100.0 * (f_add - f_mul) / f_mul);
    std::printf("add-immediate chain is %.1fx the add-register chain: not a clock, but the\n"
                "renamer collapsing add-with-immediate. A single chain cannot detect this.\n",
                a_imm.back() / a_reg.back());
    return 0;
}
