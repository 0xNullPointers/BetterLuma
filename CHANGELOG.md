# Changelog

## v0.1

### OnlineFix Multi-Game Support & Process Retention
- **Steam Process Watcher Retention**: Prevented Steam from detaching tracking and reverting the "STOP" button to "PLAY" when launching OnlineFix titles under AppID 480. A lightweight VEH hook on Steam's client game loop bypasses the AppID mismatch branch, keeping playtime counting and the process attached under its real AppID.
- **Concurrent Game Support & Process Trees**: Added multi-game tracking so multiple OnlineFix titles can run concurrently. Launcher child processes (such as Unreal Engine or Unity shipping binaries) are now captured and linked back to their parent game's AppID.
- **Process Lifecycle Grace Period**: Introduced a 20-second startup grace period to prevent periodic heartbeat packets (`CMsgClientGamesPlayed`) from prematurely unregistering a game while another title is launching.
- **Isolated Pipe Routing & Scoped Stats**: Resolved connecting client PIDs at the IPC pipe level to separate saves and cloud storage for each running game. Scoped stats depth tracking ensures stats queries use the real AppID while lobby, presence, and P2P networking retain Spacewar (480) identity.
- **Automatic Language Synchronization**: Synchronized language settings from the real game to Spacewar (480) via direct ConfigStore writes, protected against Steam reverting them during periodic disk flushes.

### Process Injection & Security Hardening
- **Kernel Image Path Verification**: Hardened DLL injection by querying the true process image path directly from the Windows kernel (`QueryFullProcessImageNameW`) before injecting `LumaCorePayload.dll`. Verifies the executable resides inside the valid Steam game installation folder to prevent unintended process injection or name collision attacks.
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
- **Configurable Remote Mirror Chain**: Added support for customizable pattern mirror URL templates in `lumacore.toml` with automatic fallbacks across GitHub Raw, jsDelivr CDN, and local caching.
- **Atomic Cache File Operations**: Pattern cache files are written to temporary files and atomically swapped, preventing file corruption if Steam exits mid-write.
- **String Cross-Reference Fallbacks**: Enhanced signature resolution with `.rdata` string cross-reference scanning to locate function entry points even if byte signatures change across Steam updates.

### Telemetry & Diagnostics
- **Real-Time Health Status (`status.json`)**: Added a machine-readable JSON status file showing loaded build IDs, installed vs. missed hook counts, package status, and queue metrics.
- **Zero-Cost Production Logging**: Debug builds feature asynchronous multi-sink logging per subsystem, while Release builds strip all logging macros at compile time for zero runtime overhead.
