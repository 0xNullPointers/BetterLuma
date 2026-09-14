// BetterLumaCore - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "NetPacket_Cloud.h"
#include "NetPacket.h"
#include "runtime/CloudRedirectHost.h"
#include "runtime/Logger.h"
#include "runtime/Ticket.h"
#include "Steam/Structs.h"
#include "steam_messages.pb.h"

#include <deque>
#include <mutex>
#include <vector>
#include <cstring>

namespace {

    std::mutex g_queueMutex;
    std::deque<std::vector<uint8_t>> g_pending;

    std::mutex g_contextMutex;
    void* g_lastRecvThis = nullptr;
    HCONNECTION g_lastConnection = 0;
    uint8_t* g_lastNetworkBuffer = nullptr;
    NetPacket::Handlers::Cloud::RecvDispatcher_t g_recvDispatcher = nullptr;

    static bool SafeInvokeRecv(NetPacket::Handlers::Cloud::RecvDispatcher_t fn, void* pThis, CNetPacket* pPacket) {
        __try {
            return fn(pThis, pPacket);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool ReadVarint(const uint8_t* data, uint32_t size, uint32_t& pos, uint64_t& out) {
        out = 0;
        uint32_t shift = 0;
        while (pos < size && shift < 64) {
            uint8_t b = data[pos++];
            out |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
        }
        return false;
    }

    static bool FindVarintField(const uint8_t* d, uint32_t size, uint32_t target, uint64_t& out) {
        uint32_t pos = 0;
        while (pos < size) {
            uint64_t tag = 0;
            if (!ReadVarint(d, size, pos, tag)) return false;
            uint32_t field = static_cast<uint32_t>(tag >> 3);
            uint32_t wireType = static_cast<uint32_t>(tag & 7);
            if (field == target && wireType == 0)
                return ReadVarint(d, size, pos, out);

            switch (wireType) {
            case 0: { uint64_t tmp; if (!ReadVarint(d, size, pos, tmp)) return false; break; }
            case 1: if (size - pos < 8) return false; pos += 8; break;
            case 5: if (size - pos < 4) return false; pos += 4; break;
            case 2: {
                uint64_t len = 0;
                if (!ReadVarint(d, size, pos, len)) return false;
                if (len > size - pos) return false; // Bounds check wire length against remaining buffer
                pos += static_cast<uint32_t>(len);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    static uint32_t ExtractAppId(const char* jobName, const uint8_t* body, uint32_t cbBody) {
        uint32_t fieldNum = 1;
        if (jobName && std::strcmp(jobName, "Cloud.ClientCommitFileUpload#1") == 0)
            fieldNum = 2;
        uint64_t v = 0;
        if (FindVarintField(body, cbBody, fieldNum, v))
            return static_cast<uint32_t>(v);
        return 0;
    }

} // anonymous namespace

namespace NetPacket::Handlers::Cloud {

    void SetRecvContext(void* pThis, HCONNECTION hConn, uint8_t* pNetworkBuffer,
                        RecvDispatcher_t fn)
    {
        std::lock_guard<std::mutex> lk(g_contextMutex);
        g_lastRecvThis = pThis;
        g_lastConnection = hConn;
        g_lastNetworkBuffer = pNetworkBuffer;
        g_recvDispatcher = fn;
    }

    bool HasRecvContext()
    {
        std::lock_guard<std::mutex> lk(g_contextMutex);
        return g_lastRecvThis != nullptr && g_recvDispatcher != nullptr;
    }

    bool HandleSend(const char* jobName,
                    const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr)
    {
        if (!CloudRedirectHost::IsActive()) return false;
        if (!HasRecvContext()) {
            LOG_NETPACKET_WARN("Cloud: no recv context available to dispatch response for {}, passing through",
                               jobName ? jobName : "unknown");
            return false;
        }
        if (!jobName || strnlen(jobName, 128) >= 128 || !pHdr || cbHdr == 0) return false;

        CMsgProtoBufHeader reqHdr;
        if (!reqHdr.ParseFromArray(pHdr, cbHdr)) return false;

        uint64_t steamId = (reqHdr.has_steamid() && reqHdr.steamid())
            ? reqHdr.steamid()
            : Ticket::GetActiveSteamID64();
        uint32_t accountId = static_cast<uint32_t>(steamId & 0xFFFFFFFFull);
        if (accountId != 0) {
            CloudRedirectHost::SetAccountId(accountId);
        }

        const uint32_t appId = ExtractAppId(jobName, pBody, cbBody);
        if (appId == 0 || !CloudRedirectHost::IsApp(appId)) return false;

        static thread_local uint8_t respBuf[NetPacket::kBodyCap];
        uint32_t respLen = 0;
        int32_t  eresult = 2; // EResult::Fail

        if (!CloudRedirectHost::HandleCloudRpc(jobName, appId, accountId,
                                               pBody, cbBody,
                                               respBuf, static_cast<uint32_t>(sizeof(respBuf)),
                                               &respLen, &eresult)) {
            return false;
        }

        CMsgProtoBufHeader respHdr;
        if (reqHdr.has_jobid_source()) respHdr.set_jobid_target(reqHdr.jobid_source());
        respHdr.set_eresult(eresult);
        respHdr.set_target_job_name(jobName);

        const uint32_t cbRespHdr = static_cast<uint32_t>(respHdr.ByteSizeLong());
        const uint32_t total     = sizeof(MsgHdr) + cbRespHdr + respLen;
        if (cbRespHdr > NetPacket::kHdrCap || respLen > NetPacket::kBodyCap || total > NetPacket::kPktCap) {
            LOG_NETPACKET_WARN("Cloud: {} response too large ({} bytes), passing through", jobName, total);
            return false;
        }

        std::vector<uint8_t> pkt(total);
        auto* mhdr = reinterpret_cast<MsgHdr*>(pkt.data());
        mhdr->eMsg = static_cast<EMsg>(static_cast<uint32_t>(k_EMsgServiceMethodResponse) | kMsgHdrProtoFlag);
        mhdr->headerLength = cbRespHdr;
        if (!respHdr.SerializeToArray(pkt.data() + sizeof(MsgHdr), cbRespHdr))
            return false;
        if (respLen)
            std::memcpy(pkt.data() + sizeof(MsgHdr) + cbRespHdr, respBuf, respLen);

        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            if (g_pending.size() < 64)
                g_pending.push_back(std::move(pkt));
        }

        LOG_NETPACKET_DEBUG("Cloud: handled {} app={} -> queued {}-byte response (eresult={})",
                            jobName, appId, total, eresult);
        return true;
    }

    void Drain(void* pThis, CNetPacket* pCarrier,
               RecvDispatcher_t invokeOriginal)
    {
        if (!pCarrier || !invokeOriginal) return;

        // Drain up to queue capacity per cycle to prevent thread starvation
        for (size_t i = 0; i < 64; ++i) {
            std::vector<uint8_t> pkt;
            {
                std::lock_guard<std::mutex> lk(g_queueMutex);
                if (g_pending.empty()) return;
                pkt = std::move(g_pending.front());
                g_pending.pop_front();
            }

            if (pkt.empty() || pkt.size() > NetPacket::kPktCap)
                continue;

            uint8_t* origData = pCarrier->m_pubData;
            uint32_t origSize = pCarrier->m_cubData;
            pCarrier->m_pubData = pkt.data();
            pCarrier->m_cubData = static_cast<uint32_t>(pkt.size());
            SafeInvokeRecv(invokeOriginal, pThis, pCarrier);
            pCarrier->m_pubData = origData;
            pCarrier->m_cubData = origSize;
            LOG_NETPACKET_DEBUG("Cloud: delivered {}-byte response", pkt.size());
        }
    }

    void DrainImmediate()
    {
        static thread_local bool s_inDrain = false;
        if (s_inDrain) return;

        void* pThis = nullptr;
        HCONNECTION hConn = 0;
        uint8_t* pNetBuf = nullptr;
        RecvDispatcher_t invokeOriginal = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_contextMutex);
            pThis = g_lastRecvThis;
            hConn = g_lastConnection;
            pNetBuf = g_lastNetworkBuffer;
            invokeOriginal = g_recvDispatcher;
        }

        if (!pThis || !invokeOriginal) {
            LOG_NETPACKET_WARN("Cloud: DrainImmediate called without valid recv context");
            return;
        }

        struct DrainGuard {
            bool& flag;
            DrainGuard(bool& f) : flag(f) { flag = true; }
            ~DrainGuard() { flag = false; }
        } guard(s_inDrain);

        for (size_t i = 0; i < 64; ++i) {
            std::vector<uint8_t> pkt;
            {
                std::lock_guard<std::mutex> lk(g_queueMutex);
                if (g_pending.empty()) break;
                pkt = std::move(g_pending.front());
                g_pending.pop_front();
            }

            if (pkt.empty() || pkt.size() > NetPacket::kPktCap)
                continue;

            CNetPacket carrier{};
            carrier.m_hConnection = hConn;
            carrier.m_pubData = pkt.data();
            carrier.m_cubData = static_cast<uint32_t>(pkt.size());
            carrier.m_cRef = 1;
            carrier.m_pubNetworkBuffer = pNetBuf;
            carrier.m_pNext = nullptr;

            if (SafeInvokeRecv(invokeOriginal, pThis, &carrier)) {
                LOG_NETPACKET_DEBUG("Cloud: immediately delivered {}-byte response", pkt.size());
            } else {
                LOG_NETPACKET_WARN("Cloud: failed to immediately deliver {}-byte response", pkt.size());
            }
        }
    }

    void Reset() {
        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            g_pending.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_contextMutex);
            g_lastRecvThis = nullptr;
            g_lastConnection = 0;
            g_lastNetworkBuffer = nullptr;
            g_recvDispatcher = nullptr;
        }
    }

} // namespace NetPacket::Handlers::Cloud
