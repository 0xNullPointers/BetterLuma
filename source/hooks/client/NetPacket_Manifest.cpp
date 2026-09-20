// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/NetPacket.h"
#include "config/LuaLoader.h"
#include "runtime/ManifestFetch.h"
#include "runtime/ManifestCache.h"
#include "hooks/capture/RuntimeCapture.h"
#include "runtime/Logger.h"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace NetPacket::Handlers::DepotFallback {

namespace {
    struct JobInfo {
        AppId_t  depotId = 0;
        uint64_t gid     = 0;
        AppId_t  appId   = 0;
    };

    std::mutex g_jobMutex;
    std::map<uint64_t, JobInfo> g_jobs;
    constexpr size_t kMaxJobs = 256;

    void RecordJob(uint64_t jobId, AppId_t depotId, uint64_t gid, AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        if (g_jobs.size() >= kMaxJobs) {
            g_jobs.erase(g_jobs.begin());
        }
        g_jobs[jobId] = JobInfo{depotId, gid, appId};
    }

    std::optional<JobInfo> TakeJob(uint64_t jobId) {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        auto it = g_jobs.find(jobId);
        if (it == g_jobs.end()) return std::nullopt;
        JobInfo info = it->second;
        g_jobs.erase(it);
        return info;
    }
} // anonymous namespace

bool HandleSend(const uint8_t* pBody, uint32_t cbBody,
                const uint8_t* pHdr, uint32_t cbHdr) {
    CContentServerDirectory_GetManifestRequestCode_Request req;
    if (!req.ParseFromArray(pBody, cbBody)) {
        LOG_PKTRT_WARN("{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"err\":\"parse-fail\",\"size\":{}}}", cbBody);
        return false;
    }
    if (!req.has_depot_id() || !req.has_manifest_id()) {
        LOG_PKTRT_DEBUG("{{{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"skip\":\"no-depot-or-manifest\"}}}}");
        return false;
    }
    const AppId_t depotId = req.depot_id();
    const uint64_t gid    = req.manifest_id();
    const AppId_t appId   = req.has_app_id() ? req.app_id() : 0;

    const bool isTracked = LuaLoader::HasDepot(depotId)
                        || LuaLoader::IsOwned(depotId)
                        || LuaLoader::IsLuaTrackedApp(depotId)
                        || (appId != 0 && (LuaLoader::HasDepot(appId)
                                        || LuaLoader::IsOwned(appId)
                                        || LuaLoader::IsLuaTrackedApp(appId)))
                        || Settings::manifestCacheEnabled
                        || !Settings::manifestFetchUrls.empty();
    if (!isTracked) {
        LOG_PKTRT_DEBUG("{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"skip\":\"not-tracked\",\"depot\":{},\"app\":{},\"gid\":{}}}",
                   depotId, appId, gid);
        return false;
    }

    CMsgProtoBufHeader hdr;
    if (!hdr.ParseFromArray(pHdr, cbHdr) || !hdr.has_jobid_source()) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"err\":\"no-jobid\"}}}}");
        return false;
    }
    const uint64_t jobId = hdr.jobid_source();

    LOG_PKTRT_INFO("{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"depot\":{},\"gid\":{},\"app\":{},\"job\":{}}}",
               depotId, gid, appId, jobId);

    RecordJob(jobId, depotId, gid, appId);
    if (Settings::manifestCacheEnabled) {
        ManifestCache::EnsureCachedAsync(depotId, gid, appId, true);
    }
    if (!Settings::manifestFetchUrls.empty() || LuaLoader::HasManifestCodeFunc() || LuaLoader::HasManifestCodeFuncEx()) {
        ManifestFetch::Submit(jobId, gid, appId, depotId);
    }
    return false;
}

void HandleRecv(const uint8_t* pHdr, uint32_t cbHdr,
                const uint8_t* pBody, uint32_t cbBody) {
    CMsgProtoBufHeader hdr;
    if (!hdr.ParseFromArray(pHdr, cbHdr)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"err\":\"header-parse-fail\"}}}}");
        return;
    }
    if (!hdr.has_jobid_target()) {
        LOG_PKTRT_DEBUG("{{{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"skip\":\"no-jobid\"}}}}");
        return;
    }
    const uint64_t jobId = hdr.jobid_target();
    auto jobInfo = TakeJob(jobId);

    // If Valve approved the request natively with k_EResultOK (1), do NOT touch it!
    // The user owns the license on Steam and received a genuine manifest request code.
    if (hdr.eresult() == static_cast<int32_t>(k_EResultOK)) {
        LOG_PKTRT_DEBUG("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"status\":\"valve-ok\",\"job\":{}}}", jobId);
        ManifestFetch::Discard(jobId);
        return;
    }

    if (!jobInfo.has_value()) {
        LOG_PKTRT_DEBUG("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"skip\":\"untracked-job\",\"job\":{}}}", jobId);
        return;
    }

    const AppId_t depotId = jobInfo->depotId;
    const uint64_t gid    = jobInfo->gid;
    const AppId_t appId   = jobInfo->appId;
    const AppId_t targetAppId = (appId != 0) ? appId : depotId;

    // Tier 2: Try resolving request code from ManifestFetch (mirror servers / Lua)
    // Budget bounded to 2500ms so RecvPkt never stalls WebSocket heartbeat
    uint32_t fetchBudgetMs = static_cast<uint32_t>(
        (std::min)((std::max)(Settings::manifestFetchTimeoutSec * 1000, 500), 2500));
    auto resolved = ManifestFetch::Resolve(jobId, fetchBudgetMs);

    if (resolved.has_value()) {
        hdr.set_eresult(static_cast<int32_t>(k_EResultOK));
        const size_t hdrSize = hdr.ByteSizeLong();
        if (hdrSize > kHdrCap || !hdr.SerializeToArray(s_rx.Hdr, kHdrCap)) {
            LOG_PKTRT_WARN("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"err\":\"header-encode-fail\",\"size\":{}}}", hdrSize);
            return;
        }
        s_rx.HdrLen = static_cast<uint32_t>(hdrSize);

        CContentServerDirectory_GetManifestRequestCode_Response resp;
        resp.set_manifest_request_code(*resolved);
        const size_t bodySize = resp.ByteSizeLong();
        if (bodySize > kBodyCap || !resp.SerializeToArray(s_rx.Body, kBodyCap)) {
            LOG_PKTRT_WARN("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"err\":\"body-encode-fail\",\"size\":{}}}", bodySize);
            return;
        }
        s_rx.BodyLen = static_cast<uint32_t>(bodySize);

        s_rx.PatchHdr = true;
        s_rx.PatchBody = true;
        LOG_PKTRT_INFO("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"status\":\"code-resolved\",\"job\":{},\"code\":{},\"depot\":{},\"gid\":{}}}",
                   jobId, *resolved, depotId, gid);
        return;
    }

    // Tier 1: ManifestFetch had no code. Check if binary manifest is cached or about to finish in ManifestCache.
    if (Settings::manifestCacheEnabled) {
        bool cached = ManifestCache::IsCached(depotId, gid);
        if (!cached) {
            // Wait up to 1500ms for in-flight download to finish
            cached = ManifestCache::WaitForInflight(depotId, gid, 1500);
        }

        if (cached) {
            LOG_PKTRT_INFO("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"status\":\"manifest-cached\",\"app\":{},\"depot\":{},\"gid\":{}}}",
                           targetAppId, depotId, gid);
            // Manifest is verified on disk in depotcache/.
            // Schedule delayed re-evaluation to trigger Steam to re-check depotcache/ now that the file is present.
            std::thread([targetAppId]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                SteamCapture::RefreshAppUpdate(targetAppId);
            }).detach();
        } else {
            LOG_PKTRT_WARN("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"status\":\"exhausted\",\"depot\":{},\"gid\":{},\"app\":{}}}",
                           depotId, gid, targetAppId);
        }
    }
}

} // namespace NetPacket::Handlers::DepotFallback
