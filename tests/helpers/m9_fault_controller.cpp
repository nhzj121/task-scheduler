// Test-only armed SQLite gates; the real Controller owns all decisions/queues.
#include "ts/controller/application.h"
#include "../../src/apps/config_file.h"
#include <cstring>
#include <iostream>
#include <poll.h>
#include <cerrno>
#include <sys/socket.h>
#include <sqlite3.h>
#include <unistd.h>
namespace {
struct Hook final : ts::storage::SqliteHook {
    std::string mode;
    int notify, release;
    bool armed{false}, selected{false}, used{false};
    Hook(std::string m, int n, int r) : mode(std::move(m)), notify(n), release(r) {}
    int before(ts::storage::SqlStage stage, const char* sql) override {
        using S = ts::storage::SqlStage;
        if (used) return SQLITE_OK;
        if (!armed) {
            pollfd p{release, POLLIN, 0};
            if (::poll(&p, 1, 0) > 0) { char c; if (::read(release, &c, 1) == 1 && c == 'A') armed = true; }
        }
        if (!armed) return SQLITE_OK;
        if (stage == S::Prepare && (mode.find("loss") != std::string::npos ?
            std::strstr(sql, "UPDATE attempts SET state='LOST'") != nullptr :
            mode == "hold-advance" ? std::strstr(sql, "UPDATE attempts SET state='STARTING'") != nullptr :
            std::strstr(sql, "UPDATE tasks SET state=?") != nullptr)) selected = true;
        if (!selected) return SQLITE_OK;
        if (stage == S::Commit) {
            if (mode == "after-task") return SQLITE_OK;
            char c = 'G';
            if (::write(notify, &c, 1) != 1 || ::read(release, &c, 1) != 1) return SQLITE_IOERR;
            if (mode.find("busy") != std::string::npos) { used = true; return SQLITE_BUSY; }
            if (mode.find("full") != std::string::npos) { used = true; return SQLITE_FULL; }
            if (mode.find("ioerr") != std::string::npos) { used = true; return SQLITE_IOERR; }
            if (mode.find("unknown") == std::string::npos) used = true;
        }
        if (stage == S::AfterCommit && mode == "after-task") {
            char c = 'G'; used = true;
            if (::write(notify, &c, 1) != 1 || ::read(release, &c, 1) != 1) return SQLITE_IOERR;
        }
        if (stage == S::AfterCommit && mode.find("unknown") != std::string::npos) { used = true; return SQLITE_IOERR; }
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
        const std::string mode(argv[2]);
        if (mode == "one-request") options.limits.requests_per_connection = 1;
        else options.persistence_hook = std::make_shared<Hook>(mode, std::stoi(argv[3]), std::stoi(argv[4]));
        if (mode == "partial-cancel") options.worker_io.send = [broken = false](int fd, const void* data, std::size_t size, int flags) mutable -> ssize_t {
            if (broken) { errno = EPIPE; return -1; }
            const auto* bytes = static_cast<const unsigned char*>(data);
            if (size >= 8 && bytes[0] == 'T' && bytes[1] == 'S' && bytes[6] == 2 && bytes[7] == 10) {
                const auto sent = ::send(fd, data, 7, flags); if (sent > 0) broken = true; return sent;
            }
            return ::send(fd, data, size, flags);
        };
        auto app = ts::controller::Application::create(config.value(), std::move(options));
        if (!app) throw std::runtime_error(app.error().message);
        return app.value()->run();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
