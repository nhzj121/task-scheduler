#include "ts/domain/resources.h"

#include <limits>

namespace ts::domain {

common::Result<ResourceCapacity> ResourceCapacity::create(std::uint32_t cpu_slots,
                                                          std::uint64_t memory_mb) {
    if (cpu_slots == 0U) {
        return common::Result<ResourceCapacity>::failure(
            "resource capacity cpu_slots must be at least 1");
    }
    return common::Result<ResourceCapacity>::success(
        ResourceCapacity(cpu_slots, memory_mb));
}

std::uint32_t ResourceCapacity::cpu_slots() const noexcept {
    return cpu_slots_;
}

std::uint64_t ResourceCapacity::memory_mb() const noexcept {
    return memory_mb_;
}

common::Result<ResourceRequest> ResourceRequest::create(std::uint32_t cpu_slots,
                                                        std::uint64_t memory_mb) {
    if (cpu_slots == 0U) {
        return common::Result<ResourceRequest>::failure(
            "resource request cpu_slots must be at least 1");
    }
    return common::Result<ResourceRequest>::success(ResourceRequest(cpu_slots, memory_mb));
}

std::uint32_t ResourceRequest::cpu_slots() const noexcept {
    return cpu_slots_;
}

std::uint64_t ResourceRequest::memory_mb() const noexcept {
    return memory_mb_;
}

ResourceUsage ResourceUsage::zero() noexcept {
    return ResourceUsage(0U, 0U);
}

ResourceUsage ResourceUsage::from_values(std::uint32_t cpu_slots,
                                         std::uint64_t memory_mb) noexcept {
    return ResourceUsage(cpu_slots, memory_mb);
}

std::uint32_t ResourceUsage::cpu_slots() const noexcept {
    return cpu_slots_;
}

std::uint64_t ResourceUsage::memory_mb() const noexcept {
    return memory_mb_;
}

common::Result<ResourceUsage> add(ResourceUsage usage, const ResourceRequest& request) {
    if (request.cpu_slots() >
            std::numeric_limits<std::uint32_t>::max() - usage.cpu_slots() ||
        request.memory_mb() >
            std::numeric_limits<std::uint64_t>::max() - usage.memory_mb()) {
        return common::Result<ResourceUsage>::failure("resource usage overflow");
    }
    return common::Result<ResourceUsage>::success(ResourceUsage::from_values(
        usage.cpu_slots() + request.cpu_slots(), usage.memory_mb() + request.memory_mb()));
}

common::Result<ResourceUsage> subtract(ResourceUsage usage, const ResourceRequest& request) {
    if (request.cpu_slots() > usage.cpu_slots() || request.memory_mb() > usage.memory_mb()) {
        return common::Result<ResourceUsage>::failure("resource usage underflow");
    }
    return common::Result<ResourceUsage>::success(ResourceUsage::from_values(
        usage.cpu_slots() - request.cpu_slots(), usage.memory_mb() - request.memory_mb()));
}

bool fits_within(const ResourceCapacity& capacity, ResourceUsage usage,
                 const ResourceRequest& request) noexcept {
    if (usage.cpu_slots() > capacity.cpu_slots() || usage.memory_mb() > capacity.memory_mb()) {
        return false;
    }
    return request.cpu_slots() <= capacity.cpu_slots() - usage.cpu_slots() &&
           request.memory_mb() <= capacity.memory_mb() - usage.memory_mb();
}

}  // namespace ts::domain
