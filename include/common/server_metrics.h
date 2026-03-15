#pragma once
#include <atomic>
#include <chrono>

struct ServerMetrics {
    std::atomic<int64_t> active_connections{0};
    std::atomic<uint64_t> total_connections{0};
    std::atomic<uint64_t> rejected_connections{0};

    std::atomic<uint64_t> total_requests{0};

    mutable std::atomic<uint64_t> qps_last_count{0};
    mutable std::atomic<int64_t> qps_last_ts_ms{0};
    mutable std::atomic<uint64_t> qps_current{0};

    const std::chrono::steady_clock::time_point start_time;

    ServerMetrics() : start_time(std::chrono::steady_clock::now()) {}

    ServerMetrics(const ServerMetrics &) = delete;
    ServerMetrics &operator=(const ServerMetrics &) = delete;

    uint64_t uptime_seconds() const {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
    }

    double avg_qps() const {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        return elapsed > 0.0 ? total_requests.load() / elapsed : 0.0;
    }

    uint64_t refresh_qps() const {
        using namespace std::chrono;
        const auto now_ms = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
        const auto last_ts = qps_last_ts_ms.load();
        const auto last_count = qps_last_count.load();
        const auto cur_cnt = total_requests.load();

        if (const double dt = (now_ms - last_ts) / 1000.0; dt >= 1.0) {
            const auto qps = (cur_cnt - last_count) / dt;
            qps_current.store(qps);
            qps_last_count.store(cur_cnt);
            qps_last_ts_ms.store(now_ms);
            return qps;
        }
        return qps_current.load();
    }

    std::string to_json(size_t worker_queue_depth = 0) const;
};
