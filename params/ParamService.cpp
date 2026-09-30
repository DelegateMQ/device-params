#include "ParamService.h"
#include <algorithm>

using dmq::databus::DataBus;

namespace param {

ParamService::ParamService(ParamStore& store, const std::string& topicPrefix)
    : m_store(store), m_topics(topicPrefix)
{
}

ParamService::~ParamService()
{
    Stop();
}

void ParamService::Start(dmq::IThread* thread)
{
    m_listConn = DataBus::Subscribe<ParamListReq>(m_topics.list,
        [this](const ParamListReq& req) { OnList(req); }, thread);
    m_getConn = DataBus::Subscribe<ParamGetReq>(m_topics.get,
        [this](const ParamGetReq& req) { OnGet(req); }, thread);
    m_setConn = DataBus::Subscribe<ParamSetReq>(m_topics.set,
        [this](const ParamSetReq& req) { OnSet(req); }, thread);

    // Published on the thread that made the change
    m_changedConn = m_store.SubscribeAny(
        [this](ParamId id, Value v, Source src) { OnChanged(id, v, src); });
}

void ParamService::Stop()
{
    m_listConn.Disconnect();
    m_getConn.Disconnect();
    m_setConn.Disconnect();
    m_changedConn.Disconnect();
}

const Def* ParamService::Visible(ParamId id) const
{
    const Def* def = m_store.Find(id);
    return (def && !(def->flags & HIDDEN)) ? def : nullptr;
}

void ParamService::OnList(const ParamListReq& req)
{
    uint16_t count = 0;
    for (size_t i = 0; i < m_store.Count(); i++)
        if (!(m_store.DefAt(i).flags & HIDDEN))
            count++;

    uint16_t index = 0;
    for (size_t i = 0; i < m_store.Count(); i++) {
        const Def& def = m_store.DefAt(i);
        if (def.flags & HIDDEN)
            continue;

        ParamDescMsg msg;
        msg.requestId = req.requestId;
        msg.index = index++;
        msg.count = count;
        msg.id = def.id;
        msg.type = static_cast<uint8_t>(def.type);
        msg.flags = def.flags;
        msg.defBits = ToBits(def.defValue);
        msg.minBits = ToBits(def.minValue);
        msg.maxBits = ToBits(def.maxValue);
        msg.valueBits = ToBits(m_store.GetValue(def.id));
#ifndef PARAM_NO_NAMES
        if (def.name)
            CopyStr(msg.name, NAME_LEN, def.name);
        if (def.units)
            CopyStr(msg.units, UNITS_LEN, def.units);
#endif
        DataBus::Publish(m_topics.desc, msg);
    }
}

void ParamService::Reply(uint32_t requestId, ParamId id, SetResult result)
{
    ParamValueMsg msg;
    msg.requestId = requestId;
    msg.id = id;
    msg.result = static_cast<uint8_t>(result);
    if (Visible(id))
        msg.SetValue(m_store.GetValue(id));
    DataBus::Publish(m_topics.value, msg);
}

void ParamService::OnGet(const ParamGetReq& req)
{
    Reply(req.requestId, req.id, Visible(req.id) ? SetResult::OK : SetResult::UNKNOWN_ID);
}

void ParamService::OnSet(const ParamSetReq& req)
{
    SetResult result;
    bool ok = false;
    const Value v = req.GetValue(ok);

    if (!Visible(req.id))
        result = SetResult::UNKNOWN_ID;
    else if (!m_allowRemoteSet)
        result = SetResult::REMOTE_DISABLED;
    else if (!ok)
        result = SetResult::TYPE_MISMATCH;
    else
        result = m_store.SetValue(req.id, v, Source::Remote);

    Reply(req.requestId, req.id, result);
}

void ParamService::OnChanged(ParamId id, Value v, Source src)
{
    if (!Visible(id))
        return;

    ParamValueMsg msg;
    msg.id = id;
    msg.result = static_cast<uint8_t>(SetResult::OK);
    msg.source = static_cast<uint8_t>(src);
    msg.SetValue(v);
    DataBus::Publish(m_topics.changed, msg);
}

} // namespace param
