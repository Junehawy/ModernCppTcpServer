#pragma once
#include <functional>
#include <string>
#include <unordered_map>

#include "http_types.h"

using NextFn = std::function<void()>;
using RouterHandler = std::function<void(const SimpleHttpRequest &, SimpleHttpResponse &)>;
using MiddlewareFn = std::function<void(const SimpleHttpRequest &, SimpleHttpResponse &, NextFn)>;

// HTTP Router
class HttpRouter {
public:
    void use(MiddlewareFn middleware) { middleware_.emplace_back(std::move(middleware)); }

    void get(const std::string &path, RouterHandler handler) { add_route("GET", path, std::move(handler)); }
    void post(const std::string &path, RouterHandler handler) { add_route("POST", path, std::move(handler)); }
    void put(const std::string &path, RouterHandler handler) { add_route("PUT", path, std::move(handler)); }
    void del(const std::string &path, RouterHandler handler) { add_route("DELETE", path, std::move(handler)); }

    void set_error_handler(RouterHandler handler) { error_handler_ = std::move(handler); }
    void set_not_found_handler(RouterHandler handler) { not_found_handler_ = std::move(handler); }

    SimpleHttpResponse handle(const SimpleHttpRequest &request) {
        SimpleHttpResponse response;

        const std::string key = request.method + ":" + request.path;
        const auto route = routes_.find(key);

        std::function<void(size_t)> chain = [&](size_t index) {
            if (index < middleware_.size()) {
                middleware_[index](request, response, [&] { chain(index + 1); });
            } else {
                if (route != routes_.end()) {
                    try {
                        route->second(request, response);
                    } catch (const std::exception &e) {
                        if (error_handler_) {
                            response = SimpleHttpResponse{};
                            error_handler_(request, response);
                        } else {
                            response.status_code = 500;
                            response.status_text = "Internal Server Error";
                            response.body = "Error: " + std::string(e.what());
                            response.content_type = "text/plain";
                        }
                    }
                } else {
                    if (not_found_handler_) {
                        not_found_handler_(request, response);
                    } else {
                        response.status_code = 404;
                        response.status_text = "Not Found";
                        response.body = "Not Found: " + request.path;
                        response.content_type = "text/plain";
                    }
                }
            }
        };

        chain(0);
        return response;
    }

private:
    void add_route(const std::string &method, const std::string &path, RouterHandler handler) {
        routes_[method + ":" + path] = std::move(handler);
    }

    std::unordered_map<std::string, RouterHandler> routes_;
    std::vector<MiddlewareFn> middleware_;
    RouterHandler not_found_handler_;
    RouterHandler error_handler_;
};
