#pragma once

#include <cstdint>

#include "ts/common/result.h"

namespace ts::domain {

class ResourceCapacity {
public:
    static common::Result<ResourceCapacity> create(std::uint32_t cpu_slots,
                                                    std::uint64_t memory_mb);

    [[nodiscard]] std::uint32_t cpu_slots() const noexcept;
    [[nodiscard]] std::uint64_t memory_mb() const noexcept;

private:
    ResourceCapacity(std::uint32_t cpu_slots, std::uint64_t memory_mb) noexcept
        : cpu_slots_(cpu_slots), memory_mb_(memory_mb) {}

    std::uint32_t cpu_slots_;
    std::uint64_t memory_mb_;
};

class ResourceRequest {
public:
    static common::Result<ResourceRequest> create(std::uint32_t cpu_slots,
                                                  std::uint64_t memory_mb);

    [[nodiscard]] std::uint32_t cpu_slots() const noexcept;
    [[nodiscard]] std::uint64_t memory_mb() const noexcept;

private:
    ResourceRequest(std::uint32_t cpu_slots, std::uint64_t memory_mb) noexcept
        : cpu_slots_(cpu_slots), memory_mb_(memory_mb) {}

    std::uint32_t cpu_slots_;
    std::uint64_t memory_mb_;
};

class ResourceUsage {
public:
    static ResourceUsage zero() noexcept;
    static ResourceUsage from_values(std::uint32_t cpu_slots, std::uint64_t memory_mb) noexcept;

    [[nodiscard]] std::uint32_t cpu_slots() const noexcept;
    [[nodiscard]] std::uint64_t memory_mb() const noexcept;

private:
    ResourceUsage(std::uint32_t cpu_slots, std::uint64_t memory_mb) noexcept
        : cpu_slots_(cpu_slots), memory_mb_(memory_mb) {}

    std::uint32_t cpu_slots_;
    std::uint64_t memory_mb_;
};

[[nodiscard]] common::Result<ResourceUsage> add(ResourceUsage usage,
                                                 const ResourceRequest& request);
[[nodiscard]] common::Result<ResourceUsage> subtract(ResourceUsage usage,
                                                      const ResourceRequest& request);
[[nodiscard]] bool fits_within(const ResourceCapacity& capacity, ResourceUsage usage,
                               const ResourceRequest& request) noexcept;

}  // namespace ts::domain
