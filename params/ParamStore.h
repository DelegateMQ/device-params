#ifndef _PARAM_STORE_H
#define _PARAM_STORE_H

// ParamStore.h
// Typed, range-checked device parameters with change notification and
// pluggable persistence.
//
// Threading:
// - Get/Set take an internal recursive mutex, copy a Value and release it.
//   (Recursive so a validator may call Get.)
// - Change signals fire after the lock is released, so a subscriber may call Set.
// - Synchronous subscribers (no thread) are called once per change on the
//   setting thread. With concurrent setters, callbacks for the same parameter
//   can arrive out of order.
// - Thread subscribers are coalesced: each subscription has at most one
//   message queued on its thread, and delivers the latest value of each
//   changed parameter. Rapid intermediate values may be skipped, but the last
//   value is always delivered, in order (per-parameter sequence numbers), so a
//   flood of sets can't overflow the subscriber's queue.
// - Not ISR-safe.
// - Bare metal (DMQ_THREAD_NONE): no threads, so callbacks are always
//   synchronous (passing a thread asserts) and Deferred saves use Poll().
//
// Persistence:
// - Manual:    only Commit() writes to the backend.
// - Immediate: Set() commits on the calling thread before returning.
// - Deferred:  the first unsaved change starts a one-shot dmq::util::Timer.
//              When it expires, Commit() runs on the save thread, or, with no
//              save thread, the save becomes due and the app's next Poll()
//              call commits it (keeps flash writes out of the timer/ISR
//              context). The app must call dmq::util::Timer::ProcessTimers()
//              periodically.
// - The destructor drains any queued deferred save and commits what is left.
//   Do not destroy a store on its own save thread.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "Param.h"
#include "backends/IBackend.h"
#include "delegate-mq/DelegateMQ.h"
#include "delegate-mq/extras/util/Timer.h"
#include "delegate-mq/extras/util/Fault.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace param {

enum class SaveMode : uint8_t {
    Manual,     ///< Only Commit() writes to the backend
    Immediate,  ///< Every persistent change is written at once
    Deferred    ///< Changes are coalesced and written after a delay (flash wear)
};

/// Flag mask matching every parameter (for ResetToDefaults).
inline constexpr uint32_t ALL = 0xFFFFFFFFu;

namespace detail {

/// One change, as passed to subscribers internally.
struct Change {
    size_t   index = 0;     ///< Position in the definition table
    ParamId  id = 0;
    Value    value;
    Source   src = Source::Local;
    uint32_t seq = 0;       ///< Per-parameter change sequence number
};

/// Latest-value mailbox for one thread subscription (non-templated to keep
/// per-type code small). Post() runs on the setting thread, Drain() on the
/// subscriber's thread.
class Coalescer {
public:
    explicit Coalescer(size_t slots);

    /// Record a change if it is newer than the one held for its slot.
    /// Returns true if the caller must dispatch a Drain() to the thread.
    bool Post(size_t slot, const Change& c);

    /// Deliver every pending change (latest per slot) and go idle.
    void Drain(const std::function<void(const Change&)>& deliver);

    /// Called when the subscription is disconnected: a drain already queued
    /// on the thread then delivers nothing.
    void Close();

private:
    std::atomic<bool>    m_closed{ false };
    dmq::Mutex           m_lock;
    std::vector<Change>  m_latest;
    std::vector<uint8_t> m_pending;
    std::vector<uint8_t> m_seen;
    bool                 m_queued = false;
};

/// Guards an object's callbacks against running after the object stops.
/// Disconnecting a delegate doesn't wait for a delivery another thread has
/// already started, so callbacks go through Run(): it calls the function only
/// while the gate is open, under the gate's lock. Close() takes the same lock,
/// so it waits for a running callback to finish, and nothing runs afterwards.
/// Recursive, so a callback may re-enter (e.g. trigger another callback).
class CallbackGate {
public:
    template <class F>
    void Run(F&& fn) {
        dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
        if (m_open)
            fn();
    }
    void Close() {
        dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
        m_open = false;
    }

private:
    dmq::RecursiveMutex m_lock;
    bool m_open = true;
};

#if PARAM_HAS_THREADS
/// Block until every message already queued on `thread` has run, so an object
/// whose delegates may still be queued there can be destroyed safely. Retries
/// while the queue is full (a DROP-policy thread drops the marker itself).
/// Returns false if not drained within `budget` (e.g. the thread has exited,
/// which also means nothing more will run). Must not be called on `thread`.
bool DrainThread(dmq::IThread& thread,
                 std::chrono::milliseconds budget = std::chrono::milliseconds(2000));
#endif

} // namespace detail

class ParamStore {
public:
    template <size_t N>
    explicit ParamStore(const Def (&defs)[N], IBackend* backend = nullptr)
        : ParamStore(defs, N, backend) {
        static_assert(N <= MAX_COUNT, "Too many parameters; raise PARAM_MAX_COUNT");
    }

    ParamStore(const Def* defs, size_t count, IBackend* backend = nullptr);
    ~ParamStore();

    ParamStore(const ParamStore&) = delete;
    ParamStore& operator=(const ParamStore&) = delete;

    /// Validate the table, load defaults, then overlay persisted records.
    /// Call once, before subscribing or setting. Does not fire change signals.
    /// Rejected records (wrong type, out of range, bad CRC) fire OnRejected
    /// with Source::Load and keep the default.
    void Init();

    /// @param saveThread  Deferred only: the thread that runs Commit(). If
    ///                    nullptr, Commit() runs from Poll() once the save is due.
    void SetSaveMode(SaveMode mode,
                     std::chrono::milliseconds delay = std::chrono::milliseconds(SAVE_DELAY_MS),
                     dmq::IThread* saveThread = nullptr);

    // Typed access (thin wrappers over the untyped calls below)
    template <class T>
    T Get(Key<T> key) const {
        return GetChecked(key.id, TypeOf<T>::tag).template As<T>();
    }

    template <class T>
    SetResult Set(Key<T> key, IdentityT<T> value, Source src = Source::Local) {
        if constexpr (std::is_enum_v<T>)
            return SetValue(key.id, Value::FromEnum(value), src);
        else
            return SetValue(key.id, Value(value), src);
    }

    // Untyped access, used by ParamService and tools
    SetResult  SetValue(ParamId id, const Value& v, Source src);
    Value      GetValue(ParamId id) const;
    const Def* Find(ParamId id) const;
    const Def* Find(const char* name) const;
    size_t     Count() const { return m_count; }
    const Def& DefAt(size_t index) const;

    /// Callback fn(T newValue, Source src) on `thread` (synchronous if nullptr).
    /// Only changes to `key` are dispatched to `thread` (coalesced, see above).
    template <class T, class F>
    [[nodiscard]] dmq::ScopedConnection Subscribe(Key<T> key, F&& fn, dmq::IThread* thread = nullptr) {
        CheckKey(key.id, TypeOf<T>::tag);
        std::function<void(T, Source)> target(std::forward<F>(fn));
        const size_t index = static_cast<size_t>(IndexOf(key.id));
        std::function<void(const detail::Change&)> deliver = [target](const detail::Change& c) {
            target(c.value.template As<T>(), c.src);
        };
        if (thread)
            return ConnectThread(std::move(deliver), *thread, 1, index);
        return m_onChanged.Connect(ChangedDelegate([index, deliver](const detail::Change& c) {
            if (c.index == index) deliver(c);
        }));
    }

    /// Callback fn(ParamId id, Value newValue, Source src) on `thread`
    /// (synchronous if nullptr; coalesced per parameter on a thread).
    template <class F>
    [[nodiscard]] dmq::ScopedConnection SubscribeAny(F&& fn, dmq::IThread* thread = nullptr) {
        std::function<void(ParamId, Value, Source)> target(std::forward<F>(fn));
        std::function<void(const detail::Change&)> deliver = [target](const detail::Change& c) {
            target(c.id, c.value, c.src);
        };
        if (thread)
            return ConnectThread(std::move(deliver), *thread, m_count, NO_FILTER);
        return m_onChanged.Connect(ChangedDelegate(deliver));
    }

    /// Optional veto for rules spanning several parameters (return false to
    /// reject with SetResult::REJECTED). Runs under the store lock on the
    /// setting thread; may call Get, must not block.
    void SetValidator(const dmq::UnicastDelegate<bool(ParamId, const Value&)>& validator);

    /// Deferred mode without a save thread: commit if a deferred save is due.
    /// Call from the main loop / a low-priority task. Returns true if it committed.
    bool Poll();

    /// Write unsaved PERSIST changes to the backend now. Returns false on a
    /// backend error (changes stay unsaved and are retried on the next commit).
    bool Commit();

    /// True if a PERSIST parameter has changed since the last successful Commit().
    bool HasUnsavedChanges() const;

    /// Reset parameters whose flags intersect flagMask to their defaults
    /// (ALL for every parameter). Fires change signals with Source::Reset.
    void ResetToDefaults(uint32_t flagMask = PERSIST);

    /// Fired when a set or loaded record is rejected.
    dmq::Signal<void(ParamId, SetResult, Source)> OnRejected;

    /// Fired when Commit() fails to write to the backend (on the committing
    /// thread). In Deferred mode the save is retried every save delay until it
    /// succeeds.
    dmq::Signal<void()> OnCommitFailed;

private:
    using ChangedDelegate = dmq::DelegateFunction<void(const detail::Change&)>;
    static constexpr size_t NO_FILTER = static_cast<size_t>(-1);

    /// Connect a coalesced thread subscription. filterIndex selects one
    /// parameter (slot 0), or NO_FILTER for all (slot = table index).
    dmq::ScopedConnection ConnectThread(std::function<void(const detail::Change&)> deliver,
                                        dmq::IThread& thread, size_t slots, size_t filterIndex);

    int        IndexOf(ParamId id) const;
    Value      GetChecked(ParamId id, TypeTag type) const;
    void       CheckKey(ParamId id, TypeTag type) const;
    SetResult  Check(const Def& def, const Value& v, Source src) const;
    void       ApplyChange(size_t index, const Value& v, Source src, uint32_t seq);
    void       OnLoadRecord(const Record& rec);
    void       ScheduleSave();
    void       OnSaveTimer();
    void       OnSaveDue();
    void       Reject(ParamId id, SetResult r, Source src);

    const Def* m_defs;
    size_t     m_count;
    IBackend*  m_backend;

    std::array<Value, MAX_COUNT> m_values{};
    std::array<bool, MAX_COUNT>  m_dirty{};
    std::array<uint32_t, MAX_COUNT> m_seq{};

    SaveMode      m_saveMode = SaveMode::Manual;
    dmq::IThread* m_saveThread = nullptr;
    std::chrono::milliseconds m_saveDelay{ SAVE_DELAY_MS };
    bool          m_saveScheduled = false;
    std::atomic<bool> m_saveDue{ false };
    std::atomic<bool> m_destroying{ false };
    dmq::util::Timer      m_saveTimer;
    dmq::ScopedConnection m_saveTimerConn;

    mutable dmq::UnicastDelegate<bool(ParamId, const Value&)> m_validator;
    dmq::Signal<void(const detail::Change&)> m_onChanged;

    mutable dmq::RecursiveMutex m_lock;
    dmq::Mutex m_commitLock;    ///< Serializes backend writes
};

} // namespace param

#endif
