#ifndef RMLUI_SDL_VERSION_MAJOR
#define RMLUI_SDL_VERSION_MAJOR 3
#endif

#include "config_manager.hpp"
#include "launcher_model.h"
#include <RmlUi/Core.h>
#include <RmlUi_Include_GL3.h>
#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_GL3.h>
#include <SDL3/SDL.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  // 1. Headless shortcut check (--play)
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--play") {
      LauncherSettings settings = ConfigManager::Load();
      return ConfigManager::LaunchGame(settings);
    }
  }

  // 2. Initialize SDL3 Video subsystem
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return 1;
  }

  // Configure OpenGL attributes for GL 3.3 Core
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  const int initial_width = 960;
  const int initial_height = 620;

  SDL_Window* window = SDL_CreateWindow("Bloodborne PC Launcher", initial_width, initial_height,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

  if (!window) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
    SDL_Quit();
    return 1;
  }

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (!gl_context) {
    std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << "\n";
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1); // VSync for launcher UI

  Rml::String gl_message;
  if (!RmlGL3::Initialize(&gl_message)) {
    std::cerr << "RmlGL3::Initialize failed: " << gl_message << "\n";
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // 3. Initialize RmlUi Interfaces
  SystemInterface_SDL system_interface(window);
  RenderInterface_GL3 render_interface;

  int fb_w = initial_width;
  int fb_h = initial_height;
  SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
  if (fb_w < 1)
    fb_w = initial_width;
  if (fb_h < 1)
    fb_h = initial_height;
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

  // 4. Load TrueType Fonts explicitly (Rule R-07)
  auto resolve_font_path = [](const std::string& rel_path) -> std::string {
    if (fs::exists(rel_path)) {
      return rel_path;
    }
    if (rel_path.rfind("launcher/", 0) == 0) {
      std::string stripped = rel_path.substr(9);
      if (fs::exists(stripped)) {
        return stripped;
      }
    }
    return rel_path;
  };

  const std::string noto_reg = resolve_font_path("launcher/assets/fonts/NotoSans-Regular.ttf");
  const std::string noto_bold = resolve_font_path("launcher/assets/fonts/NotoSans-Bold.ttf");
  const std::string jb_reg = resolve_font_path("launcher/assets/fonts/JetBrainsMono-Regular.ttf");
  const std::string jb_bold = resolve_font_path("launcher/assets/fonts/JetBrainsMono-Bold.ttf");

  // Noto Sans as primary UI font and fallback
  if (fs::exists(noto_reg)) {
    Rml::LoadFontFace(noto_reg, "Noto Sans", Rml::Style::FontStyle::Normal,
                      Rml::Style::FontWeight::Normal, true);
  }
  if (fs::exists(noto_bold)) {
    Rml::LoadFontFace(noto_bold, "Noto Sans", Rml::Style::FontStyle::Normal,
                      Rml::Style::FontWeight::Bold, false);
  }

  // JetBrains Mono under all aliases ("JetBrainsMono NF" and "JetBrainsMono
  // Nerd Font")
  if (fs::exists(jb_reg)) {
    Rml::LoadFontFace(jb_reg, "JetBrainsMono NF", Rml::Style::FontStyle::Normal,
                      Rml::Style::FontWeight::Normal, false);
    Rml::LoadFontFace(jb_reg, "JetBrainsMono Nerd Font", Rml::Style::FontStyle::Normal,
                      Rml::Style::FontWeight::Normal, false);
  }
  if (fs::exists(jb_bold)) {
    Rml::LoadFontFace(jb_bold, "JetBrainsMono NF", Rml::Style::FontStyle::Normal,
                      Rml::Style::FontWeight::Bold, false);
    Rml::LoadFontFace(jb_bold, "JetBrainsMono Nerd Font", Rml::Style::FontStyle::Normal,
                      Rml::Style::FontWeight::Bold, false);
  }

  // 5. Create RmlUi Context
  Rml::Context* rml_context = Rml::CreateContext("main", Rml::Vector2i(fb_w, fb_h));
  if (!rml_context) {
    std::cerr << "Failed to create RmlUi context!\n";
    Rml::Shutdown();
    RmlGL3::Shutdown();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // 6. Instantiate Reactive View Model & Load Document
  LauncherModel model(rml_context, window);

  std::string rml_path = "launcher/assets/launcher.rml";
  if (!fs::exists(rml_path)) {
    rml_path = "assets/launcher.rml";
  }
  Rml::ElementDocument* document = rml_context->LoadDocument(rml_path);
  if (!document) {
    std::cerr << "Failed to load RML document: " << rml_path << "\n";
    Rml::Shutdown();
    RmlGL3::Shutdown();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  document->Show();

  // 7. Main Event Loop
  bool running = true;
  SDL_Event event;

  while (running && !model.ShouldClose()) {
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_EVENT_QUIT:
        running = false;
        break;
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w >= 1 && h >= 1) {
          rml_context->SetDimensions(Rml::Vector2i(w, h));
          render_interface.SetViewport(w, h);
          glViewport(0, 0, w, h);
        }
        break;
      }
      default:
        RmlSDL::InputEventHandler(rml_context, window, event);
        break;
      }
    }

    // Update Model and RmlUi Context
    model.Update();
    rml_context->Update();

    // Clear and Render
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT) |
            static_cast<GLbitfield>(GL_STENCIL_BUFFER_BIT));

    render_interface.BeginFrame();
    rml_context->Render();
    render_interface.EndFrame();

    SDL_GL_SwapWindow(window);
    SDL_Delay(8); // Cap launcher framerate to ~120 fps
  }

  // 8. Clean Teardown
  if (document) {
    document->Close(); // Rule R-05: never delete, use Close()
  }
  Rml::Shutdown();
  RmlGL3::Shutdown();

  SDL_GL_DestroyContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
