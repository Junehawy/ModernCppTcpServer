#include "router.h"

void set_routes(HttpRouter &router) {
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
}
