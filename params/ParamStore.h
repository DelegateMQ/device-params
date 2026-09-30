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
// - Subscribers receive callbacks on the thread they pass to Subscribe(), or
//   synchronously on the setting thread when no thread is given.
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

namespace param {

enum class SaveMode : uint8_t {
    Manual,     ///< Only Commit() writes to the backend
    Immediate,  ///< Every persistent change is written at once
    Deferred    ///< Changes are coalesced and written after a delay (flash wear)
};

/// Flag mask matching every parameter (for ResetToDefaults).
inline constexpr uint32_t ALL = 0xFFFFFFFFu;

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
    /// Only changes to `key` are dispatched to `thread`.
    template <class T, class F>
    [[nodiscard]] dmq::ScopedConnection Subscribe(Key<T> key, F&& fn, dmq::IThread* thread = nullptr) {
        CheckKey(key.id, TypeOf<T>::tag);
        std::function<void(T, Source)> target(std::forward<F>(fn));
        const ParamId id = key.id;
#if PARAM_HAS_THREADS
        if (thread) {
            auto async = dmq::DelegateFunctionAsync<void(T, Source)>(target, *thread);
            return m_onChanged.Connect(ChangedDelegate([id, async](ParamId pid, Value v, Source s) mutable {
                if (pid == id) async(v.template As<T>(), s);
            }));
        }
#else
        DMQ_ASSERT_TRUE(thread == nullptr);
#endif
        return m_onChanged.Connect(ChangedDelegate([id, target](ParamId pid, Value v, Source s) {
            if (pid == id) target(v.template As<T>(), s);
        }));
    }

    /// Callback fn(ParamId id, Value newValue, Source src) on `thread`.
    template <class F>
    [[nodiscard]] dmq::ScopedConnection SubscribeAny(F&& fn, dmq::IThread* thread = nullptr) {
        std::function<void(ParamId, Value, Source)> target(std::forward<F>(fn));
#if PARAM_HAS_THREADS
        if (thread)
            return m_onChanged.Connect(dmq::DelegateFunctionAsync<void(ParamId, Value, Source)>(target, *thread));
#else
        DMQ_ASSERT_TRUE(thread == nullptr);
#endif
        return m_onChanged.Connect(ChangedDelegate(target));
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

private:
    using ChangedDelegate = dmq::DelegateFunction<void(ParamId, Value, Source)>;

    int        IndexOf(ParamId id) const;
    Value      GetChecked(ParamId id, TypeTag type) const;
    void       CheckKey(ParamId id, TypeTag type) const;
    SetResult  Check(const Def& def, const Value& v, Source src) const;
    void       ApplyChange(size_t index, const Value& v, Source src);
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

    SaveMode      m_saveMode = SaveMode::Manual;
    dmq::IThread* m_saveThread = nullptr;
    std::chrono::milliseconds m_saveDelay{ SAVE_DELAY_MS };
    bool          m_saveScheduled = false;
    std::atomic<bool> m_saveDue{ false };
    dmq::util::Timer      m_saveTimer;
    dmq::ScopedConnection m_saveTimerConn;

    mutable dmq::UnicastDelegate<bool(ParamId, const Value&)> m_validator;
    dmq::Signal<void(ParamId, Value, Source)> m_onChanged;

    mutable dmq::RecursiveMutex m_lock;
    dmq::Mutex m_commitLock;    ///< Serializes backend writes
};

} // namespace param

#endif
