#ifndef __SRC_SDKV_STATE_HPP__
#define __SRC_SDKV_STATE_HPP__

#include <sdkv/change.hpp>
#include <sdkv/listener.hpp>
#include <sdkv/subscription.hpp>

#include <list>
#include <mutex>

namespace distkv {

struct CTableChange::Impl {
    SBytes key;
    std::optional<STableValue> prev;
    std::optional<STableValue> now;
    std::uint64_t revision = 0;
    std::uint64_t origin = 0;
    std::uint64_t sequence = 0;
    bool replay = false;
};

struct CTableSubscription::Impl {
    mutable std::mutex mutex;
    std::weak_ptr<ITableListener> listener;
    std::list<CTableChange> queue;
    bool active = true;
    bool draining = false;
    std::uint64_t errors = 0;
};

} // namespace distkv

#endif
