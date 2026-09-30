#include "ParamClient.h"

using dmq::databus::DataBus;

namespace param {

ParamClient::ParamClient(const std::string& topicPrefix) : m_topics(topicPrefix)
{
}

ParamClient::~ParamClient()
{
    Stop();
}

void ParamClient::Start(dmq::IThread* thread)
{
    m_descConn = DataBus::Subscribe<ParamDescMsg>(m_topics.desc,
        [this](const ParamDescMsg& msg) { OnDesc(msg); }, thread);
    m_valueConn = DataBus::Subscribe<ParamValueMsg>(m_topics.value,
        [this](const ParamValueMsg& msg) { OnValue(msg); }, thread);
    m_changedConn = DataBus::Subscribe<ParamValueMsg>(m_topics.changed,
        [this](const ParamValueMsg& msg) { OnChanged(msg); }, thread);
}

void ParamClient::Stop()
{
    m_descConn.Disconnect();
    m_valueConn.Disconnect();
    m_changedConn.Disconnect();
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
