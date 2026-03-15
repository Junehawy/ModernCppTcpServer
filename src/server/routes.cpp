#include "common/server_metrics.h"
#include "common/worker_pool.h"
#include "net/net_utils.h"
#include "router.h"

extern ServerMetrics g_metrics;
extern WorkerPool *g_worker_pool;

void set_routes(HttpRouter &router) {

    router.use([](const SimpleHttpRequest &req, SimpleHttpResponse &resp, NextFn next) {
        const auto start = std::chrono::steady_clock::now();
        next();
        const auto us =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        g_metrics.total_requests.fetch_add(1, std::memory_order_relaxed);
        NET_LOG_DEBUG("[MW] {} {} → {} ({}µs)", req.method, req.path, resp.status_code, us);
    });
    // ── GET / ─────────────────────────────────────────────────────────
    router.get("/", [](const auto &req, auto &resp) {
        resp.body = "Welcome to moderncpp-server!\n";
        resp.content_type = "text/plain";
    });

    // ── GET /hello ────────────────────────────────────────────────────
    router.get("/hello", [](const auto &req, auto &resp) {
        resp.body = "Hello, world!\n";
        resp.content_type = "text/plain";
    });

    // ── GET /json ─────────────────────────────────────────────────────
    router.get("/json", [](const auto &req, auto &resp) {
        resp.body = R"({"status":"ok","server":"moderncpp-server"})";
        resp.content_type = "application/json";
    });

    // ── POST /echo ────────────────────────────────────────────────────
    router.post("/echo", [](const auto &req, auto &resp) {
        resp.body = req.body;
        resp.content_type = req.content_type.empty() ? "text/plain" : req.content_type;
    });

    // ── GET /info ─────────────────────────────────────────────────────
    router.get("/info", [](const auto &req, auto &resp) {
        std::string body;
        body += "Method:     " + req.method + "\n";
        body += "Path:       " + req.path + "\n";
        body += "Version:    " + req.version + "\n";
        body += "Host:       " + req.host + "\n";
        body += "User-Agent: " + req.user_agent + "\n";
        body += "\nHeaders:\n";
        for (const auto &[k, v]: req.headers)
            body += "  " + k + ": " + v + "\n";
        resp.body = body;
        resp.content_type = "text/plain";
    });

    // ── GET /health (for load balancers / k8s probes) ─────────────────
    router.get("/health", [](const auto &req, auto &resp) {
        resp.body = R"({"status":"healthy"})";
        resp.content_type = "application/json";
    });

    // ── GET /metrics ──────────────────────────────────────────────────
    router.get("/metrics", [](const auto &, auto &resp) {
        const size_t queue_depth = g_worker_pool ? g_worker_pool->pending_count() : 0;
        resp.body = g_metrics.to_json(queue_depth);
        resp.content_type = "application/json";
    });

    // ── GET /slow ─────────────────────────────────────────────────────
    router.get("/slow", [](const auto &req, auto &resp) {
        int ms = 100;

        // 手动解析 query string（不依赖框架扩展）
        // path 形如 /slow?ms=50，从 headers 或 path 里取
        // 实际上 HttpParser 会把 path 里的 query string 一起放进 req.path
        // 所以直接在 path 里找 '?ms='
        const auto &p = req.path;
        if (const auto pos = p.find("?ms="); pos != std::string::npos) {
            try {
                const int v = std::stoi(p.substr(pos + 4));
                if (v >= 0 && v <= 10000)
                    ms = v;
            } catch (...) {
            }
        }

        if (ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }

        resp.body = R"({"status":"ok","slept_ms":)" + std::to_string(ms) + "}\n";
        resp.content_type = "application/json";
    });
}
