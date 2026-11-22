// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

namespace bbport {

// Seam for monotonic time (used for quick-crash detection without sleeps in tests).
class Clock {
public:
  virtual ~Clock() = default;
  virtual std::chrono::steady_clock::time_point Now() const = 0;
};

class SteadyClock final : public Clock {
public:
  std::chrono::steady_clock::time_point Now() const override {
    return std::chrono::steady_clock::now();
  }
};

} // namespace bbport
