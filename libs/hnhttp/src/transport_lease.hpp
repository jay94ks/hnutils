#ifndef __SRC_HNHTTP_TRANSPORT_LEASE_HPP__
#define __SRC_HNHTTP_TRANSPORT_LEASE_HPP__
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <stdexcept>
namespace hnhttp::detail {
inline void claimTransport(const std::shared_ptr<void>& resource, const std::shared_ptr<void>& owner) {
    struct SLeaseEntry { std::weak_ptr<void> resource, owner; };
    static std::mutex mutex;
    static std::vector<SLeaseEntry> entries;
    std::lock_guard lock(mutex);
    std::erase_if(entries,[](const auto& entry) { return entry.owner.expired() || entry.resource.expired(); });
    for (const auto& entry : entries) {
        if (!entry.resource.owner_before(resource) && !resource.owner_before(entry.resource)) { throw std::logic_error("transport already leased"); }
    }
    entries.push_back({resource,owner});
}
}
#endif
