// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "launcher_model.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_dialog.h>
#include <iostream>
#include <memory>

namespace bbport {

namespace {

Rml::Variant SettingValueToVariant(const SettingValue& val) {
  if (std::holds_alternative<bool>(val)) return Rml::Variant(std::get<bool>(val));
  if (std::holds_alternative<int>(val)) return Rml::Variant(std::get<int>(val));
  if (std::holds_alternative<double>(val)) return Rml::Variant(static_cast<float>(std::get<double>(val)));
  return Rml::Variant(std::get<std::string>(val));
}

SettingValue VariantToSettingValue(const Rml::Variant& v, SettingType type) {
  switch (type) {
    case SettingType::Bool:
      return v.Get<bool>();
    case SettingType::Int:
      return v.Get<int>();
    case SettingType::Float:
      return static_cast<double>(v.Get<float>());
    default:
      return std::string(v.Get<Rml::String>());
  }
}

struct FolderDialogCtx {
  LauncherCore* core;
  std::string setting_key;
};

struct ExtractDialogCtx {
  LauncherCore* core;
  SDL_Window* window;
  std::string dump_path;
};

} // namespace

LauncherModel::LauncherModel(Rml::Context* context, SDL_Window* window, LauncherCore& core)
    : context_(context), window_(window), core_(core) {
  SyncModsAndPatches();
  SyncTrophies();
  SyncTranslations();
  BindModel();
}

void LauncherModel::Update() {
  ApplyDelta(core_.Poll());
}

void LauncherModel::BindModel() {
  Rml::DataModelConstructor c = context_->CreateDataModel("launcher");
  if (!c) {
    std::cerr << "LauncherModel: Failed to create RmlUi data model 'launcher'\n";
    return;
  }

  // Register list element structs (including ModItem::path used by launcher.rml).
  if (auto h = c.RegisterStruct<ModItem>()) {
    h.RegisterMember("id", &ModItem::id);
    h.RegisterMember("name", &ModItem::name);
    h.RegisterMember("author", &ModItem::author);
    h.RegisterMember("path", &ModItem::path);
    h.RegisterMember("enabled", &ModItem::enabled);
    h.RegisterMember("order", &ModItem::order);
  }
  c.RegisterArray<std::vector<ModItem>>();

  if (auto h = c.RegisterStruct<PatchItem>()) {
    h.RegisterMember("id", &PatchItem::id);
    h.RegisterMember("name", &PatchItem::name);
    h.RegisterMember("author", &PatchItem::author);
    h.RegisterMember("path", &PatchItem::path);
    h.RegisterMember("enabled", &PatchItem::enabled);
  }
  c.RegisterArray<std::vector<PatchItem>>();

  if (auto h = c.RegisterStruct<TrophyItem>()) {
    h.RegisterMember("id", &TrophyItem::id);
    h.RegisterMember("name", &TrophyItem::name);
    h.RegisterMember("detail", &TrophyItem::detail);
    h.RegisterMember("grade_str", &TrophyItem::grade_str);
    h.RegisterMember("icon_path", &TrophyItem::icon_path);
    h.RegisterMember("date_str", &TrophyItem::date_str);
    h.RegisterMember("unlocked", &TrophyItem::unlocked);
    h.RegisterMember("hidden", &TrophyItem::hidden);
  }
  c.RegisterArray<std::vector<TrophyItem>>();

  // Bind every storable Game Setting & Launcher Setting dynamically from SettingsSchema.
  for (const auto& def : core_.Schema().Items()) {
    if (def.store == SettingStoreKind::None) {
      continue;
    }
    const std::string key = def.key;
    const SettingType type = def.type;
    c.BindFunc(
        key,
        [this, key](Rml::Variant& out) {
          if (key == "ui_language") {
            out = core_.UiLanguage();
            return;
          }
          if (auto v = core_.Settings().Get(key)) {
            out = SettingValueToVariant(*v);
          }
        },
        [this, key, type](const Rml::Variant& in) {
          ApplyDelta(core_.SetSetting(key, VariantToSettingValue(in, type)).delta);
        });
  }

  // Single generic setting mutation callback for buttons/selectors: set_setting('key', 'val')
  c.BindEventCallback("set_setting", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.size() < 2) return;
    const std::string key = args[0].Get<Rml::String>();
    const SettingDef* def = core_.Schema().Find(key);
    if (!def) return;
    ApplyDelta(core_.SetSetting(key, VariantToSettingValue(args[1], def->type)).delta);
  });

  // Read-only state projections (zero duplicated state fields).
  c.Bind("active_tab", &active_tab_);
  c.Bind("copy_btn_text", &copy_btn_text_);
  c.BindFunc("status_text", [this](Rml::Variant& v) { v = core_.Translate(core_.GameStatus().status_key); });
  c.BindFunc("is_valid", [this](Rml::Variant& v) { v = core_.GameStatus().valid; });
  c.BindFunc("is_update_needed", [this](Rml::Variant& v) {
    const auto& g = core_.GameStatus();
    v = g.valid && !g.is_109;
  });
  c.BindFunc("game_version", [this](Rml::Variant& v) { v = core_.GameStatus().version; });
  c.BindFunc("game_title_id", [this](Rml::Variant& v) { v = core_.GameStatus().title_id; });
  c.BindFunc("console_log", [this](Rml::Variant& v) { v = core_.ConsoleLog(); });
  c.BindFunc("is_game_running", [this](Rml::Variant& v) { v = core_.GameProcess().running; });
  c.BindFunc("is_extracting", [this](Rml::Variant& v) { v = core_.Extraction().active; });
  c.BindFunc("extract_progress_pct", [this](Rml::Variant& v) {
    v = std::to_string(core_.Extraction().progress_pct);
  });
  c.BindFunc("extract_progress_text", [this](Rml::Variant& v) { v = core_.Extraction().status_text; });
  c.BindFunc("trophy_progress_pct", [this](Rml::Variant& v) { v = core_.TrophySummary().progress_pct; });
  c.BindFunc("trophy_progress_text", [this](Rml::Variant& v) { v = core_.TrophyProgressText(); });

  // Vector views synced on dirty delta.
  c.Bind("mods_list", &mods_view_);
  c.Bind("patches_list", &patches_view_);
  c.Bind("trophies_list", &trophies_view_);

  // Translations.
  c.Bind("tr_tab_play", &tr_tab_play_);
  c.Bind("tr_tab_graphics", &tr_tab_graphics_);
  c.Bind("tr_tab_effects", &tr_tab_effects_);
  c.Bind("tr_tab_controls", &tr_tab_controls_);
  c.Bind("tr_tab_mods", &tr_tab_mods_);
  c.Bind("tr_tab_trophies", &tr_tab_trophies_);
  c.Bind("tr_tab_online", &tr_tab_online_);
  c.Bind("tr_tab_log", &tr_tab_log_);
  c.Bind("tr_launch_game", &tr_launch_game_);
  c.Bind("tr_stop_game", &tr_stop_game_);

  // UI & LauncherCore command bindings.
  c.BindEventCallback("set_tab", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty()) return;
    active_tab_ = args[0].Get<Rml::String>();
    if (active_tab_ == "mods") ApplyDelta(core_.RefreshContent().delta);
    else if (active_tab_ == "trophies") ApplyDelta(core_.RefreshTrophies().delta);
    model_handle_.DirtyVariable("active_tab");
  });

  c.BindEventCallback("set_language", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty()) return;
    ApplyDelta(core_.SetSetting("ui_language", std::string(args[0].Get<Rml::String>())).delta);
  });

  c.BindEventCallback("save", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ApplyDelta(core_.SaveSettings().delta);
  });

  c.BindEventCallback("play", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ApplyDelta(core_.LaunchGame().delta);
  });
  c.BindEventCallback("stop_game", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    ApplyDelta(core_.StopGame().delta);
  });

  c.BindEventCallback("choose_game_dir", [this](auto, auto&, auto&) { ChooseFolderForSetting("game_dir"); });
  c.BindEventCallback("choose_user_dir", [this](auto, auto&, auto&) { ChooseFolderForSetting("user_dir"); });
  c.BindEventCallback("choose_mods_dir", [this](auto, auto&, auto&) { ChooseFolderForSetting("mods_dir"); });
  c.BindEventCallback("extract_dump", [this](auto, auto&, auto&) { ChooseDumpToExtract(); });
  c.BindEventCallback("install_update", [this](auto, auto&, auto&) { ChooseUpdateToInstall(); });

  c.BindEventCallback("toggle_mod", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (!args.empty()) ApplyDelta(core_.ToggleMod(args[0].Get<Rml::String>()).delta);
  });
  c.BindEventCallback("move_mod_up", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (!args.empty()) ApplyDelta(core_.MoveMod(args[0].Get<Rml::String>(), -1).delta);
  });
  c.BindEventCallback("move_mod_down", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (!args.empty()) ApplyDelta(core_.MoveMod(args[0].Get<Rml::String>(), +1).delta);
  });
  c.BindEventCallback("toggle_patch", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (!args.empty()) ApplyDelta(core_.TogglePatch(args[0].Get<Rml::String>()).delta);
  });

  c.BindEventCallback("clear_log", [this](auto, auto&, auto&) {
    copy_btn_text_ = "COPY";
    model_handle_.DirtyVariable("copy_btn_text");
    ApplyDelta(core_.ClearLog().delta);
  });
  c.BindEventCallback("copy_log", [this](auto, auto&, auto&) {
    SDL_SetClipboardText(core_.ConsoleLog().c_str());
    copy_btn_text_ = "COPIED!";
    model_handle_.DirtyVariable("copy_btn_text");
  });
  c.BindEventCallback("save_log", [this](auto, auto&, auto&) { ChooseSaveLogPath(); });

  model_handle_ = c.GetModelHandle();
}

void LauncherModel::ApplyDelta(const StateDelta& delta) {
  if (!model_handle_ || delta.Empty()) {
    return;
  }

  if (delta.Contains("mods_list") || delta.Contains("patches_list")) {
    SyncModsAndPatches();
  }
  if (delta.Contains("trophies_list")) {
    SyncTrophies();
  }
  if (delta.Contains("translations")) {
    SyncTranslations();
    for (const char* k : {"tr_tab_play", "tr_tab_graphics", "tr_tab_effects", "tr_tab_controls",
                          "tr_tab_mods", "tr_tab_trophies", "tr_tab_online", "tr_tab_log",
                          "tr_launch_game", "tr_stop_game"}) {
      model_handle_.DirtyVariable(k);
    }
  }

  for (const auto& key : delta.dirty_keys) {
    if (key != "translations") {
      model_handle_.DirtyVariable(key);
    }
  }

  if (delta.focus_log_tab) {
    active_tab_ = "log";
    model_handle_.DirtyVariable("active_tab");
  }
}

void LauncherModel::SyncModsAndPatches() {
  mods_view_ = core_.Mods();
  patches_view_ = core_.Patches();
}

void LauncherModel::SyncTrophies() {
  trophies_view_ = core_.Trophies();
}

void LauncherModel::SyncTranslations() {
  tr_tab_play_ = core_.Translate("tab_play");
  tr_tab_graphics_ = core_.Translate("tab_graphics");
  tr_tab_effects_ = core_.Translate("tab_effects");
  tr_tab_controls_ = core_.Translate("tab_controls");
  tr_tab_mods_ = core_.Translate("tab_mods");
  tr_tab_trophies_ = core_.Translate("tab_trophies");
  tr_tab_online_ = core_.Translate("tab_online");
  tr_tab_log_ = core_.Translate("tab_log");
  tr_launch_game_ = core_.Translate("launch_game");
  tr_stop_game_ = core_.Translate("stop_game");
}

void LauncherModel::ChooseFolderForSetting(const char* setting_key) {
  auto* ctx = new FolderDialogCtx{&core_, setting_key};
  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int) {
        std::unique_ptr<FolderDialogCtx> c(static_cast<FolderDialogCtx*>(userdata));
        if (filelist && *filelist && **filelist) {
          c->core->PostSetting(c->setting_key, std::string(*filelist));
        }
      },
      ctx, window_, nullptr, false);
}

void LauncherModel::ChooseDumpToExtract() {
  const SDL_DialogFileFilter filters[] = {{"PlayStation Dump (*.pkg, *.iso)", "pkg;iso"}};
  auto* ctx = new ExtractDialogCtx{&core_, window_, {}};
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int) {
        std::unique_ptr<ExtractDialogCtx> c(static_cast<ExtractDialogCtx*>(userdata));
        if (!filelist || !*filelist || !**filelist) {
          return;
        }
        c->dump_path = *filelist;
        auto* next_ctx = c.release();
        SDL_ShowOpenFolderDialog(
            [](void* udata, const char* const* flist, int) {
              std::unique_ptr<ExtractDialogCtx> d(static_cast<ExtractDialogCtx*>(udata));
              if (flist && *flist && **flist) {
                d->core->PostExtraction(d->dump_path, *flist);
              }
            },
            next_ctx, next_ctx->window, nullptr, false);
      },
      ctx, window_, filters, 1, nullptr, false);
}

void LauncherModel::ChooseUpdateToInstall() {
  const SDL_DialogFileFilter filters[] = {{"Update PKG (*.pkg)", "pkg"}};
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int) {
        if (filelist && *filelist && **filelist) {
          static_cast<LauncherCore*>(userdata)->PostUpdateInstall(*filelist);
        }
      },
      &core_, window_, filters, 1, nullptr, false);
}

void LauncherModel::ChooseSaveLogPath() {
  SDL_ShowSaveFileDialog(
      [](void* userdata, const char* const* filelist, int) {
        if (filelist && *filelist && **filelist) {
          static_cast<LauncherCore*>(userdata)->PostSaveLog(*filelist);
        }
      },
      &core_, window_, nullptr, 0, "bbport_log.txt");
}

} // namespace bbport
