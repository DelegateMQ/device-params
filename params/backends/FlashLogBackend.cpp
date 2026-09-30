#include "FlashLogBackend.h"

namespace param {

FlashLogBackend::FlashLogBackend(IFlash& flash) : m_flash(flash)
{
}

bool FlashLogBackend::ReadAll(dmq::UnicastDelegate<void(const Record&)>& sink)
{
    (void)sink;
    // TODO: find active sector, scan records, skip bad CRCs, deliver newest per ID
    return true;
}

bool FlashLogBackend::Write(const Record* recs, size_t count)
{
    (void)recs; (void)count;
    // TODO: append; compact into the other sector when full
    return true;
}

bool FlashLogBackend::Erase()
{
    bool ok = m_flash.EraseSector(0);
    ok = m_flash.EraseSector(1) && ok;
    m_activeSector = 0;
    m_writeOffset = 0;
    return ok;
}

} // namespace param
