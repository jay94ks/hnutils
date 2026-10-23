#include <sdkv/change.hpp>

#include "state.hpp"

#include <utility>

namespace distkv {

/* Adopt an immutable owned event. */
CTableChange::CTableChange(std::shared_ptr<const Impl> impl) : _impl(std::move(impl)) {}

/* Share an immutable event. */
CTableChange::CTableChange(const CTableChange& other) noexcept = default;

/* Share an immutable event. */
CTableChange& CTableChange::operator=(const CTableChange& other) noexcept = default;

/* Release an event. */
CTableChange::~CTableChange() = default;

/* Return the exact binary key. */
const SBytes& CTableChange::key() const noexcept { return _impl->key; }

/* Return the previous value/revision. */
const std::optional<STableValue>& CTableChange::prev() const noexcept { return _impl->prev; }

/* Return the new value/revision. */
const std::optional<STableValue>& CTableChange::now() const noexcept { return _impl->now; }

/* Return the local revision, including a deletion revision. */
std::uint64_t CTableChange::revision() const noexcept { return _impl->revision; }

/* Distinguish a current-state replay from a new mutation. */
bool CTableChange::isReplay() const noexcept { return _impl->replay; }

} // namespace distkv
