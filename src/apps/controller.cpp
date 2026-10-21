#include "ts/controller/application.h"
#include "config_file.h"

#include <iostream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "controller [--config FILE]\nM7 partial vertical slice; trusted loopback/private network only.\n"; return 0;
        }
        auto config = ts::domain::ControllerConfig::defaults();
        if (argc != 1) {
            if (argc != 3 || std::string(argv[1]) != "--config") throw std::invalid_argument("use controller --help");
            const auto parsed = ts::domain::ControllerConfig::parse(ts::apps::config_file(argv[2]));
            if (!parsed) throw std::invalid_argument(parsed.error().message);
            config = parsed.value();
        }
        if (config.logging.destination != "journald" && config.logging.destination != "stderr")
            throw std::invalid_argument("M7 system logging supports stderr/journald capture only");
        spdlog::set_default_logger(spdlog::stderr_color_mt("controller"));
        spdlog::set_level(spdlog::level::from_str(config.logging.level));
        auto app = ts::controller::Application::create(std::move(config));
        if (!app) throw std::runtime_error(app.error().message);
        return app.value()->run();
    } catch (const std::exception& e) { std::cerr << "controller: " << e.what() << '\n'; return 1; }
}
