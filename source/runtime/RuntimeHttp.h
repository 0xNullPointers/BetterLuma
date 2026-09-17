// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.
//
// Tiny WinHTTP-based GET helper used by the lua sandbox so plugin .lua
// files can pull a runtime manifest off a clearnet host without betterluma
// having to ship a manifest cache itself. Hard caps in place: 8 MiB body
// ceiling, single-host, GET only, no redirect chains followed beyond what
// WinHTTP does on its own. Plugin scripts call this through the lua
// binding `lc_http_get(url) -> body, status_code` registered in LuaState.

#ifndef BETTERLUMA_RUNTIME_HTTP_H
#define BETTERLUMA_RUNTIME_HTTP_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RuntimeHttp {

    struct Response {
        bool         networkError = true;
        int          status       = 0;
        std::string  body;
        std::string  diagnostic;
    };

    // Single-shot HTTP/HTTPS GET. Body is capped at 8 MiB. The total
    // resolve+connect+send+recv budget is 12 seconds; longer transfers
    // get cut off and return networkError=true.
    Response Get(std::string_view url,
                 std::wstring_view userAgent = L"BetterLuma-RuntimeHttp/1.0");

    // Single-shot HTTP/HTTPS POST. Same caps as GET.
    // extraHeaders are raw "Header: value" strings appended to the request.
    Response Post(std::string_view url, std::string_view body,
                  const std::vector<std::string>& extraHeaders = {});
}

#endif
