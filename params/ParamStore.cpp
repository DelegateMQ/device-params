#include "ParamStore.h"
#include "Record.h"
#include "delegate-mq/extras/util/Fault.h"
#include <cmath>
#include <cstring>
#if PARAM_HAS_THREADS
#include <thread>
#endif

namespace param {

namespace {

bool Equal(const Value& a, const Value& b)
{
    if (a.type != b.type)
        return false;
    switch (a.type) {
    case TypeTag::Bool:   return a.b == b.b;
    case TypeTag::UInt32: return a.u == b.u;
    case TypeTag::Float:  return std::memcmp(&a.f, &b.f, sizeof(float)) == 0;
    case TypeTag::Int32:
    case TypeTag::Enum:   return a.i == b.i;
    }
    return false;
}

bool InRange(const Def& def, const Value& v)
{
    switch (def.type) {
    case TypeTag::Bool:   return true;
    case TypeTag::UInt32: return v.u >= def.minValue.u && v.u <= def.maxValue.u;
    case TypeTag::Float:  return !std::isnan(v.f) && v.f >= def.minValue.f && v.f <= def.maxValue.f;
    case TypeTag::Int32:
    case TypeTag::Enum:   return v.i >= def.minValue.i && v.i <= def.maxValue.i;
    }
    return false;
}

} // namespace

namespace detail {

Coalescer::Coalescer(size_t slots)
    : m_latest(slots), m_pending(slots, 0), m_seen(slots, 0)
{
}

bool Coalescer::Post(size_t slot, const Change& c)
{
    dmq::LockGuard<dmq::Mutex> lock(m_lock);
    // Ignore a change older than the one already held (concurrent setters can
    // notify out of order); the signed difference handles wraparound.
    if (m_seen[slot] && static_cast<int32_t>(c.seq - m_latest[slot].seq) <= 0)
        return false;
    m_latest[slot] = c;
    m_seen[slot] = 1;
    m_pending[slot] = 1;
    if (m_queued)
        return false;
    m_queued = true;
    return true;
}

void Coalescer::Drain(const std::function<void(const Change&)>& deliver)
{
    {
        dmq::LockGuard<dmq::Mutex> lock(m_lock);
        m_queued = false;   // changes posted from here on queue a new drain
    }
    for (size_t slot = 0; slot < m_latest.size(); slot++) {
        if (m_closed)
            return;     // unsubscribed: the target may no longer exist
        Change c;
        {
            dmq::LockGuard<dmq::Mutex> lock(m_lock);
            if (!m_pending[slot])
                continue;
            m_pending[slot] = 0;
            c = m_latest[slot];
        }
        deliver(c);     // without the lock: the callback may Set()
    }
}

void Coalescer::Close()
{
    m_closed = true;
}

#if PARAM_HAS_THREADS
bool DrainThread(dmq::IThread& thread, std::chrono::milliseconds budget)
{
    const auto end = std::chrono::steady_clock::now() + budget;
    auto marker = dmq::MakeDelegate(+[]() {}, thread, std::chrono::duration_cast<dmq::Duration>(budget));
    for (;;) {
        if (marker.AsyncInvoke().has_value())
            return true;    // marker ran: everything queued before it has run
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
#endif

} // namespace detail

ParamStore::ParamStore(const Def* defs, size_t count, IBackend* backend)
    : m_defs(defs), m_count(count), m_backend(backend)
{
    DMQ_ASSERT_TRUE(count <= MAX_COUNT);
}

ParamStore::~ParamStore()
{
    // Stop new deferred saves (including retries), then drain any Commit()
    // already queued on the save thread before this object goes away.
    m_destroying = true;
    m_saveTimer.Stop();
    m_saveTimerConn.Disconnect();
#if PARAM_HAS_THREADS
    if (m_saveThread && !m_saveThread->IsCurrentThread())
        detail::DrainThread(*m_saveThread);
#endif
    Commit();
}

void ParamStore::Init()
{
    {
        dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
        for (size_t i = 0; i < m_count; i++) {
            const Def& def = m_defs[i];

            // Table sanity: unique IDs, consistent types, default in range
            for (size_t j = 0; j < i; j++)
                DMQ_ASSERT_TRUE(m_defs[j].id != def.id);
            DMQ_ASSERT_TRUE(def.defValue.type == def.type);
            DMQ_ASSERT_TRUE(def.minValue.type == def.type && def.maxValue.type == def.type);
            DMQ_ASSERT_TRUE(InRange(def, def.defValue));

            m_values[i] = def.defValue;
            m_dirty[i] = false;
        }
    }

    if (m_backend) {
        dmq::UnicastDelegate<void(const Record&)> sink;
        sink = dmq::MakeDelegate(this, &ParamStore::OnLoadRecord);
        m_backend->ReadAll(sink);
    }
}

void ParamStore::OnLoadRecord(const Record& rec)
{
    const int index = IndexOf(rec.id);
    if (index < 0)
        return;     // parameter removed from the table; ignore

    Value v;
    SetResult r = DecodeRecord(rec, v) ? SetResult::OK : SetResult::REJECTED;
    const Def& def = m_defs[index];
    if (r == SetResult::OK) {
        if (v.type != def.type)
            r = SetResult::TYPE_MISMATCH;
        else if (!InRange(def, v))
            r = SetResult::OUT_OF_RANGE;
    }

    if (r != SetResult::OK) {
        Reject(rec.id, r, Source::Load);
        return;
    }

    dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
    m_values[index] = v;
}

void ParamStore::SetSaveMode(SaveMode mode, std::chrono::milliseconds delay, dmq::IThread* saveThread)
{
#if !PARAM_HAS_THREADS
    DMQ_ASSERT_TRUE(saveThread == nullptr);
#endif

    m_saveTimer.Stop();
    m_saveTimerConn.Disconnect();

    dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
    m_saveMode = mode;
    m_saveDelay = delay;
    m_saveThread = saveThread;
    m_saveScheduled = false;
    m_saveDue = false;

    if (mode != SaveMode::Deferred)
        return;
#if PARAM_HAS_THREADS
    if (saveThread) {
        m_saveTimerConn = m_saveTimer.OnExpired.Connect(
            dmq::MakeDelegate(this, &ParamStore::OnSaveTimer, *saveThread));
        return;
    }
#endif
    // Poll mode: runs in the ProcessTimers() context (possibly an ISR), so
    // only set a flag; Poll() does the write.
    m_saveTimerConn = m_saveTimer.OnExpired.Connect(
        dmq::MakeDelegate(this, &ParamStore::OnSaveDue));
}

int ParamStore::IndexOf(ParamId id) const
{
    for (size_t i = 0; i < m_count; i++) {
        if (m_defs[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

void ParamStore::CheckKey(ParamId id, TypeTag type) const
{
    const Def* def = Find(id);
    DMQ_ASSERT_TRUE(def != nullptr && def->type == type);
}

Value ParamStore::GetChecked(ParamId id, TypeTag type) const
{
    CheckKey(id, type);
    return GetValue(id);
}

Value ParamStore::GetValue(ParamId id) const
{
    const int index = IndexOf(id);
    DMQ_ASSERT_TRUE(index >= 0);
    dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
    return m_values[index];
}

SetResult ParamStore::Check(const Def& def, const Value& v, Source src) const
{
    if (v.type != def.type)
        return SetResult::TYPE_MISMATCH;
    if (src == Source::Remote && (def.flags & READ_ONLY))
        return SetResult::READ_ONLY;
    if (!InRange(def, v))
        return SetResult::OUT_OF_RANGE;
    if (m_validator && !m_validator(def.id, v))
        return SetResult::REJECTED;
    return SetResult::OK;
}

dmq::ScopedConnection ParamStore::ConnectThread(std::function<void(const detail::Change&)> deliver,
                                               [[maybe_unused]] dmq::IThread& thread, size_t slots, size_t filterIndex)
{
#if PARAM_HAS_THREADS
    auto mailbox = std::make_shared<detail::Coalescer>(slots);
    auto drain = dmq::DelegateFunctionAsync<void()>(
        std::function<void()>([mailbox, deliver]() { mailbox->Drain(deliver); }), thread);

    // Shared by every copy of the connected delegate; when the signal drops
    // the last copy (Disconnect / ScopedConnection destroyed), close the
    // mailbox so an already-queued drain delivers nothing.
    // (Constructed in place: a temporary would close the mailbox on the spot.)
    struct CloseOnRelease {
        explicit CloseOnRelease(std::shared_ptr<detail::Coalescer> m) : mailbox(std::move(m)) {}
        CloseOnRelease(const CloseOnRelease&) = delete;
        CloseOnRelease& operator=(const CloseOnRelease&) = delete;
        ~CloseOnRelease() { mailbox->Close(); }
        std::shared_ptr<detail::Coalescer> mailbox;
    };
    auto closer = std::make_shared<CloseOnRelease>(mailbox);

    return m_onChanged.Connect(ChangedDelegate([mailbox, drain, filterIndex, closer](const detail::Change& c) mutable {
        if (filterIndex != NO_FILTER && c.index != filterIndex)
            return;
        const size_t slot = (filterIndex == NO_FILTER) ? c.index : 0;
        if (mailbox->Post(slot, c))
            drain();
    }));
#else
    (void)deliver; (void)slots; (void)filterIndex;
    DMQ_ASSERT();   // no threads on bare metal: subscribe without a thread
    return {};
#endif
}

SetResult ParamStore::SetValue(ParamId id, const Value& v, Source src)
{
    const int index = IndexOf(id);
    if (index < 0) {
        Reject(id, SetResult::UNKNOWN_ID, src);
        return SetResult::UNKNOWN_ID;
    }

    SetResult r;
    bool changed = false;
    uint32_t seq = 0;
    {
        dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
        r = Check(m_defs[index], v, src);
        if (r == SetResult::OK && !Equal(m_values[index], v)) {
            m_values[index] = v;
            if (m_defs[index].flags & PERSIST)
                m_dirty[index] = true;
            seq = ++m_seq[index];
            changed = true;
        }
    }

    if (r != SetResult::OK)
        Reject(id, r, src);
    else if (changed)
        ApplyChange(index, v, src, seq);
    return r;
}

void ParamStore::ApplyChange(size_t index, const Value& v, Source src, uint32_t seq)
{
    // Called without the lock held
    detail::Change c;
    c.index = index;
    c.id = m_defs[index].id;
    c.value = v;
    c.src = src;
    c.seq = seq;
    m_onChanged(c);

    if (m_defs[index].flags & PERSIST) {
        if (m_saveMode == SaveMode::Immediate)
            Commit();
        else if (m_saveMode == SaveMode::Deferred)
            ScheduleSave();
    }
}

void ParamStore::ScheduleSave()
{
    {
        dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
        if (m_saveScheduled)
            return;     // coalesce: one pending save at a time
        m_saveScheduled = true;
    }
    m_saveTimer.Start(std::chrono::duration_cast<dmq::Duration>(m_saveDelay), true);
}

void ParamStore::OnSaveTimer()
{
    // Runs on the save thread
    Commit();
}

void ParamStore::OnSaveDue()
{
    m_saveDue = true;
}

bool ParamStore::Poll()
{
    if (!m_saveDue.exchange(false))
        return false;
    return Commit();
}

void ParamStore::Reject(ParamId id, SetResult r, Source src)
{
    OnRejected(id, r, src);
}

const Def* ParamStore::Find(ParamId id) const
{
    const int index = IndexOf(id);
    return index < 0 ? nullptr : &m_defs[index];
}

const Def* ParamStore::Find([[maybe_unused]] const char* name) const
{
#ifndef PARAM_NO_NAMES
    if (!name)
        return nullptr;
    for (size_t i = 0; i < m_count; i++) {
        if (m_defs[i].name && std::strcmp(m_defs[i].name, name) == 0)
            return &m_defs[i];
    }
#endif
    return nullptr;
}

const Def& ParamStore::DefAt(size_t index) const
{
    DMQ_ASSERT_TRUE(index < m_count);
    return m_defs[index];
}

void ParamStore::SetValidator(const dmq::UnicastDelegate<bool(ParamId, const Value&)>& validator)
{
    dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
    m_validator = validator;
}

bool ParamStore::HasUnsavedChanges() const
{
    dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
    for (size_t i = 0; i < m_count; i++) {
        if (m_dirty[i])
            return true;
    }
    return false;
}

bool ParamStore::Commit()
{
    dmq::LockGuard<dmq::Mutex> commitLock(m_commitLock);

    std::array<Record, MAX_COUNT> recs;
    std::array<size_t, MAX_COUNT> indexes;
    size_t n = 0;
    {
        dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
        m_saveScheduled = false;
        for (size_t i = 0; i < m_count; i++) {
            if (m_dirty[i]) {
                recs[n] = EncodeRecord(m_defs[i].id, m_values[i]);
                indexes[n] = i;
                m_dirty[i] = false;
                n++;
            }
        }
    }

    if (n == 0 || !m_backend)
        return true;

    if (!m_backend->Write(recs.data(), n)) {
        {
            // Mark unsaved again (already dirty if changed meanwhile)
            dmq::LockGuard<dmq::RecursiveMutex> lock(m_lock);
            for (size_t k = 0; k < n; k++)
                m_dirty[indexes[k]] = true;
        }
        OnCommitFailed();
        // Retry after the save delay; otherwise the changes would wait for an
        // unrelated future Set() to schedule another save
        if (m_saveMode == SaveMode::Deferred && !m_destroying)
            ScheduleSave();
        return false;
    }
    return true;
}

void ParamStore::ResetToDefaults(uint32_t flagMask)
{
    for (size_t i = 0; i < m_count; i++) {
        const Def& def = m_defs[i];
        if (flagMask == ALL || (def.flags & flagMask))
            SetValue(def.id, def.defValue, Source::Reset);
    }
}

} // namespace param
