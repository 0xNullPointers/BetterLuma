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

namespace {

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

        std::vector<uint8_t> hdrBytes(cbRespHdr);
        if (!respHdr.SerializeToArray(hdrBytes.data(), cbRespHdr))
            return false;

        std::vector<uint8_t> bodyBytes;
        if (respLen > 0) {
            bodyBytes.assign(respBuf, respBuf + respLen);
        }

        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            auto now = std::chrono::steady_clock::now();
            std::erase_if(g_pendingResponses, [&now](const auto& kv) {
                return (now - kv.second.timestamp) > std::chrono::seconds(30);
            });

            if (jobId != 0) {
                g_pendingResponses[jobId] = PendingCloudResponse{
                    appId,
                    jobName,
                    std::move(hdrBytes),
                    std::move(bodyBytes),
                    now
                };
            }
        }

        LOG_NETPACKET_DEBUG("Cloud: handled {} app={} (jobId={}) -> prepared {}-byte response (eresult={})",
                            jobName, appId, jobId, total, eresult);
        return true;
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

    void Reset() {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_pendingResponses.clear();
    }

} // namespace NetPacket::Handlers::Cloud
