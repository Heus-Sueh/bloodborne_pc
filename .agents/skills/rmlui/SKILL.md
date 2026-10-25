---
name: rmlui
description: >-
  Architectural patterns, RCSS styling constraints, data binding, and lifecycle guidelines
  for RmlUi 6.x native interfaces with SDL3 and OpenGL. Use when developing, styling,
  or debugging launcher/client UI templates (.rml), stylesheets (.rcss), and C++ view models.
---

# RmlUi UI Architecture & RCSS Guidelines

Definitive standards and operational constraints for developing reactive graphical interfaces using **RmlUi 6.x**, **SDL3**, and **OpenGL 3.3 Core**.

---

## 1. Core RCSS & Layout Constraints

RmlUi does not include a default browser-like user-agent stylesheet. Elements behave fundamentally different from HTML5 web standards unless explicitly styled:

### 1.1 Universal Block Reset
Every RCSS stylesheet must preserve the universal block reset. Without this, standard block elements default to `display: inline` and collapse or fail to respect padding and dimensions:

```css
div, p, h1, h2, h3, h4, section, header, footer {
    display: block;
    box-sizing: border-box;
}
```

### 1.2 Font Family Specifications
- RCSS `font-family` accepts **only a single string identifier**, never comma-separated fallback lists:
  ```css
  /* PASS */
  font-family: Noto Sans;

  /* FAIL - RmlUi parser fails or drops property */
  font-family: "Noto Sans", sans-serif, Arial;
  ```
- Fonts must be loaded through the RmlUi FreeType font engine (`Rml::LoadFontFace`) before document load.

### 1.3 Container & Viewport Resets
When toggling visibility or switching navigation tabs, container scroll offsets do not reset automatically. Always explicitly reset scroll positions on tab switch:
```cpp
if (content_element) {
    content_element->SetScrollTop(0.0f);
}
```

### 1.4 Strict XML / RML Formatting
RML is strictly parsed as XML. All void tags must be self-closing:
```xml
<!-- PASS -->
<input type="text" id="game_dir" />
<img src="banner.png" />
<br/>

<!-- FAIL - Syntax/Parser Error in RmlUi -->
<input type="text" id="game_dir">
<img src="banner.png">
<br>
```

---

## 2. Reactive Data Binding & Model Architecture

RmlUi provides a reactive data-binding system that synchronizes C++ models directly with RML templates.

### 2.1 Model Declaration & Registration
Expose state variables and event callbacks to the RmlUi context:
```cpp
Rml::DataModelConstructor constructor = context->CreateDataModel("launcher_model");
if (!constructor) return false;

constructor.Bind("game_path", &model.game_path);
constructor.Bind("output_res", &model.output_res);
constructor.BindEvent("on_start_game", &LauncherModel::OnStartGame, this);
model_handle_ = constructor.GetModelHandle();
```

### 2.2 Dirty Variable Notifications
Whenever C++ code mutates a bound property outside of an RML input event, explicitly notify RmlUi to re-render the affected DOM nodes:
```cpp
void LauncherModel::SetResolution(const std::string& res) {
    settings_.output_res = res;
    if (model_handle_) {
        model_handle_.DirtyVariable("output_res");
    }
}
```

### 2.3 Decoupling UI from Business Logic
Keep RmlUi dependencies completely isolated within the launcher view/controller layer:
- **`ConfigManager`** (`src/config_manager.hpp`): Pure C++ domain logic and INI persistence. Zero RmlUi or OpenGL headers.
- **`LauncherModel`** (`launcher/src/launcher_model.h/.cpp`): Reactive data-binding layer that binds UI state to `ConfigManager` properties.
- **RML / RCSS** (`launcher/assets/`): Pure presentation markup and styling.

---

## 3. Platform & Event Loop Lifecycle

### 3.1 Input Handling with SDL3
- Forward SDL3 events into RmlUi context via the SDL platform backend:
  ```cpp
  RmlSDL::InputEventHandler(context, event);
  ```
- Handle text input composition and clipboard actions using SDL3 text input APIs (`SDL_StartTextInput`, `SDL_StopTextInput`).

### 3.2 Context Updates & Rendering
The rendering loop must strictly follow:
1. Process system and window events (`SDL_PollEvent`).
2. Update the RmlUi data model and timers (`context->Update()`).
3. Clear the frame buffer (`glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)`).
4. Render RmlUi documents (`context->Render()`).
5. Swap presentation buffers (`SDL_GL_SwapWindow`).
