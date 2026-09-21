# Changelog

## v0.5

### Play Button Redirection & Security Hardening (`-playbtn`)
- **Custom Executable Redirection (`-playbtn="<path>"`)**: Implemented dynamic executable redirection within Steam's `SpawnProcess` Detours hook. Clicking "Play" in Steam can now launch custom frontends, OnlineFix `Launcher.exe`, or script extenders (e.g., SKSE, F4SE) while keeping the original game's Steam Job Object, playtime tracking, Steam overlay, and achievement routing intact.
- **Filesystem Jail & Security Hardening**: Enforced canonical containment (`std::filesystem::canonical` and `IsSubpath`), strictly restricting `-playbtn` to `.exe` binaries residing within the game's authentic install directory. Completely mitigates path traversal (`..`), symlink/NTFS junction escapes, network UNC shares (`\\`), and Alternate Data Streams (ADS).

### 32-Bit (WOW64) Architecture & Cross-Bitness Injection
- **Dual-Architecture Payload Delivery (`BetterLumaPayload32.dll`)**: Expanded the payload layer to full 32-bit (x86/WOW64) support. Steam client automatically inspects target process architecture upon spawn and injects the matching 32-bit or 64-bit payload module via PE import directory rewriting.
- **Decorated Export Demangling & Calling Convention Alignment**: Implemented automatic resolution of `__stdcall` decorated exports (`_Name@N`) for 32-bit `EOSSDK-Win32-Shipping.dll` alongside standard 64-bit exports, ensuring seamless hook binding across both architectures.
- **Versioned Steam Persona Fallback**: Integrated multi-version fallback resolution for `ISteamFriends` (`SteamAPI_SteamFriends_v017` / `v016` / `v015`), maintaining persona name synchronization across legacy and modern Steamworks binaries.
- **Zero-Footprint CMake Detours Patching**: Automated 32-bit import update patching (`UpdateImports32`) via CMake configure-time string replacement with fail-loud validation, eliminating third-party source duplication in the repository.

### EOS Profile & Guest Credential Persistence
- **Device ID Deletion Suppression (`EOS_Connect_DeleteDeviceId`)**: Hooked and neutralized `EOS_Connect_DeleteDeviceId` inside `EpicOnlineBridge`. Preserves local EOS Device ID tokens and persistent PUIDs across sessions, eliminating repetitive username prompts and guest profile wipes on startup (e.g. in *Among Us*).

### Manifest Engine, Local Disk Caching & Provider Expansion
- **On-Disk Manifest Caching (`GetDepotManifest`)**: Hooked `GetDepotManifest` to store and load depot manifests from local disk cache, accelerating game file verifications and eliminating redundant network manifest requests.
- **Provider Expansion & API Key Integration**: Added native support for ManifestDeX, Hubcap, and Manifesthub providers, complete with configurable API keys via `BetterLuma.toml` and updated HTTP User-Agent routing.

### Spacewar Cloud Sync Error Suppression
- **Spacewar (480) Cloud Frame Filtering**: Intercepted and blocked outbound cloud sync queries for AppID 480 during OnlineFix gameplay, resolving exit errors and spurious cloud sync failure notifications in the Steam UI.

## v0.4

### Global Achievement Caching & UI Display
- **Global Achievement Percentages (`GlobalAchievementManager`)**: Implemented an achievement aggregation and caching subsystem that fetches, computes, and locally caches global achievement unlock percentages.
- **SteamUI & Overlay Rarity Integration**: Hooked client achievement IPC dispatch and SteamUI routines (`GlobalAchievementHooks`), populating true global rarity percentages directly within Steam's library UI, detail panels, and in-game overlay.
- **Thread-Safe Local Cache Ingestion**: Achievement rarity data is cached atomically on disk with instant in-memory lookup, preventing UI stalls during offline or low-connectivity gameplay.

### Workshop & UGC IPC Auto-Healing
- **Sign-Extended ID Sanitization (`PublishedFileId_t` / `UGCHandle_t`)**: Solved Workshop item loading and map selection failures caused by 32-bit integer sign-extension into 64-bit handle space (`0xFFFFFFFF...`). Implemented an auto-healing layer in `IPCBus` that normalizes sign-extended PublishedFileIds and UGC handles before dispatch.
- **Null Safety & Buffer Bounds Enforcement**: Hardened deserialization routines across all `IPCBus` message handlers with strict bounds checking and null pointer validation, preventing buffer over-reads on malformed or truncated IPC payloads.

### OnlineFix AppID Routing for Workshop & Cloud
- **Context-Aware Genuine AppID Resolution**: Decoupled multiplayer Spacewar (480) network identity from Workshop and cloud storage subsystems. IPC requests for UGC queries, Workshop subscriptions, and save path resolution now resolve to the title's genuine AppID.
- **Workshop Map & Skin Mounting**: Enables games operating under OnlineFix Spacewar proxying to access, download, and mount subscribed Workshop maps and user-created assets using their authentic Steam directory paths.

### LoaderGate Synchronization & Hook Readiness
- **Dual Client & UI Readiness Signalling**: Refactored `LoaderGate` synchronization into dedicated client and UI readiness phases, ensuring hook installation across `lcoverlay.dll` is verified and signaled before unblocking host UI threads.
- **Resilient Bootstrap Hook Sequencing**: Eliminated race windows between Detours hook attachment and late-loading Steam client workers through explicit synchronization primitives.

### Core Architecture & Rebranding
- **Official Rebrand to BetterLuma**: Standardized project targets, payload modules (`BetterLumaPayload.dll`), proxy gateways (`BetterLuma.dll`), and namespaces across the entire codebase.
- **Codebase Clean-Up**: Removed obsolete diagnostics subsystems and purged legacy duplicate source trees, optimizing memory footprint and build times.
- **Compiler & Linker Optimization**: Refined MSVC compiler optimization and linker stripping flags, achieving a lean ~1.0 MB binary size for Release builds.
- **Unified SHA-256 Hashing (`HashUtil`)**: Consolidated disparate cryptographic hashing routines into a high-performance, unified utility for IPC spec validation and pattern verification.

### Automated Deployment & Installer Tooling
- **Automated PowerShell Deployment Script (`Install-BetterLuma.ps1`)**: Introduced an automated installer script supporting one-liner execution.
- **Exclusive Registry Discovery**: Detects the Steam installation directory directly from the Windows Registry.
- **Interactive Build Selection**: Allows to select between the recommended optimized Release build and the developer Debug build.
- **Safe Collision Handling & Self-Deleting Uninstaller**: Automatically backs up conflicting DLLs and `opensteamtool.dll` to `.bak` (replacing existing BetterLuma DLLs directly) and generates a self-deleting uninstaller (`uninstall-BetterLuma.bat`).

## v0.3

### Startup Synchronization & Race Elimination (Loader Gate)
- **Kernel-Level Loader Gate (`LoaderGate`)**: Implemented a synchronization gate hooking `kernelbase.dll!LoadLibraryExW` using Microsoft Detours during `DLL_PROCESS_ATTACH`. Any host thread attempting to load `steamclient64.dll` or `steamui.dll` is held on a kernel event (`g_hBootstrapReadyEvent`) until BetterLuma's background initialization, pattern fetching, and critical hooks are fully operational.
- **Cold-Start Race Condition Elimination**: Solved the critical cold-start race condition present in LumaCore, where first-time launches without cached pattern TOMLs failed to hook Steam or capture Package 0. By gating the host process while network pattern downloads complete, `steamui.dll` is prevented from loading the unhooked `steamclient64.dll` ahead of BetterLuma.
- **Zero-Latency Fast Path for Cached Launches**: When pattern TOMLs are already cached locally, initialization finishes in under 800 ms—well before Steam attempts to map its UI libraries. The event gate signals immediately, ensuring subsequent launches run with 0 ms hold latency.
- **Automatic Transparent Module Diversion**: Gated `steamclient64.dll` load requests are automatically diverted to `bin\lcoverlay.dll` with fallback ref-counting on `diversion_hModule`, ensuring all UI and internal client components operate on the hooked overlay binary.

### Pattern Fetcher & On-Disk Ingestion
- **Synchronous On-Disk Module Hashing (`LoadForPath`)**: Decoupled `PatternFetcher` from active in-memory module handles (`HMODULE`). Unlike LumaCore which relied on in-memory module handles (creating circular startup dependencies), BetterLuma can now hash and download patterns for `<SteamInstallPath>\steamui.dll` directly from disk during bootstrap alongside `steamclient64.dll`.
- **Instant In-Memory Module Association (`AssociateModule`)**: Introduced fast module-to-pattern association, binding live `steamui.dll` module handles to pre-fetched on-disk pattern tables instantaneously upon load with zero network overhead.
- **Accelerated Fallback Retry Loop**: Reduced the deferred SteamUI retry loop polling interval from 500 ms to 20 ms, minimizing attach latency in the event of asynchronous late mapping.

### SteamUI Hook Resiliency & Diversion Hardening
- **Path-Agnostic Basename Normalization**: Replaced strict exact string matching in `LoadModuleWithPath` with robust case-insensitive basename parsing (`IsSteamClient64`), properly handling absolute paths, relative paths, forward/backward slashes, and case variations.
- **Datafile Mapping Guard**: Filtered out `LOAD_LIBRARY_AS_DATAFILE`, `LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE`, and `LOAD_LIBRARY_AS_IMAGE_RESOURCE` flags in the loader gate, preventing unwanted hook attachment when modules are mapped as non-executable resources.
- **Thread-Safe Worker Bypass**: Registered the background initialization thread ID (`g_initThreadId`) to bypass the loader gate completely, preventing self-deadlocks during `Diversion::PrepareAndLoad`.

### Build System & Version Management
- **Dynamic Release Versioning**: Streamlined CMake version resolution to consume `RELEASE_VERSION` directly from environment variables and Git tags without hardcoded or misleading fallback version strings.
- **Clean Debug Stamp Identification**: Properly isolates local development and debug builds, annotating them with clean build stamps rather than static release version metadata.
- **Release Logging Sanitization**: Hardened per-channel logging macros to compile down to `((void)0)` under Release configurations, guaranteeing zero runtime overhead and clean compilation across all build profiles.

## v0.2

### Process Injection & PE Loader Architecture
- **Static Import Table Injection (`DetourUpdateProcessWithDll`)**: Replaced LumaCore's `CreateRemoteThread` DLL injection with Microsoft Detours PE import directory rewriting for newly spawned suspended processes. When the target process is resumed, the Windows loader (`ntdll!LdrpInitializeProcess`) natively maps `BetterLumaPayload.dll` prior to application entry, completely eliminating deadlocks caused by remote threads stalling on uninitialized loader locks (`LdrpInitCompleteEvent`).
- **Detour Helper Export & Ordinal Linking**: Conformed `BetterLumaPayload.dll` to Detours loader specifications by exporting ordinal #1 (`DetourFinishHelperProcess`) and invoking `DetourRestoreAfterWith()` during `DLL_PROCESS_ATTACH`, ensuring clean unhooking of temporary import directory redirects upon load.
- **Unicode & Non-ASCII Path Compatibility (8.3 Short Paths)**: Solved DLL load failures (`STATUS_DLL_NOT_FOUND` / `0xC0000135`) in environments where Steam or games are installed under non-ASCII or localized directory paths (e.g. non-English user profiles). Because PE import descriptor records (`IMAGE_IMPORT_DESCRIPTOR.Name`) strictly mandate 8-bit ASCII, paths are now automatically converted to NTFS 8.3 short paths (`GetShortPathNameW`), ensuring pure 7-bit ASCII compatibility across both primary and remote DLL deployment.
- **Architecture Guarding (WOW64 Protection)**: Added target architecture validation to gracefully reject injection attempts into 32-bit (WOW64) processes, preventing invalid 64-to-32-bit PE import injections.

### IPC Sandbox, Save Virtualization & TOCTOU Elimination
- **Atomic Kernel Handle Verification**: Eliminated Time-of-Check to Time-of-Use (TOCTOU) race conditions in save file queries (`FileExists`, `GetFileSize`, `FileRead`). Replaced path-based `GetFileAttributesA` inspection with atomic file handle creation using `FILE_FLAG_OPEN_REPARSE_POINT`, followed by in-place attribute validation (`GetFileInformationByHandle`) directly on the acquired handle.
- **Kernel-Level Sandbox Path Canonicalization**: Enforced sandbox containment via `GetFinalPathNameByHandleA` on the opened handle, verifying the resolved path strictly resides within the client's isolated save root and thwarting symlink or junction redirection attacks.
- **Subdirectory Save File Support**: Upgraded save path sanitization to support nested subdirectories (required by Unreal Engine games, Cyberpunk 2077, Baldur's Gate 3, etc.) while maintaining strict protection against directory traversal sequences (`..`), absolute paths, UNC paths, drive specifiers, invalid characters, and DOS device names.
- **Corrected 64-bit ABI for `IClientRemoteStorage::FileExists`**: Fixed an invalid 2-parameter signature and raw `0x38` memory offset mutation present in LumaCore, replacing it with Valve's authentic 64-bit ABI `(void* pThis, AppId_t appId, uint32_t fileRoot, const char* pchFile)` and eliminating struct corruption. Removed the obsolete and unsafe `IClientRemoteStorage::Dispatch` hook.

### Hooking Engine & VEH Exception Lifecycle
- **Conversion of `SpawnProcess` to Microsoft Detours**: Converted LumaCore's fragile VEH software breakpoint (`INT3`) hook on `CClientEngine::SpawnProcess` to a native Microsoft Detours inline hook. This removes the overhead of CPU exception trapping and single-step context emulation during game launches.
- **Safe VEH Drain & Shutdown Synchronization**: Resolved a fatal shutdown crash (`0x80000003` access violation) during DLL unload. Exception bytes are now restored, instruction caches flushed (`FlushInstructionCache`), and in-flight exception handlers drained (`g_vehInFlight == 0`) before unlinking the handler via `RemoveVectoredExceptionHandler`. Target pointers and capture contexts remain valid throughout the entire drain sequence.
- **Dynamic Multi-Tier Register & PID Resolution**: Eliminated brittle register assumptions (`ctx->R15`) in LumaCore's PID transfer check bypass. Implemented a dynamic resolution strategy that inspects candidate non-volatile CPU registers, verifies live process IDs via kernel handles, and cross-references active OnlineFix PID maps to reliably detect and bypass AppID mismatch branches across different compiler builds.

### Network Packet Engine & Cloud Redirection
- **Outbound Cloud Frame Suppression**: Ensured unauthorized DLC and App cloud sync requests are suppressed at the packet transmission boundary (`SuppressSend = true`), preventing unwanted cloud metadata queries from escaping to Valve CM servers.
- **Type-Safe Synthetic Cloud RPC Dispatch**: Resolved a type confusion crash where websocket transport pointers (`CTransportWebSocket*`) were erroneously passed to `CNetFilter::RecvPkt`. Synthetic cloud RPC responses now strictly target the verified `CNetFilter` receiver instance (`g_lastRecvThis`).
- **Threadpool-Backed Dispatch & Unload Draining**: Replaced ad-hoc detached OS thread creation with Windows threadpool tasks (`QueueUserWorkItem`). Added atomic dispatch counters (`g_inFlightDispatches`) and synchronous drain coordination (`DrainDispatches()`) during `NetPacket::Uninstall()`, preventing use-after-free crashes during shutdown.
- **Atomic Frame Allocation**: Converted `FrameIdx` pool tracking in `NetPacket.cpp` to `std::atomic<uint32_t>`, guaranteeing thread-safe circular buffer slot allocation under high concurrency.

### Multi-Game State & Concurrency Management
- **Thread-Local Scoped Stats Context**: Replaced LumaCore's global atomic stats AppID tracking with thread-local pipe contexts (`thread_local HSteamPipe` / `thread_local AppId_t`) paired with a thread-safe pipe-to-AppID registry (`g_pipeToAppId`), preventing concurrent IPC threads from clobbering each other's active AppID.
- **Fail-Closed Fallback Injection Security**: Hardened fallback process injection to fail closed: if the target process cannot be opened or its full image path cannot be queried via `QueryFullProcessImageNameW`, injection immediately aborts to prevent unauthorized code execution.
- **Pending Route Inactivity Pruning**: Added `PrunePendingRoutesLocked` to automatically evict stale fallback routes with a 5-minute timeout and immediately purge routes whose associated processes have exited.
- **Dead PID Eviction**: Implemented periodic process liveness polling via `GetExitCodeProcess` to clean up exited game PIDs from `g_onlineFixPidToAppId`, bounding memory growth.

### Cryptographic Integrity & Platform Resiliency
- **Cryptographic Ed25519 Verification for IPC Specs**: Integrated public-key signature verification into `IpcSpecLoader`. Remote IPC specifications fetched from mirrors are validated against `.sig` Ed25519 signatures (`PatternSig::Verify`), preventing MITM tampering and enforcing signature requirements (`require_signed`).
- **Early 8.3 Short Path Conversion in Bootstrap**: Updated `Bootstrap::Run` to resolve the Steam installation directory to an 8.3 short path prior to reading `BetterLuma.toml`, ensuring reliable initialization in non-ASCII directory environments.
- **Safe String Truncation**: Replaced legacy `strncpy` in language storage routines with safe bounds-checked `strncpy_s` using `_TRUNCATE`.

## v0.1

### OnlineFix Multi-Game Support & Process Retention
- **Steam Process Watcher Retention**: Addressed LumaCore's tracking detachment limitation where Steam reverted the green "STOP" button to blue "PLAY" when launching OnlineFix titles under AppID 480. A lightweight VEH hook on Steam's client game loop bypasses the AppID mismatch branch, keeping playtime counting and the process attached under its real AppID.
- **Concurrent Game Support & Process Trees**: Extended beyond LumaCore's single-game tracking by adding multi-game support so multiple OnlineFix titles can run concurrently. Launcher child processes (such as Unreal Engine or Unity shipping binaries) are now captured and linked back to their parent game's AppID.
- **Process Lifecycle Grace Period**: Introduced a 20-second startup grace period to prevent periodic heartbeat packets (`CMsgClientGamesPlayed`) from prematurely unregistering a game while another title is launching.
- **Isolated Pipe Routing & Scoped Stats**: Resolved connecting client PIDs at the IPC pipe level to separate saves and cloud storage for each running game. Scoped stats depth tracking ensures stats queries use the real AppID while lobby, presence, and P2P networking retain Spacewar (480) identity.
- **Automatic Language Synchronization**: Synchronized language settings from the real game to Spacewar (480) via direct ConfigStore writes, protected against Steam reverting them during periodic disk flushes.

### Process Injection & Security Hardening
- **Kernel Image Path Verification**: Hardened DLL injection by querying the true process image path directly from the Windows kernel (`QueryFullProcessImageNameW`) before injecting `BetterLumaPayload.dll`. Verifies the executable resides inside the valid Steam game installation folder to prevent unintended process injection or name collision attacks.
- **3-Tier Injection Matching**: Replaced simple basename lookups with a 3-tier queue matching strategy: exact canonical path matching, install directory ancestry checks, and unambiguous working directory fallbacks.
- **Queue Cleanup Optimization**: Throttled expired injection queue sweeps to run periodically under lock, eliminating per-process linear allocation overhead.
- **Path & String Normalization**: Replaced legacy ANSI conversions with UTF-8 fallback handling and removed `MAX_PATH` buffer limitations to support long paths safely.
- **IPC Sandbox Hardening**: Protected IPC file operations against path traversal attacks.

### Concurrency & Thread-Safety Hardening
- **PacketPool Race & Wrap-Around Elimination**: Fixed a use-after-free race condition where high network traffic could advance the circular packet ring buffer before asynchronous send routines finished consuming the data. Patched packets are now snapshotted into thread-private caller buffers with expanded mutex locks held across the entire send and receive flow.
- **Atomic Global Pointer Synchronization**: Converted critical Steam singleton pointers captured by VEH hooks to `std::atomic` with explicit acquire/release memory ordering, eliminating compiler reordering and torn reads across worker threads.
- **Thread-Safe Metadata Caching**: Protected internal game name and AppID caches with dedicated mutexes to prevent data corruption during concurrent queries.
- **VEH Handler Lifecycle Draining**: Added reference tracking and drain synchronization so exception handlers wait for in-flight calls to complete before unregistering on shutdown.

### CloudRedirect 2.0 & Fault Isolation
- **Vtable-Level Cloud Virtualization**: Fully integrated the `cloud_redirect.dll` subsystem, intercepting cloud save methods directly at the C++ vtable level instead of relying on network layer redirection.
- **In-Memory PE Import Retargeting**: Added dynamic in-memory import scanning and patching to retarget module imports from `steamclient64.dll` to `lcoverlay.dll`, allowing proper RTTI resolution in isolated execution.
- **Immediate Synthetic Cloud Responses**: Dispatched synthetic Cloud RPC responses synchronously during packet building to eliminate cloud save delays and timeouts.
- **Zero-Window Shutdown & Module Pinning**: Replaced arbitrary sleep-polling on shutdown with condition variable drain synchronization. If worker threads remain active on timeout, the module is permanently pinned with OS flags to prevent unmapped code crashes.
- **SEH Exception Diagnostics & Circuit Breaker**: Replaced blanket exception suppression with structured diagnostics that decode access violation types, target addresses, faulting modules, and register states. Unrecoverable heap corruptions and stack overflows pass through for crash dumps, while repeated non-fatal errors trigger an automatic circuit breaker.

### Memory Integrity & Buffer Safety
- **Buffer Over-Read Prevention**: Fixed a potential buffer over-read in game name queries by clamping returned byte lengths to the stack buffer size and enforcing proper null-termination.
- **Instruction Cache & Page Protection**: Ensured VEH memory modifications strictly restore original memory page permissions and call `FlushInstructionCache` across modified ranges.
- **Safe Environment Block Parsing**: Guarded process spawn environment scanning with bounds checks and structured exception handling.

### Pattern Fetching & Update Resilience
- **Configurable Remote Mirror Chain**: Added support for customizable pattern mirror URL templates in `BetterLuma.toml` with automatic fallbacks across GitHub Raw, jsDelivr CDN, and local caching.
- **Atomic Cache File Operations**: Pattern cache files are written to temporary files and atomically swapped, preventing file corruption if Steam exits mid-write.
- **String Cross-Reference Fallbacks**: Enhanced signature resolution with `.rdata` string cross-reference scanning to locate function entry points even if byte signatures change across Steam updates.

### Telemetry & Diagnostics
- **Real-Time Health Status (`status.json`)**: Added a machine-readable JSON status file showing loaded build IDs, installed vs. missed hook counts, package status, and queue metrics.
- **Zero-Cost Production Logging**: Debug builds feature asynchronous multi-sink logging per subsystem, while Release builds strip all logging macros at compile time for zero runtime overhead.
