#include "ts/worker/application.h"
#include "config_file.h"
#include <iostream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "worker --config FILE\nLocal execution, cancellation and cleanup before session reconnect.\n"; return 0;
        }
        if (argc != 3 || std::string(argv[1]) != "--config") throw std::invalid_argument("use worker --help");
        auto parsed = ts::domain::WorkerConfig::parse(ts::apps::config_file(argv[2]));
        if (!parsed) throw std::invalid_argument(parsed.error().message);
        auto config = std::move(parsed).value();
        if (config.logging.destination != "journald" && config.logging.destination != "stderr")
            throw std::invalid_argument("M7 system logging supports stderr/journald capture only");
        spdlog::set_default_logger(spdlog::stderr_color_mt("worker"));
        spdlog::set_level(spdlog::level::from_str(config.logging.level));
        auto app = ts::worker::Application::create(std::move(config));
        if (!app) throw std::runtime_error(app.error().message);
        return app.value()->run();
    } catch (const std::exception& e) { std::cerr << "worker: " << e.what() << '\n'; return 1; }
}
