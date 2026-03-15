#include "common/server_metrics.h"

std::string ServerMetrics::to_json(const size_t worker_queue_depth) const {
    const uint64_t qps = refresh_qps();
    const uint64_t uptime = uptime_seconds();

    std::ostringstream oss;

    oss << std::fixed << std::setprecision(2);
    oss << "{\n";
    oss << "  \"uptime_seconds\": " << uptime << ",\n";
    oss << "  \"connections\": {\n";
    oss << "    \"active\": " << active_connections.load() << ",\n";
    oss << "    \"total_accepted\": " << total_connections.load() << ",\n";
    oss << "    \"rejected_over_limit\": " << rejected_connections.load() << "\n";
    oss << "  },\n";
    oss << "  \"requests\": {\n";
    oss << "    \"total\": " << total_requests.load() << ",\n";
    oss << "    \"qps_current\": " << qps << ",\n";
    oss << "    \"qps_avg\": " << avg_qps() << "\n";
    oss << "  },\n";
    oss << "  \"worker_pool\": {\n";
    oss << "    \"queue_depth\": " << worker_queue_depth << "\n";
    oss << "  }\n";
    oss << "}\n";

    return oss.str();
}
