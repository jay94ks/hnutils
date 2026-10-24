#include <socketpp/watcher.hpp>

#include <socketpp/socket.hpp>

#include "platform.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
#include <map>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#elif !defined(_WIN32)
#include <sys/event.h>
#include <sys/time.h>
#endif

namespace sockpp {

namespace {

/** Convert a chrono timeout to the integer milliseconds used by native waits. */
int timeoutMilliseconds(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() < 0) {
        return -1;
    }
    const auto maximum = static_cast<std::chrono::milliseconds::rep>(
        (std::numeric_limits<int>::max)());
    return static_cast<int>((std::min)(timeout.count(), maximum));
}

} // namespace

struct CSocketWatcher::Impl {
    std::atomic<bool> stopped{false};
    using TimerKey=std::pair<std::chrono::steady_clock::time_point,std::uint64_t>;
    std::map<TimerKey,std::function<void()>> timers;
    std::map<std::uint64_t,TimerKey> timerIds;
    std::uint64_t nextTimer=1;
    bool runTimers() {
        // --> Snapshot before invoking: callbacks may cancel/reschedule other timers.
        bool ran=false;auto now=std::chrono::steady_clock::now();
        std::size_t budget=1024;
        while(budget--&&!timers.empty()&&timers.begin()->first.first<=now) {
            auto node=timers.extract(timers.begin());timerIds.erase(node.key().second);
            auto callback=std::move(node.mapped());ran=true;callback();
        }
        return ran;
    }

#if defined(_WIN32)
    HANDLE port = nullptr;
    std::mutex operationMutex;
    std::unordered_map<void*, std::function<void(std::size_t, std::error_code)>> operations;

    struct CallbackPacket {
        std::function<void()> callback;
    };

    static constexpr ULONG_PTR CALLBACK_KEY = 1;
    static constexpr ULONG_PTR STOP_KEY = 2;
#else
    struct WaitEntry {
        std::optional<asyncpp::promise<void>> read;
        std::optional<asyncpp::promise<void>> write;
    };

    std::mutex waitMutex;
    std::unordered_map<NativeSocket, WaitEntry> waits;
    std::mutex callbackMutex;
    std::deque<std::function<void()>> callbacks;

#if defined(__linux__)
    int poller = -1;
    int wake = -1;
    static constexpr std::uint64_t WAKE_TOKEN = (std::numeric_limits<std::uint64_t>::max)();
#else
    int poller = -1;
    static constexpr std::uintptr_t WAKE_TOKEN = 1;
#endif
#endif
};

/* Create a watcher using the current platform's native backend. */
CSocketWatcher::CSocketWatcher() : _impl(std::make_unique<Impl>()) {
    ensureSocketRuntime();

#if defined(_WIN32)
    _impl->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (_impl->port == nullptr) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "CreateIoCompletionPort");
    }
#elif defined(__linux__)
    _impl->poller = ::epoll_create1(EPOLL_CLOEXEC);
    if (_impl->poller < 0) {
        throwLastSocketError("epoll_create1");
    }
    _impl->wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (_impl->wake < 0) {
        const auto error = LastSocketError();
        ::close(_impl->poller);
        throw std::system_error(error, "eventfd");
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = Impl::WAKE_TOKEN;
    if (::epoll_ctl(_impl->poller, EPOLL_CTL_ADD, _impl->wake, &event) != 0) {
        const auto error = LastSocketError();
        ::close(_impl->wake);
        ::close(_impl->poller);
        throw std::system_error(error, "epoll_ctl(wake)");
    }
#else
    _impl->poller = ::kqueue();
    if (_impl->poller < 0) {
        throwLastSocketError("kqueue");
    }
    struct kevent event {};
    EV_SET(&event, Impl::WAKE_TOKEN, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(_impl->poller, &event, 1, nullptr, 0, nullptr) != 0) {
        const auto error = LastSocketError();
        ::close(_impl->poller);
        throw std::system_error(error, "kevent(wake)");
    }
#endif
}

/* Destroy the watcher. */
CSocketWatcher::~CSocketWatcher() {
    stop();
#if defined(_WIN32)
    if (_impl->port != nullptr) {
        ::CloseHandle(_impl->port);
        _impl->port = nullptr;
    }
#elif defined(__linux__)
    if (_impl->wake >= 0) {
        ::close(_impl->wake);
    }
    if (_impl->poller >= 0) {
        ::close(_impl->poller);
    }
#else
    if (_impl->poller >= 0) {
        ::close(_impl->poller);
    }
#endif
}

/* Return the selected native event backend. */
ESocketWatcherBackend CSocketWatcher::backend() const noexcept {
#if defined(_WIN32)
    return EWATCH_IOCP;
#elif defined(__linux__)
    return EWATCH_EPOLL;
#else
    return EWATCH_KQUEUE;
#endif
}

/* Queue a callback for the watcher thread. */
void CSocketWatcher::push(std::function<void()> callback) {
    if (!callback) {
        return;
    }

#if defined(_WIN32)
    auto packet = std::make_unique<Impl::CallbackPacket>();
    packet->callback = std::move(callback);
    if (!::PostQueuedCompletionStatus(
            _impl->port,
            0,
            Impl::CALLBACK_KEY,
            reinterpret_cast<OVERLAPPED*>(packet.get()))) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "PostQueuedCompletionStatus");
    }
    (void)packet.release();
#else
    {
        std::lock_guard lock(_impl->callbackMutex);
        _impl->callbacks.emplace_back(std::move(callback));
    }

#if defined(__linux__)
    const std::uint64_t value = 1;
    const auto ignored = ::write(_impl->wake, &value, sizeof(value));
    (void)ignored;
#else
    struct kevent event {};
    EV_SET(&event, Impl::WAKE_TOKEN, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    (void)::kevent(_impl->poller, &event, 1, nullptr, 0, nullptr);
#endif
#endif
}

/* Process callbacks and at most one batch of native events. */
std::uint64_t CSocketWatcher::schedule(std::chrono::steady_clock::time_point deadline,std::function<void()> callback) {
    if(asyncpp::dispatcher::current()!=this){throw std::logic_error("timer scheduling requires owner executor");}
    if(!callback||!_impl->nextTimer){throw std::invalid_argument("invalid timer callback or exhausted IDs");}
    auto id=_impl->nextTimer++;Impl::TimerKey key{deadline,id};
    _impl->timers.emplace(key,std::move(callback));try{_impl->timerIds.emplace(id,key);}catch(...){_impl->timers.erase(key);throw;}return id;
}
bool CSocketWatcher::cancelTimer(std::uint64_t id) {
    if(asyncpp::dispatcher::current()!=this){throw std::logic_error("timer cancellation requires owner executor");}
    auto i=_impl->timerIds.find(id);if(i==_impl->timerIds.end()){return false;}_impl->timers.erase(i->second);_impl->timerIds.erase(i);return true;
}
bool CSocketWatcher::runOne(std::chrono::milliseconds timeout) {
    asyncpp::dispatcher* const previous = asyncpp::dispatcher::current(this);
    try {
        const bool timersRan=_impl->runTimers();
        if(timersRan){timeout=std::chrono::milliseconds{0};}
        if(!_impl->timers.empty()) {
            auto delay=std::chrono::ceil<std::chrono::milliseconds>(_impl->timers.begin()->first.first-std::chrono::steady_clock::now());
            delay=(std::max)(delay,std::chrono::milliseconds{0});
            if(timeout.count()<0||delay<timeout){timeout=delay;}
        }
#if defined(_WIN32)
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const int timeoutValue = timeoutMilliseconds(timeout);
        const DWORD nativeTimeout = timeoutValue < 0 ? INFINITE : static_cast<DWORD>(timeoutValue);
        const BOOL success = ::GetQueuedCompletionStatus(
            _impl->port,
            &bytes,
            &key,
            &overlapped,
            nativeTimeout);
        const DWORD nativeError = success ? ERROR_SUCCESS : ::GetLastError();

        if (!success && overlapped == nullptr) {
            if (nativeError == WAIT_TIMEOUT) {
                const bool fired=_impl->runTimers();asyncpp::dispatcher::current(previous);return timersRan||fired;
            }
            asyncpp::dispatcher::current(previous);
            throw std::system_error(static_cast<int>(nativeError), std::system_category(),
                                    "GetQueuedCompletionStatus");
        }

        if (key == Impl::CALLBACK_KEY) {
            std::unique_ptr<Impl::CallbackPacket> packet(
                reinterpret_cast<Impl::CallbackPacket*>(overlapped));
            if (packet->callback) {
                packet->callback();
            }
        } else if (key != Impl::STOP_KEY && overlapped != nullptr) {
            std::function<void(std::size_t, std::error_code)> callback;
            {
                std::lock_guard lock(_impl->operationMutex);
                const auto found = _impl->operations.find(overlapped);
                if (found != _impl->operations.end()) {
                    callback = std::move(found->second);
                    _impl->operations.erase(found);
                }
            }
            if (callback) {
                const std::error_code error = nativeError == ERROR_SUCCESS
                    ? std::error_code{}
                    : std::error_code{static_cast<int>(nativeError), SocketErrorCategory()};
                callback(static_cast<std::size_t>(bytes), error);
            }
        }

        _impl->runTimers();asyncpp::dispatcher::current(previous);
        return true;
#elif defined(__linux__)
        std::array<epoll_event, 32> events{};
        const int count = ::epoll_wait(
            _impl->poller,
            events.data(),
            static_cast<int>(events.size()),
            timeoutMilliseconds(timeout));
        if (count < 0) {
            const auto error = LastSocketError();
            asyncpp::dispatcher::current(previous);
            if (error.value() == EINTR) {
                return false;
            }
            throw std::system_error(error, "epoll_wait");
        }

        std::vector<asyncpp::promise<void>> ready;
        bool woke = false;
        for (int index = 0; index < count; ++index) {
            const auto& event = events[static_cast<std::size_t>(index)];
            if (event.data.u64 == Impl::WAKE_TOKEN) {
                std::uint64_t value = 0;
                while (::read(_impl->wake, &value, sizeof(value)) > 0) {
                }
                woke = true;
                continue;
            }

            const NativeSocket socket = static_cast<NativeSocket>(event.data.u64);
            std::lock_guard lock(_impl->waitMutex);
            const auto found = _impl->waits.find(socket);
            if (found == _impl->waits.end()) {
                continue;
            }

            const bool failure = (event.events & (EPOLLERR | EPOLLHUP)) != 0;
            if ((failure || (event.events & EPOLLIN) != 0) && found->second.read) {
                ready.emplace_back(*found->second.read);
                found->second.read.reset();
            }
            if ((failure || (event.events & EPOLLOUT) != 0) && found->second.write) {
                ready.emplace_back(*found->second.write);
                found->second.write.reset();
            }

            std::uint32_t interest = EPOLLERR | EPOLLHUP | EPOLLONESHOT;
            if (found->second.read) {
                interest |= EPOLLIN;
            }
            if (found->second.write) {
                interest |= EPOLLOUT;
            }
            if (!found->second.read && !found->second.write) {
                (void)::epoll_ctl(_impl->poller, EPOLL_CTL_DEL, socket, nullptr);
                _impl->waits.erase(found);
            } else {
                epoll_event updated{};
                updated.events = interest;
                updated.data.u64 = static_cast<std::uint64_t>(socket);
                (void)::epoll_ctl(_impl->poller, EPOLL_CTL_MOD, socket, &updated);
            }
        }

        std::deque<std::function<void()>> callbacks;
        if (woke) {
            std::lock_guard lock(_impl->callbackMutex);
            callbacks.swap(_impl->callbacks);
        }
        for (auto& callback : callbacks) {
            callback();
        }
        for (auto& promise : ready) {
            (void)promise.try_fulfill();
        }

        const bool fired=_impl->runTimers();asyncpp::dispatcher::current(previous);
        return timersRan||fired||count > 0 || !callbacks.empty() || !ready.empty();
#else
        struct kevent event {};
        timespec nativeTimeout{};
        const timespec* timeoutPointer = nullptr;
        if (timeout.count() >= 0) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
            const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - seconds);
            nativeTimeout.tv_sec = static_cast<time_t>(seconds.count());
            nativeTimeout.tv_nsec = static_cast<long>(nanoseconds.count());
            timeoutPointer = &nativeTimeout;
        }

        const int count = ::kevent(_impl->poller, nullptr, 0, &event, 1, timeoutPointer);
        if (count < 0) {
            const auto error = LastSocketError();
            asyncpp::dispatcher::current(previous);
            if (error.value() == EINTR) {
                return false;
            }
            throw std::system_error(error, "kevent");
        }

        std::optional<asyncpp::promise<void>> ready;
        bool woke = false;
        if (count > 0 && event.filter == EVFILT_USER && event.ident == Impl::WAKE_TOKEN) {
            woke = true;
        } else if (count > 0) {
            const NativeSocket socket = static_cast<NativeSocket>(event.ident);
            std::lock_guard lock(_impl->waitMutex);
            const auto found = _impl->waits.find(socket);
            if (found != _impl->waits.end()) {
                auto& slot = event.filter == EVFILT_WRITE ? found->second.write : found->second.read;
                if (slot) {
                    ready = *slot;
                    slot.reset();
                }
                if (!found->second.read && !found->second.write) {
                    _impl->waits.erase(found);
                }
            }
        }

        std::deque<std::function<void()>> callbacks;
        if (woke) {
            std::lock_guard lock(_impl->callbackMutex);
            callbacks.swap(_impl->callbacks);
        }
        for (auto& callback : callbacks) {
            callback();
        }
        if (ready) {
            if ((event.flags & EV_ERROR) != 0 && event.data != 0) {
                ready->try_reject(std::make_exception_ptr(std::system_error(
                    static_cast<int>(event.data), SocketErrorCategory(), "kevent readiness")));
            } else {
                ready->try_fulfill();
            }
        }

        const bool fired=_impl->runTimers();asyncpp::dispatcher::current(previous);
        return timersRan||fired||count > 0 || !callbacks.empty() || ready.has_value();
#endif
    } catch (...) {
        asyncpp::dispatcher::current(previous);
        throw;
    }
}

/* Process callbacks and native events until stop() is called. */
void CSocketWatcher::run() {
    while (!_impl->stopped.load(std::memory_order_acquire)) {
        (void)runOne();
    }
}

/* Request event-loop termination and wake a blocked run() call. */
void CSocketWatcher::stop() noexcept {
    if (_impl->stopped.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

#if defined(_WIN32)
    (void)::PostQueuedCompletionStatus(_impl->port, 0, Impl::STOP_KEY, nullptr);
#elif defined(__linux__)
    const std::uint64_t value = 1;
    const auto ignored = ::write(_impl->wake, &value, sizeof(value));
    (void)ignored;
#else
    struct kevent event {};
    EV_SET(&event, Impl::WAKE_TOKEN, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    (void)::kevent(_impl->poller, &event, 1, nullptr, 0, nullptr);
#endif
}

/* Test whether stop() has been requested. */
bool CSocketWatcher::isStopped() const noexcept {
    return _impl->stopped.load(std::memory_order_acquire);
}

/* Register a one-shot readiness wait. */
asyncpp::promise<void> CSocketWatcher::wait(CSocket& socket, bool write) {
#if defined(_WIN32)
    (void)socket;
    (void)write;
    return asyncpp::promise<void>::make_rejected<std::logic_error>(
        "readiness waits are not used by the IOCP backend");
#else
    if (!socket.isOpen()) {
        return asyncpp::promise<void>::make_rejected<std::logic_error>("socket is closed");
    }

    asyncpp::promise<void> result;
    const NativeSocket nativeSocket = fromHandle(socket.nativeHandle());
    {
        std::lock_guard lock(_impl->waitMutex);
        auto& entry = _impl->waits[nativeSocket];
        auto& slot = write ? entry.write : entry.read;
        if (slot) {
            return asyncpp::promise<void>::make_rejected<std::logic_error>(
                write ? "a write operation is already pending" : "a read operation is already pending");
        }
        const bool wasEmpty = !entry.read && !entry.write;
        slot = result;

#if defined(__linux__)
        epoll_event event{};
        event.events = EPOLLERR | EPOLLHUP | EPOLLONESHOT;
        if (entry.read) {
            event.events |= EPOLLIN;
        }
        if (entry.write) {
            event.events |= EPOLLOUT;
        }
        event.data.u64 = static_cast<std::uint64_t>(nativeSocket);
        const int operation = wasEmpty ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
        if (::epoll_ctl(_impl->poller, operation, nativeSocket, &event) != 0) {
            const auto error = LastSocketError();
            slot.reset();
            if (!entry.read && !entry.write) {
                _impl->waits.erase(nativeSocket);
            }
            return asyncpp::promise<void>::make_rejected(
                std::make_exception_ptr(std::system_error(error, "epoll_ctl(socket)")));
        }
#else
        struct kevent event {};
        EV_SET(
            &event,
            static_cast<std::uintptr_t>(nativeSocket),
            write ? EVFILT_WRITE : EVFILT_READ,
            EV_ADD | EV_ONESHOT,
            0,
            0,
            nullptr);
        if (::kevent(_impl->poller, &event, 1, nullptr, 0, nullptr) != 0) {
            const auto error = LastSocketError();
            slot.reset();
            if (!entry.read && !entry.write) {
                _impl->waits.erase(nativeSocket);
            }
            return asyncpp::promise<void>::make_rejected(
                std::make_exception_ptr(std::system_error(error, "kevent(socket)")));
        }
#endif
    }
    return result;
#endif
}

/* Remove and reject POSIX readiness waits after the native handle has closed. */
void CSocketWatcher::cancelWaits(std::uintptr_t closedHandle) noexcept {
#if defined(_WIN32)
    (void)closedHandle; // closesocket delivers cancellation through the outstanding IOCP packets.
#else
    std::optional<asyncpp::promise<void>> read, write;
    const auto native = fromHandle(closedHandle);
    {
        std::lock_guard lock(_impl->waitMutex);
        const auto entry = _impl->waits.find(native);
        if (entry == _impl->waits.end()) { return; }
        read = std::move(entry->second.read);
        write = std::move(entry->second.write);
        // --> Native close removed kernel registrations before continuations can resume.
        _impl->waits.erase(entry);
    }
    const auto failure = std::make_exception_ptr(std::system_error(
        std::make_error_code(std::errc::operation_canceled), "socket closed during readiness wait"));
    if (read) { read->try_reject(failure); }
    if (write) { write->try_reject(failure); }
#endif
}

/* Associate a Windows socket with this watcher's completion port. */
void CSocketWatcher::associate(CSocket& socket) {
#if defined(_WIN32)
    if (!socket.isOpen()) {
        throw std::logic_error("socket is closed");
    }
    const HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(fromHandle(socket.nativeHandle())),
        _impl->port,
        0,
        0);
    if (result == nullptr) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                "CreateIoCompletionPort(socket)");
    }
#else
    (void)socket;
#endif
}

/* Register a Windows overlapped operation completion callback. */
void CSocketWatcher::submit(
    CSocket& socket,
    void* overlapped,
    std::function<void(std::size_t, std::error_code)> callback) {
#if defined(_WIN32)
    if (overlapped == nullptr || !callback) {
        throw std::invalid_argument("invalid overlapped operation");
    }
    (void)socket;
    std::lock_guard lock(_impl->operationMutex);
    if (!_impl->operations.emplace(overlapped, std::move(callback)).second) {
        throw std::logic_error("overlapped operation is already registered");
    }
#else
    (void)socket;
    (void)overlapped;
    (void)callback;
    throw std::logic_error("overlapped operations require the IOCP backend");
#endif
}

/* Remove an overlapped operation that failed before submission. */
void CSocketWatcher::cancel(void* overlapped) noexcept {
#if defined(_WIN32)
    std::lock_guard lock(_impl->operationMutex);
    _impl->operations.erase(overlapped);
#else
    (void)overlapped;
#endif
}

} // namespace sockpp
