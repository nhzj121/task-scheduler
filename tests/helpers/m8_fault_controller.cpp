// Test-only syscall/commit gates. All decision logic, queues, file operations,
// protocol, TCP and SQLite are the REAL production application.
#include "ts/controller/application.h"
#include "../../src/apps/config_file.h"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>
namespace {
struct Gate {
    int notify, release;
    void wait() const {
        char byte = 'G';
        if (::write(notify, &byte, 1) != 1 || ::read(release, &byte, 1) != 1) throw std::runtime_error("test gate pipe");
    }
};
struct Files final : ts::logs::FilesystemHook {
    std::string mode;
    Gate gate;
    bool used{false};
    Files(std::string m, Gate g) : mode(std::move(m)), gate(g) {}
    bool matches(ts::logs::Operation op, const std::string& path) const {
        using O = ts::logs::Operation;
        const auto name = mode.substr(mode.find('-') + 1);
        if (name == "file-out") return op == O::FileSync && path.find(".stdout.log") != std::string::npos;
        if (name == "file-err") return op == O::FileSync && path.find(".stderr.log") != std::string::npos;
        if (name == "dir-root") return op == O::DirectorySync && path == ".";
        if (name == "dir-tasks") return op == O::DirectorySync && path == "tasks";
        if (name == "dir-task") return op == O::DirectorySync && path.rfind("tasks/", 0) == 0;
        if (name == "write" || name == "write-gated") return op == O::Write;
        if (name == "notify") return op == O::Notify;
        if (name == "create") return op == O::FileOpen && path.find(".stdout.log") != std::string::npos;
        return false;
    }
    ts::logs::FaultAction before(ts::logs::Operation op, const std::string& path, std::size_t) override {
        if (used || mode.rfind("after-", 0) == 0 || !matches(op, path)) return {};
        used = true;
        if (mode == "fail-write-gated") { gate.wait(); return {EIO, 0}; }
        if (mode.rfind("fail-", 0) == 0) return {EIO, 0};
        gate.wait(); return {};
    }
    int after(ts::logs::Operation op, const std::string& path) override {
        if (!used && mode.rfind("after-", 0) == 0 && matches(op, path)) { used = true; gate.wait(); }
        return 0;
    }
};
struct Sql final : ts::storage::SqliteHook {
    std::string mode; Gate gate; bool selected{false}, used{false};
    Sql(std::string m, Gate g) : mode(std::move(m)), gate(g) {}
    int before(ts::storage::SqlStage stage, const char* sql) override {
        using S = ts::storage::SqlStage;
        if (used) return SQLITE_OK;
        if (stage == S::Prepare && std::strstr(sql, "UPDATE attempts SET state=?") && std::strstr(sql, "finished_at=?")) selected = true;
        if (selected && ((mode == "terminal-commit" && stage == S::Commit) ||
                         (mode == "terminal-after" && stage == S::AfterCommit))) { used = true; gate.wait(); }
        return SQLITE_OK;
    }
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 5) return 2;
        const auto parsed = ts::domain::ControllerConfig::parse(ts::apps::config_file(argv[1]));
        if (!parsed) throw std::runtime_error(parsed.error().message);
        Gate gate{std::stoi(argv[3]), std::stoi(argv[4])};
        ts::controller::ApplicationOptions options;
        const std::string mode(argv[2]);
        if (mode.rfind("terminal-", 0) == 0) options.persistence_hook = std::make_shared<Sql>(mode, gate);
        else options.log_hook = std::make_shared<Files>(mode, gate);
        auto app = ts::controller::Application::create(parsed.value(), std::move(options));
        if (!app) throw std::runtime_error(app.error().message);
        return app.value()->run();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
