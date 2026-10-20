#include <chrono>
#include <thread>
#include <unistd.h>
#include "ts/persistence/channel.h"

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    auto result = ts::persistence::Channel::start({argv[1], 50, "2026-10-19T00:00:00.000000Z", nullptr});
    if (!result) return 3;
    auto c = std::move(result).value();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (c->status().health == ts::storage::Health::Starting && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    auto s = c->status();
    if (argc == 3 && std::string(argv[2]) == "--seed-and-crash") {
        if (s.health != ts::storage::Health::Ready) return 5;
        const std::string task_id = "00000000-0000-4000-8000-000000000001";
        const std::string attempt_id = "00000000-0000-4000-8000-000000010001";
        const std::string session_id = "00000000-0000-4000-8000-000000001000";
        const std::string at = "2026-10-19T00:00:00.000000Z";
        ts::storage::TaskSpec task; task.task_id = task_id; task.executable = "/usr/bin/true";
        const ts::storage::Command commands[] = {ts::storage::Submit{task, at},
            ts::storage::Assign{{task_id, 0}, attempt_id, "worker-1", session_id, "absent/out", "absent/err", at}};
        std::uint64_t operation = 1;
        for (const auto& command : commands) {
            if (c->try_post({ts::storage::correlate(command, operation++), command, 512}) != ts::storage::Admission::Accepted) return 6;
            bool committed = false;
            while (std::chrono::steady_clock::now() < end) {
                auto completions = c->drain();
                if (!completions) return 7;
                if (!completions.value().empty()) { committed = completions.value()[0].outcome.committed; break; }
                std::this_thread::yield();
            }
            if (!committed) return 8;
        }
        // Intentional crash boundary: skip SQLite close, WAL checkpoint, channel
        // join and lock RAII. The parent must recover solely from committed WAL.
        ::_exit(0);
    }
    return s.error && s.error->code == ts::storage::ErrorCode::LockUnavailable ? 0 : 4;
}
