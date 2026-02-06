#pragma once
#include <functional>
#include <string>
#include <unordered_map>

#include "http_types.h"

// HTTP Router
class HttpRouter {
    public:
    using RouterHandler = std::function<void(const SimpleHttpRequest&,SimpleHttpResponse&)>;

    void get(const std::string& path,RouterHandler handler) {
        add_route("GET",path,std::move(handler));
    }

    void post(const std::string& path,RouterHandler handler) {
        add_route("POST",path,std::move(handler));
    }

    void put(const std::string& path,RouterHandler handler) {
        add_route("PUT",path,std::move(handler));
    }

    void del(const std::string& path,RouterHandler handler) {
        add_route("DELETE",path,std::move(handler));
    }

    SimpleHttpResponse handle(const SimpleHttpRequest& request) {
        SimpleHttpResponse response;

        std::string key = request.method + ":" + request.path;

        auto iter = routes_.find(key);
        if (iter != routes_.end()) {
            try {
                iter->second(request,response);
            }catch(const std::exception& e) {
                response.status_code = 500;
                response.status_text = "Internal Server Error";
                response.body = "Error: " + std::string(e.what());
            }
        }else {
            response.status_code = 404;
            response.status_text = "Not Found";
            response.body = "404 Not Found: " + request.path;

        }

        return response;
    }

    void set_error_handler(RouterHandler handler) {
        error_handler_ = std::move(handler);
    }

    void set_nnot_found_handler(RouterHandler handler) {
        not_found_handler_ = std::move(handler);
    }

private:
    void add_route(const std::string& method,const std::string& path,RouterHandler handler) {
        std::string key = method + ":" + path;
        routes_[key] = std::move(handler);
    }

    std::unordered_map<std::string,RouterHandler> routes_;
    RouterHandler not_found_handler_;
    RouterHandler error_handler_;
};