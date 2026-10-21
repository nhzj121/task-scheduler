#pragma once

#include "ts/storage/types.h"
#include "ts/protocol/messages.h"

namespace ts::controller {
// M5 already validates persisted values. Invalid metadata here is an explicit
// application invariant exception, never a silently repaired row.
[[nodiscard]] protocol::Task task_view(const storage::TaskRow& row);
[[nodiscard]] protocol::Attempt attempt_view(const storage::AttemptRow& row, bool finalizing);
[[nodiscard]] std::string page_token(const storage::TaskCursor& cursor);
[[nodiscard]] common::Result<storage::TaskCursor> parse_page_token(const std::string& token);
} // namespace ts::controller
