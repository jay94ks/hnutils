#ifndef __INCLUDE_SDKV_CHANGE_HPP__
#define __INCLUDE_SDKV_CHANGE_HPP__

#include <sdkv/common.hpp>

#include <memory>

namespace distkv {

/**
 * Immutable, owned before/after images of a single effective change.
 *
 * An absent prev() means insertion; an absent now() means removal.
 * Notifications never require the listener to compare payloads.
 */
class SDKV_API CTableChange {
private:
    struct Impl;
    std::shared_ptr<const Impl> _impl;

    /** Construct a library-generated event with private propagation metadata. */
    explicit CTableChange(std::shared_ptr<const Impl> impl);

    friend class MemoryTable;

public:
    /** Copy an immutable event; it may safely be retained after the callback. */
    CTableChange(const CTableChange& other) noexcept;

    /** Copy an immutable event. */
    CTableChange& operator=(const CTableChange& other) noexcept;

    /** Release this event's shared payload. */
    ~CTableChange();

    /** Return the owned key without granting mutable table access. */
    const SBytes& key() const noexcept;

    /** Return the previous value/revision, or absence for an insertion. */
    const std::optional<STableValue>& prev() const noexcept;

    /** Return the new value/revision, or absence for a removal. */
    const std::optional<STableValue>& now() const noexcept;

    /** Return the key-local mutation revision, also defined for removals. */
    std::uint64_t revision() const noexcept;

    /** Test whether this is an initial-state replay, not a new mutation. */
    bool isReplay() const noexcept;
};

} // namespace distkv

#endif
