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

#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstring>
#include <chrono>
#include <atomic>
#include <memory>
#include <windows.h>

namespace {

    std::atomic<void*> g_lastRecvThis{nullptr};
    std::atomic<HCONNECTION> g_lastConnection{0};
    std::atomic<uint8_t*> g_lastNetBuf{nullptr};

    std::mutex g_stagedMutex;
    std::vector<std::vector<uint8_t>> g_stagedResponses;

    std::atomic<int32_t> g_inFlightDispatches{0};
    std::atomic<bool> g_dispatchShuttingDown{false};

    struct DispatchContext {
        std::vector<std::vector<uint8_t>> packets;
        NetPacket::Handlers::Cloud::RecvDispatcher_t dispatch;
    };

    struct PendingCloudResponse {
        uint32_t appId = 0;
        std::string jobName;
        std::vector<uint8_t> hdrBytes;
        std::vector<uint8_t> bodyBytes;
        std::chrono::steady_clock::time_point timestamp{};
    };

    std::mutex g_pendingMutex;
    std::unordered_map<uint64_t, PendingCloudResponse> g_pendingResponses;

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

    void SetRecvContext(void* pThis, HCONNECTION hConn, uint8_t* pNetworkBuffer)
    {
        if (pThis) {
            g_lastRecvThis.store(pThis, std::memory_order_release);
            g_lastConnection.store(hConn, std::memory_order_release);
            g_lastNetBuf.store(pNetworkBuffer, std::memory_order_release);
        }
    }

    bool HandleSend(const char* jobName,
                    const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr)
    {
        if (!CloudRedirectHost::IsActive()) return false;
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
        uint64_t jobId = reqHdr.has_jobid_source() ? reqHdr.jobid_source() : 0;
        if (jobId != 0) {
            respHdr.set_jobid_target(jobId);
        }
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
        if (respLen > 0)
            std::memcpy(pkt.data() + sizeof(MsgHdr) + cbRespHdr, respBuf, respLen);

        {
            std::lock_guard<std::mutex> lk(g_stagedMutex);
            if (g_stagedResponses.size() < 64) {
                g_stagedResponses.push_back(std::move(pkt));
            }
        }

        LOG_NETPACKET_DEBUG("Cloud: handled {} app={} (jobId={}) -> staged {}-byte wire response (eresult={})",
                            jobName, appId, jobId, total, eresult);
        return true;
    }

    void DispatchSynthesized(RecvDispatcher_t dispatch)
    {
        if (!dispatch) return;
        if (g_dispatchShuttingDown.load(std::memory_order_acquire)) return;

        std::vector<std::vector<uint8_t>> toDispatch;
        {
            std::lock_guard<std::mutex> lk(g_stagedMutex);
            if (g_stagedResponses.empty()) return;
            toDispatch = std::move(g_stagedResponses);
            g_stagedResponses.clear();
        }

        auto* ctx = new DispatchContext{std::move(toDispatch), dispatch};
        g_inFlightDispatches.fetch_add(1, std::memory_order_acq_rel);

        BOOL queued = QueueUserWorkItem([](LPVOID param) -> DWORD {
            std::unique_ptr<DispatchContext> c(static_cast<DispatchContext*>(param));
            struct InFlightScope {
                ~InFlightScope() {
                    g_inFlightDispatches.fetch_sub(1, std::memory_order_acq_rel);
                }
            } scope;

            // 1ms yield to let BBuildAndAsyncSendFrame complete and release transport locks
            Sleep(1);

            if (g_dispatchShuttingDown.load(std::memory_order_acquire))
                return 0;

            void* targetThis = g_lastRecvThis.load(std::memory_order_acquire);
            for (int retry = 0; retry < 5 && !targetThis; ++retry) {
                if (g_dispatchShuttingDown.load(std::memory_order_acquire))
                    return 0;
                Sleep(10);
                targetThis = g_lastRecvThis.load(std::memory_order_acquire);
            }

            if (!targetThis) {
                LOG_NETPACKET_WARN("Cloud: cannot dispatch synthesized response - receiver context (g_lastRecvThis) is null (offline/disconnected)");
                return 0;
            }

            HCONNECTION hConn = g_lastConnection.load(std::memory_order_acquire);
            uint8_t* pNetBuf = g_lastNetBuf.load(std::memory_order_acquire);

            for (const auto& pkt : c->packets) {
                if (g_dispatchShuttingDown.load(std::memory_order_acquire))
                    break;
                if (pkt.empty() || pkt.size() > NetPacket::kPktCap)
                    continue;

                CNetPacket carrier{};
                carrier.m_hConnection = hConn;
                carrier.m_pubData = const_cast<uint8_t*>(pkt.data());
                carrier.m_cubData = static_cast<uint32_t>(pkt.size());
                carrier.m_cRef = 1;
                carrier.m_pubNetworkBuffer = pNetBuf;
                carrier.m_pNext = nullptr;

                if (c->dispatch(targetThis, &carrier)) {
                    LOG_NETPACKET_DEBUG("Cloud: dispatched synthesized response ({} bytes) to oRecvPkt", pkt.size());
                } else {
                    LOG_NETPACKET_WARN("Cloud: failed to dispatch synthesized response ({} bytes)", pkt.size());
                }
            }
            return 0;
        }, ctx, WT_EXECUTEDEFAULT);

        if (!queued) {
            g_inFlightDispatches.fetch_sub(1, std::memory_order_acq_rel);
            delete ctx;
            LOG_NETPACKET_WARN("Cloud: QueueUserWorkItem failed (err={})", GetLastError());
        }
    }

    bool HandleRecv(const CMsgProtoBufHeader& inHdr,
                    const uint8_t* pBody, uint32_t cbBody)
    {
        uint64_t targetJob = inHdr.has_jobid_target() ? inHdr.jobid_target() : 0;

        PendingCloudResponse pending;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            if (targetJob != 0) {
                auto it = g_pendingResponses.find(targetJob);
                if (it != g_pendingResponses.end()) {
                    pending = std::move(it->second);
                    g_pendingResponses.erase(it);
                    found = true;
                }
            }
            // Fallback: if jobid_target didn't match directly but target_job_name starts with Cloud.
            if (!found && inHdr.has_target_job_name() && inHdr.target_job_name().rfind("Cloud.", 0) == 0) {
                const std::string& name = inHdr.target_job_name();
                for (auto it = g_pendingResponses.begin(); it != g_pendingResponses.end(); ++it) {
                    if (it->second.jobName == name) {
                        pending = std::move(it->second);
                        g_pendingResponses.erase(it);
                        found = true;
                        break;
                    }
                }
            }
        }

        if (!found) return false;

        if (pending.hdrBytes.size() > NetPacket::kHdrCap || pending.bodyBytes.size() > NetPacket::kBodyCap) {
            LOG_NETPACKET_WARN("Cloud: pending response too large for pool, skipping replace");
            return false;
        }

        std::memcpy(NetPacket::s_rx.Hdr, pending.hdrBytes.data(), pending.hdrBytes.size());
        NetPacket::s_rx.HdrLen = static_cast<uint32_t>(pending.hdrBytes.size());
        NetPacket::s_rx.PatchHdr = true;

        if (!pending.bodyBytes.empty()) {
            std::memcpy(NetPacket::s_rx.Body, pending.bodyBytes.data(), pending.bodyBytes.size());
            NetPacket::s_rx.BodyLen = static_cast<uint32_t>(pending.bodyBytes.size());
            NetPacket::s_rx.PatchBody = true;
        } else {
            NetPacket::s_rx.BodyLen = 0;
            NetPacket::s_rx.PatchBody = true;
        }

        LOG_NETPACKET_INFO("Cloud: replaced wire response for job {} ({}) app={} -> (hdr={} body={})",
                           targetJob, pending.jobName, pending.appId,
                           pending.hdrBytes.size(), pending.bodyBytes.size());
        return true;
    }

    void DrainDispatches() {
        g_dispatchShuttingDown.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(g_stagedMutex);
            g_stagedResponses.clear();
        }

        // Drain in-flight worker dispatches before unhooking or unloading (up to 1000ms)
        for (int i = 0; i < 200; ++i) {
            if (g_inFlightDispatches.load(std::memory_order_acquire) == 0)
                break;
            Sleep(5);
        }
    }

    void Init() {
        g_dispatchShuttingDown.store(false, std::memory_order_release);
    }

    void Reset() {
        DrainDispatches();
        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            g_pendingResponses.clear();
        }
        g_lastRecvThis.store(nullptr, std::memory_order_release);
        g_lastConnection.store(0, std::memory_order_release);
        g_lastNetBuf.store(nullptr, std::memory_order_release);
    }

} // namespace NetPacket::Handlers::Cloud
