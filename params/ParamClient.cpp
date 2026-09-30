#include "ParamClient.h"
#include "ParamStore.h"
#include <random>

using dmq::databus::DataBus;

namespace param {

ParamClient::ParamClient(const std::string& topicPrefix) : m_topics(topicPrefix)
{
    // Every client sees every reply on the topic, so request IDs must not
    // overlap between clients (or tool processes). Start at a random point:
    // two clients' ranges then collide only if they start within a few
    // thousand of each other out of 2^32.
    std::random_device rd;
    m_nextRequestId = rd();
}

ParamClient::~ParamClient()
{
    Stop();
}

void ParamClient::Start(dmq::IThread* thread)
{
    m_thread = thread;
    // Every callback goes through the gate, so none can run after Stop()
    auto gate = std::make_shared<detail::CallbackGate>();
    m_gate = gate;

    m_descConn = DataBus::Subscribe<ParamDescMsg>(m_topics.desc,
        [this, gate](const ParamDescMsg& msg) { gate->Run([&] { OnDesc(msg); }); }, thread);
    m_valueConn = DataBus::Subscribe<ParamValueMsg>(m_topics.value,
        [this, gate](const ParamValueMsg& msg) { gate->Run([&] { OnValue(msg); }); }, thread);
    m_changedConn = DataBus::Subscribe<ParamValueMsg>(m_topics.changed,
        [this, gate](const ParamValueMsg& msg) { gate->Run([&] { OnChanged(msg); }); }, thread);
}

void ParamClient::Stop()
{
    // Close first: waits for a callback already running on another thread,
    // and blocks any delivery that is in flight or still queued
    if (m_gate)
        m_gate->Close();
    m_gate.reset();

    m_descConn.Disconnect();
    m_valueConn.Disconnect();
    m_changedConn.Disconnect();

    // Replies queued before the disconnect still call into this object
    if (m_thread && !m_thread->IsCurrentThread())
        detail::DrainThread(*m_thread);
    m_thread = nullptr;
}

uint32_t ParamClient::NextRequestId()
{
    uint32_t id = m_nextRequestId++;
    if (id == 0)            // 0 is reserved for change notifications
        id = m_nextRequestId++;
    return id;
}

uint32_t ParamClient::RequestList()
{
    ParamListReq req;
    req.requestId = NextRequestId();
    DataBus::Publish(m_topics.list, req);
    return req.requestId;
}

uint32_t ParamClient::RequestGet(ParamId id)
{
    ParamGetReq req;
    req.requestId = NextRequestId();
    req.id = id;
    DataBus::Publish(m_topics.get, req);
    return req.requestId;
}

uint32_t ParamClient::RequestSet(ParamId id, const Value& v)
{
    ParamSetReq req;
    req.requestId = NextRequestId();
    req.id = id;
    req.SetValue(v);
    DataBus::Publish(m_topics.set, req);
    return req.requestId;
}

} // namespace param
