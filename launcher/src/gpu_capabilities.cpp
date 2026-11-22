// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu_capabilities.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bbport {

namespace {

VkDeviceSize LargestLocalHeap(VkPhysicalDevice device) {
  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(device, &memory);
  VkDeviceSize largest = 0;
  for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
    if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
        memory.memoryHeaps[i].size > largest) {
      largest = memory.memoryHeaps[i].size;
    }
  }
  return largest;
}

bool BetterDevice(VkPhysicalDevice candidate, VkPhysicalDevice current) {
  VkPhysicalDeviceProperties next{}, old{};
  vkGetPhysicalDeviceProperties(candidate, &next);
  vkGetPhysicalDeviceProperties(current, &old);
  const bool next_api = next.apiVersion >= VK_API_VERSION_1_3;
  const bool old_api = old.apiVersion >= VK_API_VERSION_1_3;
  if (next_api != old_api) return next_api;
  const bool next_discrete = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  const bool old_discrete = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  if (next_discrete != old_discrete) return next_discrete;
  const bool next_cpu = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
  const bool old_cpu = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
  if (next_cpu != old_cpu) return !next_cpu;
  return LargestLocalHeap(candidate) > LargestLocalHeap(current);
}

bool HasExtension(VkPhysicalDevice device, const char* name) {
  std::uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
    return false;
  }
  std::vector<VkExtensionProperties> list(count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, list.data()) != VK_SUCCESS) {
    return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (std::strcmp(list[i].extensionName, name) == 0) {
      return true;
    }
  }
  return false;
}

bool LiveResolutionSuits(VkPhysicalDevice device, bool log_stderr) {
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(device, &props);
  const bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  const VkDeviceSize memory = LargestLocalHeap(device);
  const bool old_nvidia = props.vendorID == 0x10de &&
                          !HasExtension(device, "VK_KHR_fragment_shader_barycentric");
  const bool suits = discrete && memory >= (static_cast<VkDeviceSize>(7680) << 20) && !old_nvidia;
  if (log_stderr) {
    std::fprintf(stderr, "GPU: %s, %s, %llu MiB: live resolution changes %s\n",
                 props.deviceName,
                 discrete ? "discrete" : "integrated or other",
                 static_cast<unsigned long long>(memory >> 20),
                 suits ? "on" : "off (startup resolution patch)");
  }
  return suits;
}

bool CheckBlitSupport(VkPhysicalDevice device, bool log_stderr) {
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(device, &props);
  struct FormatSpec {
    VkFormat format;
    const char* name;
  };
  constexpr FormatSpec kFormats[] = {
      {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"},
      {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"},
      {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11"},
      {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
      {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"},
  };
  bool supported = true;
  for (const auto& f : kFormats) {
    VkFormatProperties features{};
    vkGetPhysicalDeviceFormatProperties(device, f.format, &features);
    constexpr VkFormatFeatureFlags kRequired = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((features.optimalTilingFeatures & kRequired) != kRequired) {
      if (log_stderr) {
        std::fprintf(stderr, "GPU scene scaling: %s lacks blit support for %s\n", props.deviceName, f.name);
      }
      supported = false;
    }
  }
  if (supported && log_stderr) {
    std::fprintf(stderr, "GPU scene scaling: %s supports live presets\n", props.deviceName);
  }
  return supported;
}

int ListGamepadsCli() {
  const bool was_init = (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0;
  if (!was_init && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "gamepads: %s\n", SDL_GetError());
    return 1;
  }
  for (int i = 0; i < 5; ++i) {
    SDL_PumpEvents();
    SDL_Delay(40);
  }
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) {
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof(guid));
    const char* name = SDL_GetGamepadNameForID(ids[i]);
    std::printf("%s\t%s\n", guid, name ? name : "?");
  }
  SDL_free(ids);
  if (!was_init) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
  return 0;
}

int ListDisplaysCli() {
  const bool was_init = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0;
  if (!was_init && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "displays: %s\n", SDL_GetError());
    return 1;
  }
  const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  for (int i = 0; ids && i < count; ++i) {
    const char* name = SDL_GetDisplayName(ids[i]);
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i]);
    std::printf("%s\t%dx%d\t%d\n",
                (name && *name) ? name : "?",
                mode ? mode->w : 0,
                mode ? mode->h : 0,
                ids[i] == primary ? 1 : 0);
  }
  SDL_free(ids);
  if (!was_init) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
  return 0;
}

int ReadInputCli(const char* kind) {
  const bool want_key = std::strcmp(kind, "pad") != 0;
  const bool want_pad = std::strcmp(kind, "key") != 0;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "read-input: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  const char* prompt = (want_key && want_pad) ? "Press a key, a mouse button or a gamepad button"
                       : want_key             ? "Press a key or a mouse button"
                                              : "Press a gamepad button";
  if (!SDL_CreateWindowAndRenderer("bbport", 520, 90, 0, &window, &renderer)) {
    std::fprintf(stderr, "read-input: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) SDL_OpenGamepad(ids[i]);
  SDL_free(ids);

  const Uint64 end = SDL_GetTicks() + 15000;
  bool done = false;
  while (!done && SDL_GetTicks() < end) {
    SDL_SetRenderDrawColor(renderer, 24, 24, 28, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
    SDL_RenderDebugText(renderer, 16, 30, prompt);
    SDL_RenderDebugText(renderer, 16, 50, "Escape: cancel");
    SDL_RenderPresent(renderer);
    SDL_Event e;
    while (!done && SDL_WaitEventTimeout(&e, 50)) {
      switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
          done = true;
          break;
        case SDL_EVENT_GAMEPAD_ADDED:
          SDL_OpenGamepad(e.gdevice.which);
          break;
        case SDL_EVENT_KEY_DOWN:
          if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
            done = true;
          } else if (want_key) {
            std::printf("key %s\n", SDL_GetScancodeName(e.key.scancode));
            done = true;
          }
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          if (want_key && e.button.button >= SDL_BUTTON_LEFT && e.button.button <= SDL_BUTTON_X2) {
            static const char* const names[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};
            std::printf("key %s\n", names[e.button.button]);
            done = true;
          }
          break;
        case SDL_EVENT_MOUSE_WHEEL:
          if (want_key && e.wheel.y != 0) {
            const float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            std::printf("key %s\n", y > 0 ? "Wheel Up" : "Wheel Down");
            done = true;
          }
          break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
          if (want_pad) {
            std::printf("pad %s\n", SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(e.gbutton.button)));
            done = true;
          }
          break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
          if (want_pad && e.gaxis.value > 16000 &&
              (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            std::printf("pad %s\n", e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "lefttrigger" : "righttrigger");
            done = true;
          }
          break;
        default:
          break;
      }
    }
  }
  std::fflush(stdout);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

} // namespace

std::vector<GpuInfo> ProbeVulkanGpusInProcess() {
  std::vector<GpuInfo> gpus;
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "bbport scene scaling probe";
  app.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) {
    return gpus;
  }

  std::uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
    vkDestroyInstance(instance, nullptr);
    return gpus;
  }

  std::vector<VkPhysicalDevice> devices(count);
  if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return gpus;
  }

  VkPhysicalDevice best = devices[0];
  for (std::uint32_t i = 1; i < count; ++i) {
    if (BetterDevice(devices[i], best)) {
      best = devices[i];
    }
  }

  gpus.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(devices[i], &props);
    GpuInfo info;
    info.vulkan_index = i;
    info.name = props.deviceName;
    info.preferred = (devices[i] == best);
    gpus.push_back(std::move(info));
  }

  vkDestroyInstance(instance, nullptr);
  return gpus;
}

std::optional<SelectedGpuDevice> ProbeSelectedVulkanDeviceInProcess(std::string_view gpu_id_env) {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "bbport scene scaling probe";
  app.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) {
    return std::nullopt;
  }

  std::uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
    vkDestroyInstance(instance, nullptr);
    return std::nullopt;
  }

  std::vector<VkPhysicalDevice> devices(count);
  if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return std::nullopt;
  }

  std::string gpu_id_str(gpu_id_env);
  if (gpu_id_str.empty()) {
    if (const char* env_id = std::getenv("BB_GPU_ID"); env_id && env_id[0] != '\0') {
      gpu_id_str = env_id;
    }
  }

  std::uint32_t selected_idx = 0;
  VkPhysicalDevice selected = devices[0];
  if (!gpu_id_str.empty() && std::atoi(gpu_id_str.c_str()) >= 0) {
    const unsigned long idx = std::strtoul(gpu_id_str.c_str(), nullptr, 10);
    if (idx >= count) {
      vkDestroyInstance(instance, nullptr);
      return std::nullopt;
    }
    selected_idx = static_cast<std::uint32_t>(idx);
    selected = devices[selected_idx];
  } else {
    for (std::uint32_t i = 1; i < count; ++i) {
      if (BetterDevice(devices[i], selected)) {
        selected = devices[i];
        selected_idx = i;
      }
    }
  }

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(selected, &props);
  SelectedGpuDevice out;
  out.index = selected_idx;
  out.vendor_id = props.vendorID;
  out.name = props.deviceName;
  out.discrete = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
  out.live_resolution_suits = LiveResolutionSuits(selected, false);
  out.scene_scaling_blit_supported = CheckBlitSupport(selected, false);

  vkDestroyInstance(instance, nullptr);
  return out;
}

std::string ProbeMonitorResolutionInProcess(std::string_view wanted_display) {
  const bool was_init = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0;
  if (!was_init && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    return {};
  }

  const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  std::string chosen;
  for (int i = 0; ids && i < count; ++i) {
    const char* raw_name = SDL_GetDisplayName(ids[i]);
    const std::string name = (raw_name && *raw_name) ? raw_name : "?";
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i]);
    if (!mode || mode->w <= 0 || mode->h <= 0) continue;
    const std::string size = std::to_string(mode->w) + "x" + std::to_string(mode->h);
    const bool is_primary = (ids[i] == primary);

    if (!wanted_display.empty()) {
      if (wanted_display == name || wanted_display == std::to_string(i + 1)) {
        chosen = size;
      }
    } else if (is_primary) {
      chosen = size;
    }
  }
  SDL_free(ids);
  if (!was_init) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
  return chosen;
}

int RunGpuCapabilitiesCli(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--read-input") == 0) {
    return ReadInputCli(argc > 2 ? argv[2] : "any");
  }
  if (argc > 1 && std::strcmp(argv[1], "--gamepads") == 0) {
    return ListGamepadsCli();
  }
  if (argc > 1 && std::strcmp(argv[1], "--displays") == 0) {
    return ListDisplaysCli();
  }

  const bool live_mode = argc > 1 && std::strcmp(argv[1], "--live-resolution") == 0;
  const bool device_mode = argc > 1 && std::strcmp(argv[1], "--device") == 0;
  const bool gpus_mode = argc > 1 && std::strcmp(argv[1], "--gpus") == 0;

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "bbport scene scaling probe";
  app.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) {
    std::fputs("GPU scene scaling: cannot create Vulkan instance\n", stderr);
    return 1;
  }

  std::uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
    std::fputs("GPU scene scaling: no Vulkan device\n", stderr);
    vkDestroyInstance(instance, nullptr);
    return 1;
  }

  std::vector<VkPhysicalDevice> devices(count);
  if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
    std::fputs("GPU scene scaling: cannot enumerate Vulkan devices\n", stderr);
    vkDestroyInstance(instance, nullptr);
    return 1;
  }

  if (gpus_mode) {
    VkPhysicalDevice best = devices[0];
    for (std::uint32_t i = 1; i < count; ++i) {
      if (BetterDevice(devices[i], best)) best = devices[i];
    }
    static const char* const types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    for (std::uint32_t i = 0; i < count; ++i) {
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(devices[i], &props);
      const unsigned type = props.deviceType <= VK_PHYSICAL_DEVICE_TYPE_CPU ? props.deviceType : 0;
      std::printf("%u\t%#06x\t%s\t%s\t%d\n", i, props.vendorID, types[type], props.deviceName,
                  devices[i] == best ? 1 : 0);
    }
    vkDestroyInstance(instance, nullptr);
    return 0;
  }

  VkPhysicalDevice selected = devices[0];
  const char* gpu_id = std::getenv("BB_GPU_ID");
  if (gpu_id && std::atoi(gpu_id) >= 0) {
    const unsigned long index = std::strtoul(gpu_id, nullptr, 10);
    if (index >= count) {
      std::fputs("GPU scene scaling: BB_GPU_ID is outside the device list\n", stderr);
      vkDestroyInstance(instance, nullptr);
      return 1;
    }
    selected = devices[index];
  } else {
    for (std::uint32_t i = 1; i < count; ++i) {
      if (BetterDevice(devices[i], selected)) selected = devices[i];
    }
  }

  if (device_mode) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(selected, &props);
    std::printf("%#06x\t%s\n", props.vendorID, props.deviceName);
    vkDestroyInstance(instance, nullptr);
    return 0;
  }

  if (live_mode) {
    std::printf("%d\n", LiveResolutionSuits(selected, true) ? 1 : 0);
    vkDestroyInstance(instance, nullptr);
    return 0;
  }

  const bool supported = CheckBlitSupport(selected, true);
  vkDestroyInstance(instance, nullptr);
  return supported ? 0 : 1;
}

} // namespace bbport
