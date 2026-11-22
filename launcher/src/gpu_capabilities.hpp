// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "system_probe.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bbport {

struct SelectedGpuDevice {
  std::uint32_t index = 0;
  std::uint32_t vendor_id = 0;
  std::string name;
  bool discrete = false;
  bool live_resolution_suits = false;
  bool scene_scaling_blit_supported = false;
};

// Direct in-process Vulkan GPU enumeration.
std::vector<GpuInfo> ProbeVulkanGpusInProcess();

// Direct in-process selected Vulkan physical device query.
std::optional<SelectedGpuDevice> ProbeSelectedVulkanDeviceInProcess(std::string_view gpu_id_env = "");

// Direct in-process SDL3 monitor resolution query ("WxH").
std::string ProbeMonitorResolutionInProcess(std::string_view wanted_display = "");

// CLI handler for --gpus, --device, --live-resolution, --displays, --gamepads, --read-input, --check-blit.
int RunGpuCapabilitiesCli(int argc, char** argv);

} // namespace bbport
