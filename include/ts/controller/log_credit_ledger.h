#pragma once
#include <map>
#include <thread>
#include "ts/common/result.h"
#include "ts/logs/writer.h"

namespace ts::controller {
struct LogCredit { std::uint64_t bytes{0}, slots{0}; };
struct CreditPartition { LogCredit free, granted, queued; };
class LogCreditLedger final {
public:
    LogCreditLedger(std::uint64_t bytes, std::uint64_t grantable_slots);
    [[nodiscard]] common::Result<void> reserve(const std::string& session, LogCredit initial);
    [[nodiscard]] common::Result<void> activate(const std::string& session, std::uint64_t generation);
    [[nodiscard]] common::Result<void> grant(const std::string& session, std::uint64_t generation, LogCredit credit);
    [[nodiscard]] common::Result<void> accept(std::uint64_t operation, const logs::Binding& binding, std::uint64_t raw_bytes);
    [[nodiscard]] common::Result<void> complete(std::uint64_t operation, const logs::Binding& binding, std::uint64_t raw_bytes);
    [[nodiscard]] bool reclaim(const std::string& session);
    [[nodiscard]] std::optional<LogCredit> outstanding(const std::string& session) const;
    [[nodiscard]] CreditPartition partition() const;
private:
    struct Lease { std::uint64_t generation; LogCredit credit; };
    struct Pending { logs::Binding binding; std::uint64_t bytes; };
    void owner() const;
    std::thread::id owner_{std::this_thread::get_id()};
    CreditPartition partition_;
    std::map<std::string, Lease> sessions_;
    std::map<std::uint64_t, Pending> pending_;
};
} // namespace ts::controller
