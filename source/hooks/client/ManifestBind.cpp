// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/ManifestBind.h"
#include "hooks/Macros.h"
#include "core/entry.h"
#include "runtime/ManifestCache.h"
#include "config/Settings.h"
#include <format>
#include <mutex>
#include <string>

// hook that patches depot gid/size in the output vector after Steam builds it.
// we don't hook BIsDlcEnabled / IsAppDlcInstalled / IsCloudEnabledForApp -
// CheckAppOwnership already covers those, adding em would be redundant.

namespace ManifestBind::Internal {

    constexpr uint32_t kDepotHardCap = 8192;

    // safe window over CUtlVector<DepotEntry> - Steam's internal layout
    class DepotBank {
        CUtlVector<DepotEntry>* m_store = nullptr;
        uint32_t m_items = 0;

    public:
        explicit DepotBank(CUtlVector<DepotEntry>* store) : m_store(store) {
            if (!m_store || !m_store->m_Size) return;
            m_items = m_store->m_Size;
            if (m_items > kDepotHardCap) {
                LOG_MANBND_WARN("BuildDepotDependency: clipping count {} to {}", m_items, kDepotHardCap);
                m_items = kDepotHardCap;
            }
            if (!m_store->m_Memory.m_pMemory) {
                LOG_MANBND_ERROR("BuildDepotDependency: backing memory is null");
                m_items = 0;
            }
        }

        bool HasEntries() const { return m_items > 0; }
        uint32_t Len() const { return m_items; }
        const DepotEntry& Get(uint32_t ix) const { return m_store->m_Memory.m_pMemory[ix]; }
        DepotEntry& Mut(uint32_t ix) { return m_store->m_Memory.m_pMemory[ix]; }

        std::string DumpEntry(uint32_t ix) const {
            const auto& e = Get(ix);
            return std::format("[DepotId={} | AppId={} | Gid={} | Size={} | Dlc={} | Lcs={} | Carry={} | Shared={}]",
                e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
                (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
        }
    };

    // walk the depot list and slap in any overrides from lua config
    static void SlapManifestOverrides(DepotBank& bank) {
        if (!bank.HasEntries()) return;
        const auto& overrides = LuaLoader::GetManifestOverrides();
        if (overrides.empty()) return;

        static std::once_flag s_once;
        std::call_once(s_once, [&]() {
            LOG_MANBND_INFO("manifest-map-dump size={}", static_cast<uint32_t>(overrides.size()));
            for (const auto& [k, v] : overrides)
                LOG_MANBND_INFO("manifest-map-entry depot={} gid={}", k, v.gid);
        });

        uint32_t idx = 0;
        while (idx < bank.Len()) {
            uint64_t key = static_cast<uint64_t>(bank.Get(idx).DepotId);
            auto it = overrides.find(key);
            if (it != overrides.end()) {
                uint64_t newSz = it->second.size ? it->second.size : bank.Get(idx).ManifestSize;
                LOG_MANBND_INFO("manifest-override depot={} gid={}->{} size={}->{}",
                    bank.Get(idx).DepotId, bank.Get(idx).ManifestGid, it->second.gid,
                    bank.Get(idx).ManifestSize, newSz);
                bank.Mut(idx).ManifestGid  = it->second.gid;
                bank.Mut(idx).ManifestSize = newSz;

                ManifestCache::EnsureCached(bank.Get(idx).DepotId, it->second.gid, bank.Get(idx).AppId);
            } else {
                LOG_MANBND_INFO("manifest-scan depot={} gid={} appid={} size={} mapSize={}",
                    bank.Get(idx).DepotId, bank.Get(idx).ManifestGid,
                    bank.Get(idx).AppId, bank.Get(idx).ManifestSize,
                    static_cast<uint32_t>(overrides.size()));
            }
            ++idx;
        }
    }

} // namespace ManifestBind::Internal

namespace {
    using ManifestBind::Internal::DepotBank;
    using ManifestBind::Internal::SlapManifestOverrides;

    LM_HOOK(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32_t* pBuildId, bool* pbBetaFallback)
    {
        bool ok = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        if (pDepotInfo) {
            DepotBank db(pDepotInfo);
            LOG_MANBND_TRACE("BuildDepotDependency appid={} depots={} ok={}", AppId, db.Len(), ok);
            if (ok) {
                SlapManifestOverrides(db);
                if (Settings::manifestCacheEnabled) {
                    for (uint32_t i = 0; i < db.Len(); ++i) {
                        const auto& e = db.Get(i);
                        if (e.DepotId != 0 && e.ManifestGid != 0 && !ManifestCache::IsCached(e.DepotId, e.ManifestGid)) {
                            ManifestCache::EnsureCachedAsync(e.DepotId, e.ManifestGid, e.AppId ? e.AppId : AppId, false);
                        }
                    }
                }
            }
        }
        return ok;
    }

    LM_HOOK(GetDepotManifest, int,
            void* pThis,
            const char* pchBranch,
            AppId_t appId,
            AppId_t depotId,
            uint64_t manifestGid,
            void* pOutput)
    {
        LOG_MANBND_TRACE("GetDepotManifest: app={} depot={} gid={} branch={}",
                         appId, depotId, manifestGid, pchBranch ? pchBranch : "null");

        if (Settings::manifestCacheEnabled && depotId != 0 && manifestGid != 0) {
            if (!ManifestCache::IsCached(depotId, manifestGid)) {
                LOG_MANBND_INFO("GetDepotManifest: precaching depot={} gid={} app={}", depotId, manifestGid, appId);
                ManifestCache::EnsureCached(depotId, manifestGid, appId ? appId : depotId, false);
            }
        }

        return oGetDepotManifest(pThis, pchBranch, appId, depotId, manifestGid, pOutput);
    }

} // anonymous namespace

namespace ManifestBind {

    void Install() {
        LM_TX_BEGIN();
        LM_INSTALL(BuildDepotDependency);
        LM_INSTALL(GetDepotManifest);
        LM_TX_COMMIT();
    }

    void Uninstall() {
        LM_TX_BEGIN();
        LM_REMOVE(BuildDepotDependency);
        LM_REMOVE(GetDepotManifest);
        LM_TX_COMMIT();
    }
}
