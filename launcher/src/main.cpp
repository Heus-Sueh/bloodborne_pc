// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef RMLUI_SDL_VERSION_MAJOR
#define RMLUI_SDL_VERSION_MAJOR 3
#endif

#include "gpu_capabilities.hpp"
#include "launcher_core.hpp"
#include "launcher_model.hpp"
#include "native_launch_planner.hpp"

#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_GL3.h>
#include <SDL3/SDL.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

namespace fs = std::filesystem;
using namespace bbport;

namespace {

int RunHeadlessPlanFile(int argc, char** argv, const fs::path& exe_path) {
  const PortPaths default_paths = DiscoverPortPaths(exe_path);
  fs::path port_dir = default_paths.port_dir;
  fs::path data_dir = default_paths.data_dir;
  fs::path caps_path;
  fs::path plan_file;
  bool skip_prepare = false;
  bool skip_online = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--port-dir" && i + 1 < argc) {
      port_dir = fs::path(argv[++i]);
    } else if (arg == "--data-dir" && i + 1 < argc) {
      data_dir = fs::path(argv[++i]);
    } else if (arg == "--caps" && i + 1 < argc) {
      caps_path = fs::path(argv[++i]);
    } else if (arg == "--plan-file" && i + 1 < argc) {
      plan_file = fs::path(argv[++i]);
    } else if (arg == "--skip-prepare") {
      skip_prepare = true;
    } else if (arg == "--skip-online") {
      skip_online = true;
    }
  }

  LinuxSystemProbe probe(default_paths);
  const auto env = probe.HostEnvironment();
  if (env.count("BB_SKIP_PREPARE") && env.at("BB_SKIP_PREPARE") == "1") {
    skip_prepare = true;
  }

  NativeExecutionPlan plan;
  std::string err;
  if (!NativeLaunchPlanner::PlanAndPrepare(
          env, port_dir, data_dir, caps_path, skip_prepare, skip_online, plan, &err,
          [](const std::string& line) { std::cout << line << "\n"; })) {
    std::cerr << err << "\n";
    return 1;
  }
  if (!NativeLaunchPlanner::WriteNulPlanFile(plan, plan_file, &err)) {
    std::cerr << err << "\n";
    return 1;
  }
  return 0;
}

int RunHeadlessPlay(const fs::path& exe_path) {
  const PortPaths paths = DiscoverPortPaths(exe_path);
  SettingsSchema schema;
  std::string err;
  if (!SettingsSchema::LoadFromFile(paths.SchemaPath(), schema, &err)) {
    std::cerr << "bb-launcher --play: " << err << "\n";
    return 1;
  }

  SettingsStore store(std::move(schema), paths);
  store.Load();
  LinuxSystemProbe probe(paths);
  const LaunchPlan base_plan = LaunchEnvCompiler::Compile(paths, store, probe);

#ifdef _WIN32
  const char* probe_name = "bb-probe.exe";
#else
  const char* probe_name = "bb-probe";
#endif
  std::error_code ec;
  fs::path direct_probe = paths.port_dir / (paths.packaged ? "bin" : "out") / probe_name;
  if (!fs::is_regular_file(direct_probe, ec)) direct_probe = paths.port_dir / "bin" / probe_name;
  if (!fs::is_regular_file(direct_probe, ec)) direct_probe = paths.port_dir / "out" / probe_name;

  std::map<std::string, std::string> current_env = base_plan.env;
  current_env["BB_PROBE"] = direct_probe.string();
  current_env["BB_RESTART_BY_EXIT"] = "1";
  current_env["BB_GPU_USER_DIR"] = store.ResolvedSavesDir().string();

  PosixProcessRunner runner;
  while (true) {
    NativeExecutionPlan native_plan;
    const bool skip_prep = (current_env.count("BB_SKIP_PREPARE") && current_env["BB_SKIP_PREPARE"] == "1");
    if (!NativeLaunchPlanner::PlanAndPrepare(
            current_env, paths.port_dir, paths.data_dir, {}, skip_prep, false, native_plan, &err,
            [](const std::string& line) { std::cout << line << "\n"; })) {
      std::cerr << "bb-launcher --play: " << err << "\n";
      return 1;
    }

    current_env = native_plan.full_env;

    SpawnSpec spec;
    spec.executable = native_plan.probe_executable;
    spec.args = native_plan.probe_args;
    spec.cwd = paths.data_dir;
    spec.env = current_env;
    spec.inherit_env = false;

    auto spawned = runner.Spawn(spec);
    if (!spawned) {
      if (native_plan.mod_game) fs::remove_all(*native_plan.mod_game, ec);
      std::cerr << "bb-launcher --play: " << spawned.error << "\n";
      return 1;
    }

    std::vector<std::string> lines;
    int exit_code = 0;
    while (true) {
      lines.clear();
      spawned.session->DrainOutput(lines);
      for (const auto& l : lines) {
        std::cout << l << "\n";
      }
      if (auto code = spawned.session->TryWait(); code.has_value()) {
        lines.clear();
        spawned.session->DrainOutput(lines);
        for (const auto& l : lines) {
          std::cout << l << "\n";
        }
        exit_code = *code;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (native_plan.mod_game) {
      fs::remove_all(*native_plan.mod_game, ec);
    }
    if (exit_code == kRestartExitCode) {
      std::cout << "— restarting through native launch planner —\n";
      continue;
    }
    return exit_code;
  }
}

} // namespace

int main(int argc, char** argv) {
  const fs::path exe_path = (argc > 0 && argv[0]) ? fs::path(argv[0]) : fs::path{};

  // 1. Headless shortcut checks (--plan-file, --play, or integrated GPU/display/input capability queries)
  if (argc > 1) {
    const std::string_view first(argv[1]);
    if (first == "--gpus" || first == "--device" || first == "--live-resolution" ||
        first == "--displays" || first == "--gamepads" || first == "--read-input" ||
        first == "--check-blit") {
      return RunGpuCapabilitiesCli(argc, argv);
    }
  }
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--plan-file") {
      return RunHeadlessPlanFile(argc, argv, exe_path);
    }
    if (std::string_view(argv[i]) == "--play") {
      return RunHeadlessPlay(exe_path);
    }
  }

  // 2. Initialize LauncherCore before creating UI
  std::unique_ptr<LauncherCore> core;
  if (auto opened = LauncherCore::OpenDefault(core, exe_path); !opened) {
    std::cerr << "LauncherCore::OpenDefault failed: " << opened.error << "\n";
    return 1;
  }

  // 3. Initialize SDL3 Video subsystem
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return 1;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  const int initial_width = 1200;
  const int initial_height = 760;

  SDL_Window* window = SDL_CreateWindow("Bloodborne PC Launcher", initial_width, initial_height,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
    SDL_Quit();
    return 1;
  }

  SDL_SetWindowMinimumSize(window, 720, 520);

  // Set window icon from packaging/bbport.png or launcher/assets/bbport.png
  for (const fs::path& icon_candidate : {
           core->Paths().port_dir / "packaging" / "bbport.png",
           core->Paths().port_dir / "launcher" / "assets" / "bbport.png",
           fs::path("packaging/bbport.png"),
           fs::path("launcher/assets/bbport.png"),
           fs::path("assets/bbport.png"),
       }) {
    std::error_code ec;
    if (fs::exists(icon_candidate, ec)) {
      if (SDL_Surface* icon_surf = SDL_LoadPNG(icon_candidate.string().c_str())) {
        SDL_SetWindowIcon(window, icon_surf);
        SDL_DestroySurface(icon_surf);
        break;
      }
    }
  }

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (!gl_context) {
    std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << "\n";
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1);

  Rml::String gl_message;
  if (!RmlGL3::Initialize(&gl_message)) {
    std::cerr << "RmlGL3::Initialize failed: " << gl_message << "\n";
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SystemInterface_SDL system_interface(window);
  RenderInterface_GL3 render_interface;

  int fb_w = initial_width;
  int fb_h = initial_height;
  SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
  if (fb_w < 1) fb_w = initial_width;
  if (fb_h < 1) fb_h = initial_height;
  render_interface.SetViewport(fb_w, fb_h);

  Rml::SetSystemInterface(&system_interface);
  Rml::SetRenderInterface(&render_interface);

  if (!Rml::Initialise()) {
    std::cerr << "Rml::Initialise failed!\n";
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // 4. Resolve assets relative to Port Directory
  const fs::path port_dir = core->Paths().port_dir;
  auto resolve_asset_path = [&](const std::string& rel_path) -> std::string {
    const fs::path in_port = port_dir / rel_path;
    if (fs::exists(in_port)) return in_port.string();
    if (fs::exists(rel_path)) return rel_path;
    if (rel_path.rfind("launcher/", 0) == 0) {
      const std::string stripped = rel_path.substr(9);
      if (fs::exists(stripped)) return stripped;
    }
    return in_port.string();
  };

  auto load_font = [&](const std::string& rel, const std::string& family,
                       Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback = false) {
    const std::string p = resolve_asset_path(rel);
    if (fs::exists(p)) {
      Rml::LoadFontFace(p, family, style, weight, fallback);
    }
  };

  load_font("launcher/assets/fonts/Cinzel-Regular.ttf", "Cinzel", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal);
  load_font("launcher/assets/fonts/Cinzel-Bold.ttf", "Cinzel", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold);
  load_font("launcher/assets/fonts/Cormorant-Regular.ttf", "Cormorant", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal);
  load_font("launcher/assets/fonts/Cormorant-Regular.ttf", "Cormorant", Rml::Style::FontStyle::Italic, Rml::Style::FontWeight::Normal);
  load_font("launcher/assets/fonts/Cormorant-Bold.ttf", "Cormorant", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold);

  const std::string cjk_reg = resolve_asset_path("launcher/assets/fonts/NotoSansCJK-Regular.ttc");
  bool cjk_loaded = false;
  if (fs::exists(cjk_reg)) {
    cjk_loaded = Rml::LoadFontFace(cjk_reg, "Noto Sans CJK SC", Rml::Style::FontStyle::Normal,
                                   Rml::Style::FontWeight::Normal, true, 2);
  }
  if (!cjk_loaded) {
    const std::vector<std::string> system_cjk_paths = {
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
    };
    for (const auto& sys_path : system_cjk_paths) {
      if (fs::exists(sys_path)) {
        if (Rml::LoadFontFace(sys_path, "Noto Sans CJK SC", Rml::Style::FontStyle::Normal,
                              Rml::Style::FontWeight::Normal, true, 2) ||
            Rml::LoadFontFace(sys_path, "Noto Sans CJK", Rml::Style::FontStyle::Normal,
                              Rml::Style::FontWeight::Normal, true, 0)) {
          break;
        }
      }
    }
  }

  load_font("launcher/assets/fonts/NotoSans-Regular.ttf", "Noto Sans", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, true);
  load_font("launcher/assets/fonts/NotoSans-Bold.ttf", "Noto Sans", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold);
  load_font("launcher/assets/fonts/JetBrainsMono-Regular.ttf", "JetBrainsMono NF", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, true);
  load_font("launcher/assets/fonts/JetBrainsMono-Bold.ttf", "JetBrainsMono NF", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold);
  load_font("launcher/assets/fonts/lucide.ttf", "lucide", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal);
  load_font("launcher/assets/fonts/Oswald-Regular.ttf", "Oswald", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal);
  load_font("launcher/assets/fonts/Oswald-Bold.ttf", "Oswald", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold);
  load_font("launcher/assets/fonts/SourceSans3-Regular.ttf", "Source Sans 3", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal);
  load_font("launcher/assets/fonts/SourceSans3-Bold.ttf", "Source Sans 3", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold);

  // 5. Create RmlUi Context & thin LauncherModel adapter
  Rml::Context* rml_context = Rml::CreateContext("main", Rml::Vector2i(fb_w, fb_h));
  if (!rml_context) {
    std::cerr << "Rml::CreateContext failed!\n";
    Rml::Shutdown();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  LauncherModel launcher_model(rml_context, window, *core);

  const std::string rml_path = resolve_asset_path("launcher/assets/launcher.rml");
  Rml::ElementDocument* document = rml_context->LoadDocument(rml_path);
  if (!document) {
    std::cerr << "Failed to load RML document: " << rml_path << "\n";
    Rml::Shutdown();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  document->Show();

  auto apply_responsive_layout = [&]() {
    int pixel_w = initial_width;
    int pixel_h = initial_height;
    SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
    if (pixel_w < 1) pixel_w = initial_width;
    if (pixel_h < 1) pixel_h = initial_height;

    int logical_w = pixel_w;
    int logical_h = pixel_h;
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    if (logical_w < 1) logical_w = pixel_w;

    render_interface.SetViewport(pixel_w, pixel_h);
    rml_context->SetDimensions(Rml::Vector2i(pixel_w, pixel_h));

    const float hidpi = float(pixel_w) / float(logical_w);
    rml_context->SetDensityIndependentPixelRatio(std::clamp(hidpi, 0.85f, 3.0f));

    document->SetClass("compact", logical_w < 1140);
    document->SetClass("narrow", logical_w < 740);
    rml_context->Update();
  };

  apply_responsive_layout();

  bool running = true;
  SDL_Event event;
  while (running && !launcher_model.ShouldClose()) {
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_EVENT_QUIT:
          running = false;
          break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
          apply_responsive_layout();
          break;
        default:
          RmlSDL::InputEventHandler(rml_context, window, event);
          break;
      }
    }

    launcher_model.Update();
    rml_context->Update();

    render_interface.BeginFrame();
    render_interface.Clear();
    rml_context->Render();
    render_interface.EndFrame();

    SDL_GL_SwapWindow(window);
    SDL_Delay(8);
  }

  document->Close();
  Rml::Shutdown();
  RmlGL3::Shutdown();
  SDL_GL_DestroyContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
