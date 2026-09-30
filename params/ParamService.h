#ifndef _PARAM_SERVICE_H
#define _PARAM_SERVICE_H

// ParamService.h
// Device side of remote parameter access: answers list/get/set requests on the
// DelegateMQ DataBus and publishes every change. Built only when PARAM_REMOTE=ON.
// Topics and messages are described in ParamMessages.h.
//
// - Remote set is off by default (AllowRemoteSet); an inbound set is a command
//   interface. Disabled sets reply REMOTE_DISABLED.
// - HIDDEN parameters are not listed, and get/set on them replies UNKNOWN_ID.
// - Remote sets go through ParamStore with Source::Remote, so READ_ONLY,
//   range and validator checks all apply.
//
// To reach tools over a network, also call ExposeParamsOnNetwork() on the
// node's NetworkNode.
//
// @see https://github.com/DelegateMQ/DelegateMQ

#include "ParamStore.h"
#include "ParamMessages.h"
#include <atomic>
#include <string>

namespace param {

class ParamService {
public:
    ParamService(ParamStore& store, const std::string& topicPrefix);
    ~ParamService();

    ParamService(const ParamService&) = delete;
    ParamService& operator=(const ParamService&) = delete;

    /// Subscribe to request topics and start publishing changes. Requests are
    /// handled on `thread` (synchronously on the publisher's thread if nullptr).
    void Start(dmq::IThread* thread = nullptr);
    void Stop();

    void AllowRemoteSet(bool allow) { m_allowRemoteSet = allow; }

private:
    void OnList(const ParamListReq& req);
    void OnGet(const ParamGetReq& req);
    void OnSet(const ParamSetReq& req);
    void OnChanged(ParamId id, Value v, Source src);

    /// Definition for a remotely visible parameter, or nullptr.
    const Def* Visible(ParamId id) const;
    void Reply(uint32_t requestId, ParamId id, SetResult result);

    ParamStore&       m_store;
    ParamTopics       m_topics;
    std::atomic<bool> m_allowRemoteSet{ false };

    dmq::ScopedConnection m_listConn;
    dmq::ScopedConnection m_getConn;
    dmq::ScopedConnection m_setConn;
    dmq::ScopedConnection m_changedConn;
};

} // namespace param

#endif
