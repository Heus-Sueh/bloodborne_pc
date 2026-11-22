// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "port_paths.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bbport {

struct GpuInfo {
  std::uint32_t vulkan_index = 0;
  std::string name;
  bool preferred = false;
};

// Seam for host environment and hardware/runtime queries.
// Two adapters: LinuxSystemProbe (production) and FakeSystemProbe (tests).
class SystemProbe {
public:
  virtual ~SystemProbe() = default;
  virtual std::vector<GpuInfo> ListGpus() const = 0;
  virtual bool HasOnlineModule(const PortPaths& paths, std::string_view pool) const = 0;
  virtual std::map<std::string, std::string> HostEnvironment() const = 0;
  virtual std::string DetectSystemLanguage() const = 0;
};

class LinuxSystemProbe final : public SystemProbe {
public:
  explicit LinuxSystemProbe(PortPaths paths = DiscoverPortPaths());

  std::vector<GpuInfo> ListGpus() const override;
  bool HasOnlineModule(const PortPaths& paths, std::string_view pool) const override;
  std::map<std::string, std::string> HostEnvironment() const override;
  std::string DetectSystemLanguage() const override;

private:
  PortPaths paths_;
};

} // namespace bbport
