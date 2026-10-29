#include "ts/controller/log_credit_ledger.h"
#include <stdexcept>
#include "ts/domain/ids.h"
namespace ts::controller {
using R = common::Result<void>;
LogCreditLedger::LogCreditLedger(std::uint64_t bytes, std::uint64_t slots) : partition_{{bytes, slots}, {}, {}} {
    if (!bytes || !slots) throw std::invalid_argument("zero grantable log capacity");
}
void LogCreditLedger::owner() const { if (std::this_thread::get_id() != owner_) throw std::logic_error("wrong credit owner"); }
R LogCreditLedger::reserve(const std::string& session, LogCredit initial) {
    owner();
    if (!domain::SessionId::parse(session) || sessions_.count(session) || !initial.bytes || !initial.slots ||
        initial.bytes > partition_.free.bytes || initial.slots > partition_.free.slots) return R::failure("unbacked/duplicate initial log grant");
    sessions_.emplace(session, Lease{0, initial});
    partition_.free.bytes -= initial.bytes; partition_.free.slots -= initial.slots;
    partition_.granted.bytes += initial.bytes; partition_.granted.slots += initial.slots; return R::success();
}
R LogCreditLedger::activate(const std::string& session, std::uint64_t generation) {
    owner(); const auto it = sessions_.find(session);
    if (!generation || it == sessions_.end() || it->second.generation) return R::failure("invalid credit activation");
    it->second.generation = generation; return R::success();
}
R LogCreditLedger::grant(const std::string& session, std::uint64_t generation, LogCredit credit) {
    owner(); const auto it = sessions_.find(session);
    if (!generation || it == sessions_.end() || it->second.generation != generation || (!credit.bytes && !credit.slots) ||
        credit.bytes > partition_.free.bytes || credit.slots > partition_.free.slots) return R::failure("unbacked/stale log grant");
    it->second.credit.bytes += credit.bytes; it->second.credit.slots += credit.slots;
    partition_.free.bytes -= credit.bytes; partition_.free.slots -= credit.slots;
    partition_.granted.bytes += credit.bytes; partition_.granted.slots += credit.slots; return R::success();
}
R LogCreditLedger::accept(std::uint64_t operation, const logs::Binding& binding, std::uint64_t bytes) {
    owner(); const auto it = sessions_.find(binding.session);
    if (!operation || pending_.count(operation) || !bytes || bytes > 65536 || !binding.generation ||
        it == sessions_.end() || it->second.generation != binding.generation || bytes > it->second.credit.bytes ||
        !it->second.credit.slots || !logs::valid_binding(binding)) return R::failure("log credit transfer violation");
    pending_.emplace(operation, Pending{binding, bytes});
    it->second.credit.bytes -= bytes; --it->second.credit.slots;
    partition_.granted.bytes -= bytes; --partition_.granted.slots;
    partition_.queued.bytes += bytes; ++partition_.queued.slots; return R::success();
}
R LogCreditLedger::complete(std::uint64_t operation, const logs::Binding& binding, std::uint64_t bytes) {
    owner(); const auto it = pending_.find(operation);
    if (it == pending_.end() || !(it->second.binding == binding) || it->second.bytes != bytes) return R::failure("unknown/duplicate/stale log completion");
    pending_.erase(it); partition_.queued.bytes -= bytes; --partition_.queued.slots;
    partition_.free.bytes += bytes; ++partition_.free.slots; return R::success();
}
bool LogCreditLedger::reclaim(const std::string& session) {
    owner(); const auto it = sessions_.find(session); if (it == sessions_.end()) return false;
    const auto credit = it->second.credit;
    partition_.granted.bytes -= credit.bytes; partition_.granted.slots -= credit.slots;
    partition_.free.bytes += credit.bytes; partition_.free.slots += credit.slots;
    sessions_.erase(it); return true; // queued ownership is untouched
}
std::optional<LogCredit> LogCreditLedger::outstanding(const std::string& session) const {
    owner(); const auto it = sessions_.find(session);
    return it == sessions_.end() ? std::nullopt : std::optional<LogCredit>(it->second.credit);
}
CreditPartition LogCreditLedger::partition() const { owner(); return partition_; }
} // namespace ts::controller
