// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <chrono>

namespace ClockMark {

    class Span {
    public:
        Span() : start_(std::chrono::steady_clock::now()) {}

        double Ms() const {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start_).count();
        }

    private:
        std::chrono::steady_clock::time_point start_;
    };

}
