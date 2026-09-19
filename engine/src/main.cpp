// bench CLI: sysinfo | list | run | validate
#include "bench/result.hpp"
#include "bench/runner.hpp"
#include "bench/sysinfo.hpp"
#include "bench/workload.hpp"

#include <CLI/CLI.hpp>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// "4K,1M,64M" -> bytes. Bare numbers are bytes.
std::uint64_t parse_size(const std::string& s) {
    std::uint64_t v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{})
        throw CLI::ValidationError("size", "not a number: " + s);
    std::string suffix(p, s.data() + s.size());
    for (auto& c : suffix)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (suffix.empty() || suffix == "B")
        return v;
    if (suffix == "K" || suffix == "KB" || suffix == "KIB")
        return v << 10;
    if (suffix == "M" || suffix == "MB" || suffix == "MIB")
        return v << 20;
    if (suffix == "G" || suffix == "GB" || suffix == "GIB")
        return v << 30;
    throw CLI::ValidationError("size", "unknown suffix: " + s);
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else if (c != ' ') {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

int write_json(const bench::json& j, const std::string& out_path, bool pretty) {
    const std::string text = pretty ? j.dump(2) : j.dump();
    if (out_path.empty() || out_path == "-") {
        std::cout << text << '\n';
        return 0;
    }
    std::ofstream f{out_path};
    if (!f) {
        std::cerr << "bench: cannot write " << out_path << '\n';
        return 1;
    }
    f << text << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"bench: CPU / memory / cache / disk micro-benchmarks emitting JSON"};
    app.require_subcommand(1);
    app.set_version_flag("--version",
                         std::string{"bench "} + bench::collect_machine_info().engine_version);

    // ---- sysinfo -------------------------------------------------------------------
    bool pretty = true;
    auto* sysinfo = app.add_subcommand("sysinfo", "Print the machine block as JSON");
    sysinfo->add_flag("!--compact", pretty, "Single-line JSON");
    sysinfo->callback(
        [&] { std::exit(write_json(bench::to_json(bench::collect_machine_info()), "", pretty)); });

    // ---- list ----------------------------------------------------------------------
    auto* list = app.add_subcommand("list", "List workloads and the metrics they produce");
    list->callback([] {
        std::cout << std::format("{:<12} {:<9} {:<62} {}\n", "WORKLOAD", "STATUS", "METRICS",
                                 "DESCRIPTION");
        for (const auto& d : bench::workload_registry()) {
            std::string metrics;
            for (const auto m : d.metrics) {
                if (!metrics.empty())
                    metrics += ',';
                metrics += bench::to_string(m);
            }
            std::cout << std::format("{:<12} {:<9} {:<62} {}\n", bench::to_string(d.kind),
                                     d.implemented ? "ready" : "planned", metrics, d.description);
        }
        std::exit(0);
    });

    // ---- run -----------------------------------------------------------------------
    std::string workloads_csv, threads_csv = "1", ws_csv = "0", out_path;
    bool all = false, run_pretty = false, verbose = false;
    bench::RunConfig cfg;
    auto* run = app.add_subcommand("run", "Run benchmarks and emit a run envelope");
    run->add_option("-w,--workload", workloads_csv, "Comma-separated workloads (see `bench list`)");
    run->add_flag("--all", all, "Run every implemented workload");
    run->add_option("-t,--threads", threads_csv, "Comma-separated thread counts (default 1)");
    run->add_option("--working-set", ws_csv,
                    "Comma-separated working sets, e.g. 4K,1M,64M (default 0)");
    run->add_option("-n,--trials", cfg.trials, "Timed trials per configuration")->default_val(1);
    run->add_option("--trial-ms", cfg.trial_ms,
                    "Each trial runs whole batches until this many ms elapsed")
        ->default_val(50)
        ->check(CLI::PositiveNumber);
    run->add_option("--warmup", cfg.warmup_trials,
                    "Minimum warmup trials per configuration, discarded")
        ->default_val(5)
        ->check(CLI::NonNegativeNumber);
    run->add_option("--warmup-ms", cfg.warmup_ms,
                    "Minimum warmup time per configuration (ms), discarded")
        ->default_val(500)
        ->check(CLI::NonNegativeNumber);
    run->add_option("--spin-ms", cfg.spin_ms,
                    "Busy-spin before the first trial so the core reaches turbo")
        ->default_val(500)
        ->check(CLI::NonNegativeNumber);
    run->add_option("--seed", cfg.seed, "RNG seed for workload inputs")->default_val(0);
    run->add_option("--max-seconds", cfg.max_seconds,
                    "Safety cap on total run time; stops at a trial boundary (0 = no cap)")
        ->default_val(0)
        ->check(CLI::NonNegativeNumber);
    std::string cpus_csv;
    bool pin = false;
    run->add_flag("--pin", pin,
                  "Pin worker i to the i-th CPU of --cpus (default: 0,2,4,... then odds)");
    run->add_option("--cpus", cpus_csv, "Comma-separated CPU ids for --pin, e.g. 0,2,4,6");
    run->add_option("-o,--out", out_path, "Output file (default stdout)");
    run->add_flag("--pretty", run_pretty, "Indent JSON output");
    run->add_flag("-v,--verbose", verbose, "Print each result to stderr as it completes");
    run->callback([&] {
        if (all) {
            for (const auto& d : bench::workload_registry())
                if (d.implemented)
                    cfg.workloads.push_back(d.kind);
        }
        for (const auto& name : split_csv(workloads_csv)) {
            const auto w = bench::parse_workload(name);
            if (!w)
                throw CLI::ValidationError("--workload", "unknown workload: " + name);
            cfg.workloads.push_back(*w);
        }
        if (cfg.workloads.empty())
            throw CLI::ValidationError("--workload", "no workloads selected (or --all)");
        cfg.thread_counts.clear();
        for (const auto& t : split_csv(threads_csv))
            cfg.thread_counts.push_back(std::stoi(t));
        cfg.working_sets.clear();
        for (const auto& s : split_csv(ws_csv))
            cfg.working_sets.push_back(parse_size(s));
        cfg.verbose = verbose;
        cfg.pin = pin;
        for (const auto& c : split_csv(cpus_csv))
            cfg.cpus.push_back(std::stoi(c));
        if (!cfg.cpus.empty() && !pin)
            throw CLI::ValidationError("--cpus", "requires --pin");
        std::signal(SIGINT, [](int) { bench::request_abort(); });

        std::vector<std::string> argv_copy(argv, argv + argc);
        argv_copy[0] = "bench";
        const auto machine = bench::collect_machine_info();
        bench::ProgressFn progress;
        if (verbose)
            progress = [](const bench::Result& r, bool warmup) {
                std::string cpus;
                for (const auto& c : r.params["per_thread_cpu"])
                    cpus += std::format("{} ", c.get<int>());
                if (!cpus.empty())
                    cpus.pop_back();
                std::cerr << std::format("{:<8} t={:<3} {}={:<4} {:>14.0f} {}  {:.2f} ms  "
                                         "spread={:.1f}us  pinviol={} cpus=[{}]\n",
                                         bench::to_string(r.workload), r.thread_count,
                                         warmup ? "warmup" : "trial ", warmup ? -r.trial : r.trial,
                                         r.value, bench::to_string(bench::unit_of(r.metric)),
                                         static_cast<double>(r.duration_ns) / 1e6,
                                         r.params["start_spread_us"].get<double>(),
                                         r.params["pin_violations"].get<int>(), cpus);
            };
        const auto envelope = bench::run_benchmarks(cfg, machine, std::move(argv_copy), progress);
        const auto j = bench::to_json(envelope);
        const auto problems = bench::validate_run(j); // never emit something we would reject
        for (const auto& p : problems)
            std::cerr << "bench: internal validation: " << p << '\n';
        const int rc = write_json(j, out_path, run_pretty);
        std::exit(problems.empty() ? rc : 2);
    });

    // ---- validate ------------------------------------------------------------------
    std::string in_path;
    auto* validate = app.add_subcommand("validate", "Structurally validate a run JSON file");
    validate->add_option("file", in_path, "Run JSON file")->required()->check(CLI::ExistingFile);
    validate->callback([&] {
        std::ifstream f{in_path};
        bench::json j;
        try {
            j = bench::json::parse(f);
        } catch (const bench::json::exception& e) {
            std::cerr << "bench: " << in_path << ": not JSON: " << e.what() << '\n';
            std::exit(1);
        }
        const auto problems = bench::validate_run(j);
        if (problems.empty()) {
            std::cout << std::format("valid: {} ({} results)\n", in_path, j["results"].size());
            std::exit(0);
        }
        for (const auto& p : problems)
            std::cerr << "  " << p << '\n';
        std::cerr << std::format("invalid: {} ({} problems)\n", in_path, problems.size());
        std::exit(1);
    });

    CLI11_PARSE(app, argc, argv);
    return 0;
}
