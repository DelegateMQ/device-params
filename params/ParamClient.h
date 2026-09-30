#ifndef _PARAM_CLIENT_H
#define _PARAM_CLIENT_H

// ParamClient.h
// Tool side of remote parameter access: sends list/get/set requests to a
// ParamService and reports replies and change notifications through signals.
// Replies carry the requestId returned by the Request*() call.
//
// To reach a device over a network, also call AttachParamClientToNetwork() on
// the tool's NetworkNode.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "ParamMessages.h"
#include <atomic>
#include <string>

namespace param {

class ParamClient {
public:
    explicit ParamClient(const std::string& topicPrefix);
    ~ParamClient();

    ParamClient(const ParamClient&) = delete;
    ParamClient& operator=(const ParamClient&) = delete;

    /// Subscribe to reply topics. Signals fire on `thread` (synchronously if nullptr).
    void Start(dmq::IThread* thread = nullptr);
    void Stop();

    uint32_t RequestList();
    uint32_t RequestGet(ParamId id);
    uint32_t RequestSet(ParamId id, const Value& v);

    dmq::Signal<void(const ParamDescMsg&)>  OnDesc;      ///< One per listed parameter
    dmq::Signal<void(const ParamValueMsg&)> OnValue;     ///< Get/set replies
    dmq::Signal<void(const ParamValueMsg&)> OnChanged;   ///< Change notifications

private:
    uint32_t NextRequestId();

    ParamTopics           m_topics;
    std::atomic<uint32_t> m_nextRequestId{ 1 };

    dmq::ScopedConnection m_descConn;
    dmq::ScopedConnection m_valueConn;
    dmq::ScopedConnection m_changedConn;
};

} // namespace param

#endif
