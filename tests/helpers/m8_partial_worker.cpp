// Real Worker application with deterministic short send/EAGAIN at the M3 seam.
#include "ts/worker/application.h"
#include "../../src/apps/config_file.h"
#include <algorithm>
#include <cerrno>
#include <iostream>
#include <sys/socket.h>
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        auto parsed = ts::domain::WorkerConfig::parse(ts::apps::config_file(argv[1]));
        if (!parsed) throw std::runtime_error(parsed.error().message);
        ts::worker::ApplicationOptions options;
        options.socket_io.send = [calls = std::size_t{0}](int fd, const void* bytes, std::size_t size, int flags) mutable -> ssize_t {
            if (++calls % 7 == 0) { errno = EAGAIN; return -1; }
            return ::send(fd, bytes, std::min(size, std::size_t{97}), flags);
        };
        auto app = ts::worker::Application::create(parsed.value(), std::move(options));
        if (!app) throw std::runtime_error(app.error().message);
        return app.value()->run();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
