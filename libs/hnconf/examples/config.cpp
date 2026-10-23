#include <hnconf.hpp>

#include <asyncpp/sync_wait.h>
#include <asyncpp/thread_pool.h>

#include <iostream>

/* Build and edit a snapshot, then serialize it on an asyncpp worker. */
int main() {
    try {
        auto config = hnconf::CConfig::parse(R"(
            worker_processes auto;
            http {
                server {
                    listen 8080;
                    server_name localhost;
                    location / { root "./www root"; }
                }
            }
        )");
        // --> Getters return copies; explicitly replace edited ancestors.
        auto http = config.nodes().at(config.find("http").at(0));
        auto servers = http.children();
        auto directives = servers.at(0).children();
        directives.at(0).setArguments({"9090"});
        servers.at(0).setChildren(std::move(directives));
        http.setChildren(std::move(servers));
        config.replaceNode(config.find("http").at(0), http);

        asyncpp::thread_pool workers(1);
        hnconf::CConfigAsync async(workers);
        std::cout << asyncpp::as_promise(async.serialize(config)).get();
        // --> async.load(path) and async.save(config, path) use this same worker.
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
