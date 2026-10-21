// Test-only composition: real application, real SQLite, deliberate gate/fault
// exclusively on PersistenceThread. No production CLI fault switch exists.
#include "ts/controller/application.h"
#include "../../src/apps/config_file.h"
#include <cstring>
#include <cerrno>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>
#include <sys/socket.h>
namespace {
struct Gate final : ts::storage::SqliteHook {
    std::string mode;
    int notify, release;
    bool selected{false}, used{false};
    Gate(std::string value, int out, int in) : mode(std::move(value)), notify(out), release(in) {}
    int before(ts::storage::SqlStage stage, const char* sql) override {
        using S = ts::storage::SqlStage;
        if (used) return SQLITE_OK;
        if (mode == "read-fail" && stage == S::Step && std::strstr(sql, "SELECT * FROM tasks WHERE")) return SQLITE_IOERR;
        if (mode != "hold-submit" && mode != "hold-assign" && mode != "fail-assign" && mode != "unknown-submit") return SQLITE_OK;
        const bool assign = mode.find("assign") != std::string::npos;
        if (stage == S::Step && std::strstr(sql, assign ? "INSERT INTO attempts" : "INSERT INTO tasks")) selected = true;
        if (selected && stage == S::Commit) {
            const char byte = 'G'; char answer = 0;
            if (::write(notify, &byte, 1) != 1 || ::read(release, &answer, 1) != 1) return SQLITE_IOERR;
            if (mode == "fail-assign") { used = true; return SQLITE_IOERR; }
            if (mode != "unknown-submit") used = true;
        }
        if (selected && mode == "unknown-submit" && stage == S::AfterCommit) { used = true; return SQLITE_IOERR; }
        return SQLITE_OK;
    }
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 5) return 2;
        auto config = ts::domain::ControllerConfig::parse(ts::apps::config_file(argv[1]));
        if (!config) throw std::runtime_error(config.error().message);
        ts::controller::ApplicationOptions options;
        if (std::string(argv[2]) == "metadata-bound") options.limits.restore_bytes = 16000;
        if (std::string(argv[2]) == "registration-bound") options.limits.registrations = 2;
        options.persistence_hook = std::make_shared<Gate>(argv[2], std::stoi(argv[3]), std::stoi(argv[4]));
        if (std::string(argv[2]) == "partial-run") {
            options.worker_io.send = [broken = false](int fd, const void* data, std::size_t size, int flags) mutable -> ssize_t {
                if (broken) { errno = EPIPE; return -1; }
                const auto* bytes = static_cast<const unsigned char*>(data);
                if (size >= 8 && bytes[0] == 'T' && bytes[1] == 'S' && bytes[6] == 2 && bytes[7] == 4) {
                    const auto sent = ::send(fd, data, 7, flags);
                    if (sent > 0) broken = true;
                    return sent;
                }
                return ::send(fd, data, size, flags);
            };
        }
        auto app = ts::controller::Application::create(config.value(), std::move(options));
        if (!app) throw std::runtime_error(app.error().message);
        return app.value()->run();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
