// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef RMLUI_STATIC_LIB
#define RMLUI_STATIC_LIB
#endif

#include "bbport_quick_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <fmt/format.h>

#include "bbport_effect_flags.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "runtime_gpu_abi.h"
#include "shim/bb_env.h"

extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

namespace BbQuickMenu {
namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_open{false};
std::atomic<bool> g_needs_refresh{false};
bool g_rml_initialized = false;

void UnPremultiplyRgba(const Rml::byte* src, Rml::byte* dst) {
    const Rml::byte a = src[3];
    if (a == 0) {
        dst[0] = 255;
        dst[1] = 255;
        dst[2] = 255;
        dst[3] = 0;
    } else if (a == 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
    } else {
        dst[0] = Rml::byte(std::min(255, (int(src[0]) * 255) / int(a)));
        dst[1] = Rml::byte(std::min(255, (int(src[1]) * 255) / int(a)));
        dst[2] = Rml::byte(std::min(255, (int(src[2]) * 255) / int(a)));
        dst[3] = a;
    }
}

class QuickMenuSystemInterface final : public Rml::SystemInterface {
public:
    QuickMenuSystemInterface() : start_(std::chrono::steady_clock::now()) {}

    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

    void JoinPath(Rml::String& translated_path, const Rml::String& document_path,
                  const Rml::String& path) override {
        // Rml::SystemInterface::JoinPath strips a leading '/' assuming a web-root-relative path,
        // which corrupts POSIX absolute paths like "/home/.../TROP000.PNG" into "home/.../TROP000.PNG".
        if (!path.empty() && path[0] == '/') {
            translated_path = path;
            return;
        }
        Rml::SystemInterface::JoinPath(translated_path, document_path, path);
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) {
            std::fprintf(stderr, "RmlUi QuickMenu: %s\n", message.c_str());
        }
        return true;
    }

private:
    std::chrono::steady_clock::time_point start_;
};

struct CompiledGeom {
    struct Vert {
        ImVec2 pos;
        ImVec2 uv;
        ImU32 col;
    };
    std::vector<Vert> vertices;
    std::vector<int> indices;
};

class QuickMenuRenderInterface final : public Rml::RenderInterface {
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        auto* geom = new CompiledGeom();
        geom->vertices.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            Rml::byte rgba[4];
            UnPremultiplyRgba(&vertices[i].colour[0], rgba);
            geom->vertices[i] = {
                .pos = {vertices[i].position.x, vertices[i].position.y},
                .uv = {vertices[i].tex_coord.x, vertices[i].tex_coord.y},
                .col = IM_COL32(rgba[0], rgba[1], rgba[2], rgba[3]),
            };
        }
        geom->indices.assign(indices.begin(), indices.end());
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geom);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        auto* geom = reinterpret_cast<const CompiledGeom*>(handle);
        if (!geom || geom->indices.empty() || geom->vertices.empty()) {
            return;
        }
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        if (!draw) {
            return;
        }

        if (scissor_enabled_) {
            draw->PushClipRect({float(scissor_.Left()), float(scissor_.Top())},
                               {float(scissor_.Right()), float(scissor_.Bottom())}, false);
        }

        const bool has_tex = (texture != 0);
        if (has_tex) {
            auto* tex = reinterpret_cast<ImTextureData*>(texture);
            draw->PushTexture(tex->GetTexRef());
        }

        const ImVec2 white_uv = ImGui::GetFontTexUvWhitePixel();
        const int vtx_count = int(geom->vertices.size());
        const int idx_count = int(geom->indices.size());
        draw->PrimReserve(idx_count, vtx_count);

        ImDrawVert* vtx_dst = draw->_VtxWritePtr;
        ImDrawIdx* idx_dst = draw->_IdxWritePtr;
        const ImDrawIdx base_idx = draw->_VtxCurrentIdx;

        for (int i = 0; i < vtx_count; ++i) {
            const auto& sv = geom->vertices[i];
            float px = sv.pos.x + translation.x;
            float py = sv.pos.y + translation.y;
            if (has_transform_) {
                const Rml::Vector4f tp = transform_ * Rml::Vector4f(px, py, 0.0f, 1.0f);
                if (tp.w != 0.0f && tp.w != 1.0f) {
                    px = tp.x / tp.w;
                    py = tp.y / tp.w;
                } else {
                    px = tp.x;
                    py = tp.y;
                }
            }
            vtx_dst[i].pos = {px, py};
            vtx_dst[i].uv = has_tex ? sv.uv : white_uv;
            vtx_dst[i].col = sv.col;
        }

        for (int i = 0; i < idx_count; ++i) {
            idx_dst[i] = ImDrawIdx(base_idx + ImDrawIdx(geom->indices[i]));
        }

        draw->_VtxWritePtr += vtx_count;
        draw->_IdxWritePtr += idx_count;
        draw->_VtxCurrentIdx += ImDrawIdx(vtx_count);

        if (has_tex) {
            draw->PopTexture();
        }
        if (scissor_enabled_) {
            draw->PopClipRect();
        }
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        delete reinterpret_cast<CompiledGeom*>(handle);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override {
        if (source.empty()) {
            return 0;
        }
        std::string resolved = source;
        if (!fs::exists(resolved) && !resolved.empty() && resolved[0] != '/') {
            const std::string with_slash = "/" + resolved;
            if (fs::exists(with_slash)) {
                resolved = with_slash;
            }
        }
        if (!fs::exists(resolved)) {
            const auto pos = source.find("user/");
            if (pos != Rml::String::npos && fs::exists(source.substr(pos))) {
                resolved = source.substr(pos);
            }
        }
        if (ImTextureData* tex = BbOverlay::GetOrLoadTexture(fs::path(resolved))) {
            texture_dimensions = {tex->Width, tex->Height};
            return reinterpret_cast<Rml::TextureHandle>(tex);
        }
        return 0;
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override {
        if (source.empty() || source_dimensions.x <= 0 || source_dimensions.y <= 0) {
            return 0;
        }
        auto* tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, source_dimensions.x, source_dimensions.y);
        auto* dst = static_cast<Rml::byte*>(tex->GetPixels());
        const std::size_t total_bytes =
            std::size_t(source_dimensions.x) * std::size_t(source_dimensions.y) * 4;
        for (std::size_t i = 0; i + 3 < total_bytes && i + 3 < source.size(); i += 4) {
            UnPremultiplyRgba(&source[i], &dst[i]);
        }
        ImGui::RegisterUserTexture(tex);
        return reinterpret_cast<Rml::TextureHandle>(tex);
    }

    void ReleaseTexture(Rml::TextureHandle /*texture*/) override {
        // Textures are kept registered with ImGui's Vulkan backend for the session.
    }

    void EnableScissorRegion(bool enable) override { scissor_enabled_ = enable; }

    void SetScissorRegion(Rml::Rectanglei region) override { scissor_ = region; }

    void SetTransform(const Rml::Matrix4f* transform) override {
        if (transform) {
            has_transform_ = true;
            transform_ = *transform;
        } else {
            has_transform_ = false;
        }
    }

private:
    bool scissor_enabled_ = false;
    Rml::Rectanglei scissor_{};
    bool has_transform_ = false;
    Rml::Matrix4f transform_ = Rml::Matrix4f::Identity();
};

struct QuickTrophyItem {
    int id = 0;
    Rml::String grade;       // "P", "G", "S", "B"
    Rml::String grade_label; // "PLATINA", "OURO", "PRATA", "BRONZE"
    Rml::String name;
    Rml::String detail;
    Rml::String status_text;
    Rml::String icon_src;
    bool has_icon = false;
    bool unlocked = false;
    bool selected = false;
};

struct QuickSettingItem {
    Rml::String id;
    Rml::String label;
    Rml::String value;
    bool selected = false;
};

struct QuickActionItem {
    Rml::String id;
    Rml::String icon;
    Rml::String label;
    Rml::String desc;
    bool selected = false;
};

constexpr const char* kQuickMenuRml = R"RML(
<rml>
<head>
    <title>PS4 Quick Menu</title>
    <style>
        div, p, h1, h2, h3, span, button {
            box-sizing: border-box;
        }
        div, p, h1, h2, h3 {
            display: block;
        }
        body {
            position: absolute;
            top: 0px;
            left: 0px;
            right: 0px;
            bottom: 0px;
            margin: 0px;
            padding: 0px;
            font-family: Source Sans 3;
            font-size: 14px;
            color: #f0f2f7;
            background-color: rgba(0, 0, 0, 105);
            overflow: hidden;
        }
        .lucide {
            display: inline-block;
            font-family: lucide;
            font-weight: normal;
            letter-spacing: 0px;
            vertical-align: middle;
        }
        #drawer {
            position: absolute;
            top: 0px;
            left: 0px;
            bottom: 0px;
            width: 540px;
            background-color: rgba(13, 17, 26, 246);
            border-right: 2px #0072ce;
            display: flex;
            flex-direction: column;
            padding: 20px 22px 16px 22px;
        }
        .ps4-header {
            display: flex;
            flex-direction: row;
            align-items: center;
            justify-content: space-between;
            padding-bottom: 12px;
            border-bottom: 1px #1e293b;
            margin-bottom: 12px;
        }
        .ps4-brand-left {
            display: flex;
            flex-direction: column;
        }
        .ps4-tag {
            font-family: Cinzel;
            font-size: 10px;
            font-weight: bold;
            letter-spacing: 2px;
            color: #3898ff;
            margin-bottom: 2px;
        }
        .ps4-title {
            font-family: Cinzel;
            font-size: 19px;
            font-weight: bold;
            letter-spacing: 1.5px;
            color: #ffffff;
        }
        .ps4-perf-pill {
            background-color: #131c2e;
            border: 1px #243554;
            border-radius: 4px;
            padding: 5px 10px;
            font-family: JetBrainsMono NF;
            font-size: 11px;
            color: #93c5fd;
        }
        .tab-bar {
            display: flex;
            flex-direction: row;
            gap: 8px;
            margin-bottom: 14px;
        }
        .tab-btn {
            flex: 1;
            display: flex;
            flex-direction: row;
            align-items: center;
            justify-content: center;
            gap: 7px;
            padding: 9px 10px;
            background-color: #131b2b;
            border: 1px #202d45;
            border-radius: 4px;
            color: #94a3b8;
            font-family: Cinzel;
            font-size: 11px;
            font-weight: bold;
            letter-spacing: 0.8px;
            cursor: pointer;
        }
        .tab-btn:hover {
            background-color: #1a263d;
            color: #ffffff;
        }
        .tab-btn.active {
            background-color: #005bb5;
            border-color: #4da3ff;
            color: #ffffff;
        }
        .summary-card {
            background-color: #131b2b;
            border: 1px #22314d;
            border-radius: 6px;
            padding: 12px 14px;
            margin-bottom: 10px;
        }
        .summary-top {
            display: flex;
            flex-direction: row;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 7px;
        }
        .summary-game {
            font-family: Cinzel;
            font-size: 13px;
            font-weight: bold;
            color: #f8fafc;
            letter-spacing: 1px;
        }
        .summary-pct {
            font-family: JetBrainsMono NF;
            font-size: 12px;
            font-weight: bold;
            color: #60a5fa;
        }
        .style-switch-btn {
            background-color: #162238;
            border: 1px #2f4b7c;
            border-radius: 4px;
            padding: 3px 8px;
            font-family: JetBrainsMono NF;
            font-size: 10px;
            font-weight: bold;
            color: #93c5fd;
            cursor: pointer;
        }
        .style-switch-btn:hover {
            background-color: #1f3354;
            border-color: #60a5fa;
            color: #ffffff;
        }
        .prog-track {
            width: 100%;
            height: 6px;
            background-color: #090d16;
            border-radius: 3px;
            overflow: hidden;
            margin-bottom: 10px;
        }
        .prog-fill {
            height: 100%;
            background-color: #0072ce;
        }
        .tier-row {
            display: flex;
            flex-direction: row;
            justify-content: space-between;
            gap: 6px;
        }
        .tier-pill {
            flex: 1;
            display: flex;
            flex-direction: row;
            align-items: center;
            justify-content: center;
            gap: 5px;
            background-color: #0d1320;
            border: 1px #1e2b44;
            border-radius: 4px;
            padding: 4px 6px;
            font-family: JetBrainsMono NF;
            font-size: 11px;
            font-weight: bold;
        }
        .tier-p { color: #cee0f8; border-color: #4b6b94; }
        .tier-g { color: #eabe4a; border-color: #70581c; }
        .tier-s { color: #c6ced8; border-color: #4f5966; }
        .tier-b { color: #ca7e48; border-color: #6b3f21; }
        .filter-row {
            display: flex;
            flex-direction: row;
            gap: 6px;
            margin-bottom: 10px;
        }
        .filter-btn {
            flex: 1;
            background-color: #111826;
            border: 1px #1f2b42;
            border-radius: 4px;
            padding: 6px 8px;
            color: #94a3b8;
            font-size: 11px;
            font-weight: bold;
            text-align: center;
            cursor: pointer;
        }
        .filter-btn:hover {
            background-color: #1a253a;
            color: #ffffff;
        }
        .filter-btn.active {
            background-color: #19325c;
            border-color: #3898ff;
            color: #ffffff;
        }
        .scroll-list {
            flex: 1;
            overflow-y: auto;
            padding-right: 4px;
        }
        .trophy-item {
            display: flex;
            flex-direction: row;
            align-items: center;
            gap: 12px;
            background-color: #111726;
            border: 1px #1c273d;
            border-radius: 5px;
            padding: 9px 11px;
            margin-bottom: 7px;
            cursor: pointer;
            opacity: 0.72;
        }
        .trophy-item.unlocked {
            opacity: 1.0;
            background-color: #141e33;
            border-color: #27395c;
        }
        .trophy-item.selected {
            border: 2px #3898ff;
            background-color: #172847;
            opacity: 1.0;
        }
        .trophy-art-box {
            width: 46px;
            height: 46px;
            flex-shrink: 0;
            background-color: #0a0e17;
            border: 1px #283652;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .trophy-art-img {
            width: 44px;
            height: 44px;
        }
        .trophy-cup-fallback {
            font-family: lucide;
            font-size: 20px;
        }
        .cup-p { color: #cee0f8; }
        .cup-g { color: #eabe4a; }
        .cup-s { color: #c6ced8; }
        .cup-b { color: #ca7e48; }
        .trophy-main {
            flex: 1;
            min-width: 0px;
            display: flex;
            flex-direction: column;
        }
        .trophy-top-line {
            display: flex;
            flex-direction: row;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 2px;
        }
        .trophy-name {
            font-size: 13px;
            font-weight: bold;
            color: #f8fafc;
        }
        .trophy-badge {
            font-family: JetBrainsMono NF;
            font-size: 10px;
            font-weight: bold;
            padding: 1px 6px;
            border-radius: 3px;
            background-color: #0b101b;
        }
        .trophy-desc {
            font-size: 11px;
            color: #94a3b8;
            margin-bottom: 3px;
        }
        .trophy-status {
            font-family: JetBrainsMono NF;
            font-size: 10px;
            color: #64748b;
        }
        .trophy-status.unlocked {
            color: #38bdf8;
        }
        .setting-row {
            display: flex;
            flex-direction: row;
            align-items: center;
            justify-content: space-between;
            background-color: #121929;
            border: 1px #1e2b45;
            border-radius: 5px;
            padding: 11px 14px;
            margin-bottom: 7px;
            cursor: pointer;
        }
        .setting-row:hover {
            background-color: #182338;
        }
        .setting-row.selected {
            background-color: #17294a;
            border: 2px #3898ff;
        }
        .setting-label {
            font-size: 13px;
            font-weight: bold;
            color: #e2e8f0;
        }
        .setting-val {
            font-family: JetBrainsMono NF;
            font-size: 12px;
            font-weight: bold;
            color: #60a5fa;
            background-color: #0c121e;
            border: 1px #233454;
            border-radius: 4px;
            padding: 4px 10px;
        }
        .action-card {
            display: flex;
            flex-direction: row;
            align-items: center;
            gap: 12px;
            background-color: #121929;
            border: 1px #1e2b45;
            border-radius: 6px;
            padding: 12px 14px;
            margin-bottom: 8px;
            cursor: pointer;
        }
        .action-card:hover {
            background-color: #182338;
        }
        .action-card.selected {
            background-color: #17294a;
            border: 2px #3898ff;
        }
        .action-icon {
            font-family: lucide;
            font-size: 18px;
            color: #3898ff;
            width: 24px;
            text-align: center;
        }
        .action-body {
            flex: 1;
            display: flex;
            flex-direction: column;
        }
        .action-title {
            font-size: 13px;
            font-weight: bold;
            color: #f8fafc;
        }
        .action-desc {
            font-size: 11px;
            color: #94a3b8;
        }
        .ps4-footer {
            padding-top: 10px;
            border-top: 1px #1e293b;
            margin-top: 8px;
            display: flex;
            flex-direction: row;
            justify-content: space-between;
            font-family: JetBrainsMono NF;
            font-size: 10px;
            color: #94a3b8;
        }
        scrollbarvertical { width: 6px; }
        scrollbarvertical slidertrack { background-color: transparent; }
        scrollbarvertical sliderbar { background-color: #283a5e; border-radius: 3px; min-height: 24px; }
        scrollbarvertical sliderbar:hover { background-color: #3898ff; }
    </style>
</head>
<body data-model="quickmenu">
    <div id="drawer">
        <div class="ps4-header">
            <div class="ps4-brand-left">
                <div class="ps4-tag">{{ header_tag }}</div>
                <div class="ps4-title">BLOODBORNE</div>
            </div>
            <div class="ps4-perf-pill">{{ perf_text }}</div>
        </div>

        <div class="tab-bar">
            <button class="tab-btn" data-class-active="active_tab == 'trophies'" data-event-click="set_tab('trophies')">
                <span class="lucide">&#xE373;</span>
                <span>{{ tr_tab_trophies }}</span>
            </button>
            <button class="tab-btn" data-class-active="active_tab == 'settings'" data-event-click="set_tab('settings')">
                <span class="lucide">&#xE11D;</span>
                <span>{{ tr_tab_settings }}</span>
            </button>
            <button class="tab-btn" data-class-active="active_tab == 'system'" data-event-click="set_tab('system')">
                <span class="lucide">&#xE154;</span>
                <span>{{ tr_tab_system }}</span>
            </button>
        </div>

        <!-- TAB 1: TROPHIES -->
        <div style="flex: 1; display: flex; flex-direction: column; overflow: hidden;" data-if="active_tab == 'trophies'">
            <div class="summary-card">
                <div class="summary-top">
                    <span class="summary-game">Bloodborne™ Trophies</span>
                    <div style="display: flex; flex-direction: row; align-items: center; gap: 8px;">
                        <button class="style-switch-btn" data-event-click="cycle_trophy_style">{{ trophy_style_badge }}</button>
                        <span class="summary-pct">{{ trophy_summary_text }}</span>
                    </div>
                </div>
                <div class="prog-track">
                    <div class="prog-fill" data-style-width="trophy_progress_pct + '%'"></div>
                </div>
                <div class="tier-row">
                    <div class="tier-pill tier-p"><span class="lucide">&#xE373;</span><span>P {{ count_p }}</span></div>
                    <div class="tier-pill tier-g"><span class="lucide">&#xE373;</span><span>G {{ count_g }}</span></div>
                    <div class="tier-pill tier-s"><span class="lucide">&#xE373;</span><span>S {{ count_s }}</span></div>
                    <div class="tier-pill tier-b"><span class="lucide">&#xE373;</span><span>B {{ count_b }}</span></div>
                </div>
            </div>

            <div class="filter-row">
                <button class="filter-btn" data-class-active="filter_mode == 'all'" data-event-click="set_filter('all')">{{ tr_filter_all }}</button>
                <button class="filter-btn" data-class-active="filter_mode == 'unlocked'" data-event-click="set_filter('unlocked')">{{ tr_filter_unlocked }}</button>
                <button class="filter-btn" data-class-active="filter_mode == 'locked'" data-event-click="set_filter('locked')">{{ tr_filter_locked }}</button>
                <button class="filter-btn" data-class-active="reveal_hidden" data-event-click="toggle_hidden">{{ tr_reveal_hidden }}</button>
            </div>

            <div id="trophy-scroll" class="scroll-list">
                <div class="trophy-item" data-for="t : trophies"
                     data-class-unlocked="t.unlocked"
                     data-class-selected="t.selected"
                     data-event-click="preview_trophy(t.id)">
                    <div class="trophy-art-box">
                        <img class="trophy-art-img" data-if="t.has_icon" data-attr-src="t.icon_src" />
                        <span class="trophy-cup-fallback" data-if="!t.has_icon"
                              data-class-cup-p="t.grade == 'P'"
                              data-class-cup-g="t.grade == 'G'"
                              data-class-cup-s="t.grade == 'S'"
                              data-class-cup-b="t.grade == 'B'">&#xE373;</span>
                    </div>
                    <div class="trophy-main">
                        <div class="trophy-top-line">
                            <span class="trophy-name">{{ t.name }}</span>
                            <span class="trophy-badge"
                                  data-class-cup-p="t.grade == 'P'"
                                  data-class-cup-g="t.grade == 'G'"
                                  data-class-cup-s="t.grade == 'S'"
                                  data-class-cup-b="t.grade == 'B'">{{ t.grade_label }}</span>
                        </div>
                        <div class="trophy-desc">{{ t.detail }}</div>
                        <div class="trophy-status" data-class-unlocked="t.unlocked">{{ t.status_text }}</div>
                    </div>
                </div>
            </div>
        </div>

        <!-- TAB 2: QUICK GRAPHICS & GAME SETTINGS -->
        <div id="settings-scroll" class="scroll-list" data-if="active_tab == 'settings'">
            <div class="setting-row" data-for="s : settings"
                 data-class-selected="s.selected"
                 data-event-click="cycle_setting(s.id, 1)">
                <span class="setting-label">{{ s.label }}</span>
                <span class="setting-val">{{ s.value }}</span>
            </div>
        </div>

        <!-- TAB 3: SYSTEM & ACTIONS -->
        <div id="system-scroll" class="scroll-list" data-if="active_tab == 'system'">
            <div class="action-card" data-for="a : actions"
                 data-class-selected="a.selected"
                 data-event-click="run_action(a.id)">
                <span class="action-icon">{{ a.icon }}</span>
                <div class="action-body">
                    <span class="action-title">{{ a.label }}</span>
                    <span class="action-desc">{{ a.desc }}</span>
                </div>
            </div>
        </div>

        <div class="ps4-footer">
            <span>{{ footer_nav }}</span>
            <span>{{ footer_close }}</span>
        </div>
    </div>
</body>
</rml>
)RML";

class QuickMenuController {
public:
    bool Init(u32 w, u32 h) {
        if (!Rml::Initialise()) {
            return false;
        }
        LoadFonts();
        context_ = Rml::CreateContext("bb_quick_menu", Rml::Vector2i(int(w), int(h)));
        if (!context_) {
            return false;
        }
        if (!BindDataModel()) {
            return false;
        }
        document_ = context_->LoadDocumentFromMemory(kQuickMenuRml, "quick_menu.rml");
        if (!document_) {
            return false;
        }
        document_->Show();
        RefreshAll();
        return true;
    }

    void LoadFonts() {
        const Rml::Span<const Rml::byte> embedded_font(
            reinterpret_cast<const Rml::byte*>(bb_font_ttf),
            std::size_t(bb_font_ttf_end - bb_font_ttf));

        auto load_disk_or_embedded = [&](const char* rel_path, const char* family,
                                         Rml::Style::FontWeight weight, bool fallback = false) {
            if (fs::exists(rel_path)) {
                if (Rml::LoadFontFace(rel_path, family, Rml::Style::FontStyle::Normal, weight, fallback)) {
                    return;
                }
            }
            Rml::LoadFontFace(embedded_font, family, Rml::Style::FontStyle::Normal, weight, fallback);
        };

        load_disk_or_embedded("launcher/assets/fonts/SourceSans3-Regular.ttf", "Source Sans 3",
                              Rml::Style::FontWeight::Normal);
        load_disk_or_embedded("launcher/assets/fonts/SourceSans3-Bold.ttf", "Source Sans 3",
                              Rml::Style::FontWeight::Bold);
        load_disk_or_embedded("launcher/assets/fonts/Cinzel-Regular.ttf", "Cinzel",
                              Rml::Style::FontWeight::Normal);
        load_disk_or_embedded("launcher/assets/fonts/Cinzel-Bold.ttf", "Cinzel",
                              Rml::Style::FontWeight::Bold);
        load_disk_or_embedded("launcher/assets/fonts/JetBrainsMono-Regular.ttf", "JetBrainsMono NF",
                              Rml::Style::FontWeight::Normal, true);
        load_disk_or_embedded("launcher/assets/fonts/JetBrainsMono-Bold.ttf", "JetBrainsMono NF",
                              Rml::Style::FontWeight::Bold);
        if (fs::exists("launcher/assets/fonts/lucide.ttf")) {
            Rml::LoadFontFace("launcher/assets/fonts/lucide.ttf", "lucide",
                              Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal);
        } else {
            Rml::LoadFontFace(embedded_font, "lucide", Rml::Style::FontStyle::Normal,
                              Rml::Style::FontWeight::Normal);
        }
        // Embedded DejaVu Sans (Cyrillic/Latin) as universal fallback.
        Rml::LoadFontFace(embedded_font, "DejaVu Sans", Rml::Style::FontStyle::Normal,
                          Rml::Style::FontWeight::Normal, true);
    }

    bool BindDataModel() {
        Rml::DataModelConstructor c = context_->CreateDataModel("quickmenu");
        if (!c) {
            return false;
        }
        if (auto h = c.RegisterStruct<QuickTrophyItem>()) {
            h.RegisterMember("id", &QuickTrophyItem::id);
            h.RegisterMember("grade", &QuickTrophyItem::grade);
            h.RegisterMember("grade_label", &QuickTrophyItem::grade_label);
            h.RegisterMember("name", &QuickTrophyItem::name);
            h.RegisterMember("detail", &QuickTrophyItem::detail);
            h.RegisterMember("status_text", &QuickTrophyItem::status_text);
            h.RegisterMember("icon_src", &QuickTrophyItem::icon_src);
            h.RegisterMember("has_icon", &QuickTrophyItem::has_icon);
            h.RegisterMember("unlocked", &QuickTrophyItem::unlocked);
            h.RegisterMember("selected", &QuickTrophyItem::selected);
        }
        c.RegisterArray<std::vector<QuickTrophyItem>>();

        if (auto h = c.RegisterStruct<QuickSettingItem>()) {
            h.RegisterMember("id", &QuickSettingItem::id);
            h.RegisterMember("label", &QuickSettingItem::label);
            h.RegisterMember("value", &QuickSettingItem::value);
            h.RegisterMember("selected", &QuickSettingItem::selected);
        }
        c.RegisterArray<std::vector<QuickSettingItem>>();

        if (auto h = c.RegisterStruct<QuickActionItem>()) {
            h.RegisterMember("id", &QuickActionItem::id);
            h.RegisterMember("icon", &QuickActionItem::icon);
            h.RegisterMember("label", &QuickActionItem::label);
            h.RegisterMember("desc", &QuickActionItem::desc);
            h.RegisterMember("selected", &QuickActionItem::selected);
        }
        c.RegisterArray<std::vector<QuickActionItem>>();

        c.Bind("active_tab", &active_tab_);
        c.Bind("filter_mode", &filter_mode_);
        c.Bind("reveal_hidden", &reveal_hidden_);
        c.Bind("header_tag", &header_tag_);
        c.Bind("trophy_style_badge", &trophy_style_badge_);
        c.Bind("perf_text", &perf_text_);
        c.Bind("trophy_summary_text", &trophy_summary_text_);
        c.Bind("trophy_progress_pct", &trophy_progress_pct_);
        c.Bind("count_p", &count_p_);
        c.Bind("count_g", &count_g_);
        c.Bind("count_s", &count_s_);
        c.Bind("count_b", &count_b_);
        c.Bind("tr_tab_trophies", &tr_tab_trophies_);
        c.Bind("tr_tab_settings", &tr_tab_settings_);
        c.Bind("tr_tab_system", &tr_tab_system_);
        c.Bind("tr_filter_all", &tr_filter_all_);
        c.Bind("tr_filter_unlocked", &tr_filter_unlocked_);
        c.Bind("tr_filter_locked", &tr_filter_locked_);
        c.Bind("tr_reveal_hidden", &tr_reveal_hidden_);
        c.Bind("footer_nav", &footer_nav_);
        c.Bind("footer_close", &footer_close_);
        c.Bind("trophies", &trophies_);
        c.Bind("settings", &settings_);
        c.Bind("actions", &actions_);

        c.BindEventCallback("set_tab", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                SwitchTab(args[0].Get<Rml::String>());
            }
        });
        c.BindEventCallback("set_filter", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                filter_mode_ = args[0].Get<Rml::String>();
                selected_index_ = 0;
                RefreshTrophies();
                model_.DirtyVariable("filter_mode");
            }
        });
        c.BindEventCallback("toggle_hidden", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            reveal_hidden_ = !reveal_hidden_;
            RefreshTrophies();
            model_.DirtyVariable("reveal_hidden");
        });
        c.BindEventCallback("preview_trophy", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                PreviewTrophyById(args[0].Get<int>());
            }
        });
        c.BindEventCallback("cycle_trophy_style", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            StepTrophyStyle(1);
            BbOverlay::TriggerTrophyPreview('B');
        });
        c.BindEventCallback("cycle_setting", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                const int dir = args.size() >= 2 ? args[1].Get<int>() : 1;
                CycleSettingById(args[0].Get<Rml::String>(), dir);
            }
        });
        c.BindEventCallback("run_action", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                ExecuteActionById(args[0].Get<Rml::String>());
            }
        });

        model_ = c.GetModelHandle();
        return true;
    }

    static bool IsPortuguese() {
        if (const char* m = bb_env("BB_MENU_LANGUAGE"); m && std::strncmp(m, "pt", 2) == 0) {
            return true;
        }
        if (const char* l = bb_env("BB_LANGUAGE")) {
            const int id = std::atoi(l);
            return id == 7 || id == 17;
        }
        return false;
    }

    static bool IsRussian() {
        if (const char* m = bb_env("BB_MENU_LANGUAGE"); m && std::strncmp(m, "ru", 2) == 0) {
            return true;
        }
        return BbSettings::Get().menu_language.load(std::memory_order_relaxed) ==
               BbSettings::MenuLanguage::Russian;
    }

    const char* Tr(const char* en, const char* pt, const char* ru) const {
        if (IsPortuguese()) return pt;
        if (IsRussian()) return ru;
        return en;
    }

    const char* TrophyStyleDisplayName(BbOverlay::TrophyStyle st) const {
        switch (st) {
        case BbOverlay::TrophyStyle::Ps5:
            return Tr("PS5 (Modern Top-Right)", "PS5 (Moderno Topo-Direito)", "PS5 (Современный)");
        case BbOverlay::TrophyStyle::Ps4White:
            return Tr("PS4 White (Classic)", "PS4 Branco (Clássico)", "PS4 Белый (Классика)");
        case BbOverlay::TrophyStyle::Ps4Black:
        default:
            return Tr("PS4 Black (FW 5.0+)", "PS4 Preto (FW 5.0+)", "PS4 Чёрный (FW 5.0+)");
        }
    }

    const char* TrophyStyleShortBadge(BbOverlay::TrophyStyle st) const {
        switch (st) {
        case BbOverlay::TrophyStyle::Ps5:
            return Tr("Style: PS5", "Estilo: PS5", "Стиль: PS5");
        case BbOverlay::TrophyStyle::Ps4White:
            return Tr("Style: PS4 White", "Estilo: PS4 Branco", "Стиль: PS4 Белый");
        case BbOverlay::TrophyStyle::Ps4Black:
        default:
            return Tr("Style: PS4 Black", "Estilo: PS4 Preto", "Стиль: PS4 Чёрный");
        }
    }

    void StepTrophyStyle(int dir) {
        static constexpr BbOverlay::TrophyStyle kOrder[3] = {
            BbOverlay::TrophyStyle::Ps5,
            BbOverlay::TrophyStyle::Ps4Black,
            BbOverlay::TrophyStyle::Ps4White,
        };
        const BbOverlay::TrophyStyle cur = BbOverlay::GetTrophyStyle();
        int idx = 0;
        for (int i = 0; i < 3; ++i) {
            if (kOrder[i] == cur) idx = i;
        }
        idx = (idx + dir + 3) % 3;
        BbOverlay::SetTrophyStyle(kOrder[idx]);
        RefreshStrings();
        RefreshSettings();
        RefreshActions();
    }

    void RefreshStrings() {
        const BbOverlay::TrophyStyle st = BbOverlay::GetTrophyStyle();
        header_tag_ = (st == BbOverlay::TrophyStyle::Ps5)
            ? "PLAYSTATION 5 CONTROL CENTER"
            : "PLAYSTATION 4 QUICK MENU";
        trophy_style_badge_ = TrophyStyleShortBadge(st);
        tr_tab_trophies_ = Tr("TROPHIES", "TROFÉUS", "ТРОФЕИ");
        tr_tab_settings_ = Tr("GRAPHICS", "GRÁFICOS", "ГРАФИКА");
        tr_tab_system_ = Tr("SYSTEM", "SISTEMA", "СИСТЕМА");
        tr_filter_all_ = Tr("All", "Todos", "Все");
        tr_filter_unlocked_ = Tr("Unlocked", "Obtidos", "Открытые");
        tr_filter_locked_ = Tr("Locked", "Faltam", "Закрытые");
        tr_reveal_hidden_ = Tr("Spoilers", "Ocultos", "Скрытые");
        footer_nav_ = Tr("[L1/R1] Tab  •  [D-Pad] Select  •  [Cross/Enter] Activate",
                         "[L1/R1] Aba  •  [D-Pad] Navegar  •  [✕/Enter] Ativar",
                         "[L1/R1] Вкладка  •  [D-Pad] Выбор  •  [✕/Enter] Применить");
        footer_close_ = Tr("[Home / F1 / Circle] Resume",
                           "[Home / F1 / ○] Voltar ao Jogo",
                           "[Home / F1 / ○] В игру");
        model_.DirtyVariable("header_tag");
        model_.DirtyVariable("trophy_style_badge");
        model_.DirtyVariable("tr_tab_trophies");
        model_.DirtyVariable("tr_tab_settings");
        model_.DirtyVariable("tr_tab_system");
        model_.DirtyVariable("tr_filter_all");
        model_.DirtyVariable("tr_filter_unlocked");
        model_.DirtyVariable("tr_filter_locked");
        model_.DirtyVariable("tr_reveal_hidden");
        model_.DirtyVariable("footer_nav");
        model_.DirtyVariable("footer_close");
    }

    void RefreshTrophies() {
        raw_catalog_ = BbOverlay::GetTrophyCatalogSnapshot();
        int total = int(raw_catalog_.size());
        int unlocked_total = 0;
        int got_p = 0, tot_p = 0, got_g = 0, tot_g = 0, got_s = 0, tot_s = 0, got_b = 0, tot_b = 0;

        trophies_.clear();
        for (const auto& t : raw_catalog_) {
            if (t.unlocked) ++unlocked_total;
            if (t.grade == 'P') { ++tot_p; if (t.unlocked) ++got_p; }
            else if (t.grade == 'G') { ++tot_g; if (t.unlocked) ++got_g; }
            else if (t.grade == 'S') { ++tot_s; if (t.unlocked) ++got_s; }
            else { ++tot_b; if (t.unlocked) ++got_b; }

            if (filter_mode_ == "unlocked" && !t.unlocked) continue;
            if (filter_mode_ == "locked" && t.unlocked) continue;

            QuickTrophyItem item;
            item.id = t.id;
            item.grade = Rml::String(1, t.grade);
            item.grade_label = t.grade == 'P' ? Tr("PLATINUM", "PLATINA", "ПЛАТИНА")
                             : t.grade == 'G' ? Tr("GOLD", "OURO", "ЗОЛОТО")
                             : t.grade == 'S' ? Tr("SILVER", "PRATA", "СЕРЕБРО")
                                              : Tr("BRONZE", "BRONZE", "БРОНЗА");
            const bool masked = t.hidden && !t.unlocked && !reveal_hidden_;
            item.name = masked ? Tr("Hidden Trophy", "Troféu Oculto", "Скрытый трофей") : t.name;
            item.detail = masked
                ? Tr("Press Triangle or click Spoilers to reveal details.",
                     "Pressione Triângulo ou clique em Ocultos para revelar.",
                     "Нажмите Треугольник или Скрытые, чтобы показать описание.")
                : t.detail;
            if (t.unlocked) {
                item.status_text = t.unlocked_at.empty()
                    ? Tr("UNLOCKED", "CONQUISTADO", "ПОЛУЧЕН")
                    : fmt::format("{} • {}", Tr("UNLOCKED", "CONQUISTADO", "ПОЛУЧЕН"), t.unlocked_at);
            } else {
                item.status_text = t.hidden
                    ? Tr("LOCKED • SECRET", "BLOQUEADO • SECRETO", "ЗАКРЫТ • СЕКРЕТНЫЙ")
                    : Tr("LOCKED", "BLOQUEADO", "ЗАКРЫТ");
            }
            if (!t.icon_path.empty() && fs::exists(t.icon_path)) {
                item.has_icon = true;
                item.icon_src = t.icon_path.string();
            }
            item.unlocked = t.unlocked;
            trophies_.push_back(std::move(item));
        }

        const int pct = total > 0 ? (unlocked_total * 100) / total : 0;
        trophy_progress_pct_ = std::to_string(pct);
        trophy_summary_text_ = fmt::format("{} / {} ({}%)", unlocked_total, total, pct);
        count_p_ = fmt::format("{}/{}", got_p, tot_p);
        count_g_ = fmt::format("{}/{}", got_g, tot_g);
        count_s_ = fmt::format("{}/{}", got_s, tot_s);
        count_b_ = fmt::format("{}/{}", got_b, tot_b);

        ClampSelection();
        for (int i = 0; i < int(trophies_.size()); ++i) {
            trophies_[i].selected = (active_tab_ == "trophies" && i == selected_index_);
        }

        model_.DirtyVariable("trophy_progress_pct");
        model_.DirtyVariable("trophy_summary_text");
        model_.DirtyVariable("count_p");
        model_.DirtyVariable("count_g");
        model_.DirtyVariable("count_s");
        model_.DirtyVariable("count_b");
        model_.DirtyVariable("trophies");
    }

    void RefreshSettings() {
        const auto& s = BbSettings::Get();
        const char* on_str = Tr("ON", "ATIVADO", "ВКЛ");
        const char* off_str = Tr("OFF", "DESATIVADO", "ВЫКЛ");

        settings_.clear();
        auto add = [&](const char* id, const char* label, std::string val) {
            QuickSettingItem item;
            item.id = id;
            item.label = label;
            item.value = std::move(val);
            settings_.push_back(std::move(item));
        };

        add("upscaler", Tr("Upscaler Backend", "Upscaler", "Апскейлер"),
            BbSettings::UpscalerName(s.upscaler.load()));
        add("preset", Tr("Upscaler Quality Preset", "Predefinição de Qualidade", "Пресет качества"),
            BbSettings::PresetName(s.preset.load()));
        add("frame_gen", Tr("Frame Generation", "Geração de Quadros", "Генерация кадров"),
            BbSettings::FrameGenName(s.frame_generation.load()));
        add("aa", Tr("Anti-Aliasing", "Anti-Aliasing", "Сглаживание"),
            BbSettings::AntiAliasingName(s.anti_aliasing.load()));
        add("sharpen", Tr("RCAS Sharpening", "Nitidez (RCAS)", "Резкость (RCAS)"),
            s.sharpen.load() ? on_str : off_str);
        add("vbao", Tr("GT-VBAO Ambient Occlusion", "Oclusão de Ambiente GT-VBAO", "Затенение GT-VBAO"),
            s.vbao.load() ? on_str : off_str);
        add("show_fps", Tr("In-Game FPS Overlay", "Contador de FPS na Tela", "Счётчик FPS"),
            s.show_fps.load() ? on_str : off_str);
        add("trophy_theme", Tr("Trophy Notification Style", "Estilo da Notificação de Troféu", "Стиль уведомления о трофее"),
            TrophyStyleDisplayName(BbOverlay::GetTrophyStyle()));
        add("motion_blur", Tr("Motion Blur", "Desfoque de Movimento", "Размытие в движении"),
            s.effects[BbSettings::EffectMotionBlur].load() ? on_str : off_str);
        add("bloom", Tr("Bloom Lighting", "Iluminação Bloom", "Свечение (Bloom)"),
            s.effects[BbSettings::EffectBloom].load() ? on_str : off_str);
        add("ca", Tr("Chromatic Aberration", "Aberração Cromática", "Хроматическая аберрация"),
            s.effects[BbSettings::EffectChromaticAberration].load() ? on_str : off_str);
        add("dof", Tr("Depth of Field (DoF)", "Profundidade de Campo (DoF)", "Глубина резкости"),
            s.effects[BbSettings::EffectDof].load() ? on_str : off_str);
        add("ssao", Tr("Screen-Space AO (SSAO)", "Oclusão de Ambiente SSAO", "SSAO"),
            s.effects[BbSettings::EffectSsao].load() ? on_str : off_str);
        add("shadows", Tr("Dynamic Shadows", "Sombras Dinâmicas", "Динамические тени"),
            s.effects[BbSettings::EffectDynamicShadows].load() ? on_str : off_str);

        ClampSelection();
        for (int i = 0; i < int(settings_.size()); ++i) {
            settings_[i].selected = (active_tab_ == "settings" && i == selected_index_);
        }
        model_.DirtyVariable("settings");
    }

    void RefreshActions() {
        actions_.clear();
        auto add = [&](const char* id, const char* icon, const char* label, const char* desc) {
            QuickActionItem item;
            item.id = id;
            item.icon = icon;
            item.label = label;
            item.desc = desc;
            actions_.push_back(std::move(item));
        };

        add("resume", "\xEE\x84\xBC",
            Tr("Resume Game", "Voltar ao Jogo", "Вернуться в игру"),
            Tr("Close the PS4 Quick Menu and continue the hunt.",
               "Fecha o Quick Menu e retorna imediatamente à caçada.",
               "Закрыть быстрое меню и продолжить охоту."));
        add("test_trophies", "\xEE\x8D\xB3",
            Tr("Preview Trophy Notifications (4 Tiers)",
               "Testar Notificações de Troféu (4 Cores)",
               "Проверить уведомления о трофеях (4 уровня)"),
            Tr("Triggers Bronze, Silver, Gold, and Platinum pop-ups in the active PS5/PS4 style.",
               "Exibe os pop-ups de Bronze, Prata, Ouro e Platina no estilo PS5 ou PS4 ativo.",
               "Показать уведомления бронзы, серебра, золота и платины в выбранном стиле."));
        add("toggle_trophy_color", "\xEE\x90\x92",
            Tr("Switch Trophy Style (PS5 / PS4 Black / PS4 White)",
               "Alternar Estilo do Troféu (PS5 / PS4 Preto / PS4 Branco)",
               "Сменить стиль трофеев (PS5 / PS4 Чёрный / PS4 Белый)"),
            TrophyStyleDisplayName(BbOverlay::GetTrophyStyle()));
        add("restart", "\xEE\x81\xB0",
            Tr("Restart Game Session", "Reiniciar Sessão do Jogo", "Перезапустить игру"),
            Tr("Applies startup resolution and patch changes via launch planner.",
               "Aplica mudanças de resolução e patches iniciais.",
               "Применить изменения разрешения и патчей запуска."));
        add("quit", "\xEE\x85\xA7",
            Tr("Exit to Desktop", "Sair para a Área de Trabalho", "Выйти на рабочий стол"),
            Tr("Closes Bloodborne cleanly.", "Encerra o jogo imediatamente.", "Закрыть игру."));

        ClampSelection();
        for (int i = 0; i < int(actions_.size()); ++i) {
            actions_[i].selected = (active_tab_ == "system" && i == selected_index_);
        }
        model_.DirtyVariable("actions");
    }

    void RefreshAll() {
        RefreshStrings();
        RefreshTrophies();
        RefreshSettings();
        RefreshActions();
    }

    int CurrentListSize() const {
        if (active_tab_ == "trophies") return int(trophies_.size());
        if (active_tab_ == "settings") return int(settings_.size());
        return int(actions_.size());
    }

    void ClampSelection() {
        const int n = CurrentListSize();
        if (n <= 0) {
            selected_index_ = 0;
        } else if (selected_index_ >= n) {
            selected_index_ = n - 1;
        } else if (selected_index_ < 0) {
            selected_index_ = 0;
        }
    }

    void UpdateSelectionFlags() {
        ClampSelection();
        if (active_tab_ == "trophies") {
            for (int i = 0; i < int(trophies_.size()); ++i) {
                trophies_[i].selected = (i == selected_index_);
            }
            model_.DirtyVariable("trophies");
            ScrollFocusedIntoView("trophy-scroll", selected_index_, int(trophies_.size()), 72.0f);
        } else if (active_tab_ == "settings") {
            for (int i = 0; i < int(settings_.size()); ++i) {
                settings_[i].selected = (i == selected_index_);
            }
            model_.DirtyVariable("settings");
            ScrollFocusedIntoView("settings-scroll", selected_index_, int(settings_.size()), 48.0f);
        } else {
            for (int i = 0; i < int(actions_.size()); ++i) {
                actions_[i].selected = (i == selected_index_);
            }
            model_.DirtyVariable("actions");
        }
    }

    void ScrollFocusedIntoView(const char* container_id, int idx, int total, float row_h) {
        if (!document_ || total <= 0) return;
        if (Rml::Element* el = document_->GetElementById(container_id)) {
            const float view_h = el->GetClientHeight();
            if (view_h <= 0.0f) return;
            const float target_top = float(idx) * row_h;
            const float cur = el->GetScrollTop();
            if (target_top < cur) {
                el->SetScrollTop(target_top);
            } else if (target_top + row_h > cur + view_h) {
                el->SetScrollTop(target_top + row_h - view_h);
            }
        }
    }

    void SwitchTab(const Rml::String& tab) {
        if (active_tab_ == tab) return;
        active_tab_ = tab;
        selected_index_ = 0;
        model_.DirtyVariable("active_tab");
        RefreshAll();
    }

    void StepTab(int delta) {
        static const char* const kTabs[3] = {"trophies", "settings", "system"};
        int cur = active_tab_ == "settings" ? 1 : (active_tab_ == "system" ? 2 : 0);
        cur = (cur + delta + 3) % 3;
        SwitchTab(kTabs[cur]);
    }

    void MoveSelection(int delta) {
        const int n = CurrentListSize();
        if (n <= 0) return;
        selected_index_ = (selected_index_ + delta + n) % n;
        UpdateSelectionFlags();
    }

    void PreviewTrophyById(int id) {
        for (const auto& t : raw_catalog_) {
            if (t.id == id) {
                BbOverlay::ShowTrophy(t.grade, t.name, t.icon_path, 0.0f);
                return;
            }
        }
        BbOverlay::TriggerTrophyPreview('P');
    }

    void CycleSettingById(const Rml::String& id, int dir) {
        auto& s = BbSettings::Get();
        const auto& caps = BbSettings::Caps();

        if (id == "upscaler") {
            std::vector<int> avail;
            if (caps.fsr4_supported.load()) avail.push_back(BbSettings::UpscalerFsr4);
            if (caps.fsr411_supported.load()) avail.push_back(BbSettings::UpscalerFsr411);
            avail.push_back(BbSettings::UpscalerFsr3);
            if (caps.dlss_supported.load()) avail.push_back(BbSettings::UpscalerDlss);
            avail.push_back(BbSettings::UpscalerFsr1);
            avail.push_back(BbSettings::UpscalerTaa);
            avail.push_back(BbSettings::UpscalerOff);
            const int cur = s.upscaler.load();
            auto it = std::find(avail.begin(), avail.end(), cur);
            int idx = it != avail.end() ? int(it - avail.begin()) : 0;
            idx = (idx + dir + int(avail.size())) % int(avail.size());
            s.upscaler = avail[idx];
            BbSettings::SyncAntiAliasing();
        } else if (id == "preset") {
            const int cur = s.preset.load();
            s.preset = (cur + dir + BbSettings::PresetCount) % BbSettings::PresetCount;
        } else if (id == "frame_gen") {
            std::vector<int> avail = {BbSettings::FrameGenOff, BbSettings::FrameGenFsr3};
            if (caps.dlssg_max_multiplier.load() > 1) avail.push_back(BbSettings::FrameGenDlss);
            const int cur = s.frame_generation.load();
            auto it = std::find(avail.begin(), avail.end(), cur);
            int idx = it != avail.end() ? int(it - avail.begin()) : 0;
            idx = (idx + dir + int(avail.size())) % int(avail.size());
            s.frame_generation = avail[idx];
        } else if (id == "aa") {
            static constexpr int kOrder[5] = {
                BbSettings::AaAuto, BbSettings::AaGame, BbSettings::AaFxaa,
                BbSettings::AaSmaa, BbSettings::AaOff};
            const int cur = s.anti_aliasing.load();
            int idx = 0;
            for (int i = 0; i < 5; ++i) if (kOrder[i] == cur) idx = i;
            idx = (idx + dir + 5) % 5;
            s.anti_aliasing = kOrder[idx];
            BbSettings::SyncAntiAliasing();
        } else if (id == "sharpen") {
            s.sharpen = !s.sharpen.load();
        } else if (id == "vbao") {
            s.vbao = !s.vbao.load();
            if (s.vbao.load()) {
                s.effects[BbSettings::EffectSsao] = true;
            }
        } else if (id == "show_fps") {
            s.show_fps = !s.show_fps.load();
        } else if (id == "trophy_theme") {
            StepTrophyStyle(dir);
            return;
        } else if (id == "motion_blur") {
            s.effects[BbSettings::EffectMotionBlur] = !s.effects[BbSettings::EffectMotionBlur].load();
        } else if (id == "bloom") {
            s.effects[BbSettings::EffectBloom] = !s.effects[BbSettings::EffectBloom].load();
        } else if (id == "ca") {
            s.effects[BbSettings::EffectChromaticAberration] = !s.effects[BbSettings::EffectChromaticAberration].load();
        } else if (id == "dof") {
            s.effects[BbSettings::EffectDof] = !s.effects[BbSettings::EffectDof].load();
        } else if (id == "ssao") {
            const bool next = !s.effects[BbSettings::EffectSsao].load();
            s.effects[BbSettings::EffectSsao] = next;
            if (!next) s.vbao = false;
        } else if (id == "shadows") {
            s.effects[BbSettings::EffectDynamicShadows] = !s.effects[BbSettings::EffectDynamicShadows].load();
        }
        BbSettings::Save();
        RefreshSettings();
    }

    void ExecuteActionById(const Rml::String& id) {
        if (id == "resume") {
            SetOpen(false);
        } else if (id == "test_trophies") {
            BbOverlay::TriggerTrophyPreview('B');
            BbOverlay::TriggerTrophyPreview('S');
            BbOverlay::TriggerTrophyPreview('G');
            BbOverlay::TriggerTrophyPreview('P');
            SetOpen(false);
        } else if (id == "toggle_trophy_color") {
            StepTrophyStyle(1);
            BbOverlay::TriggerTrophyPreview('B');
        } else if (id == "restart") {
            SetOpen(false);
            if (runtime_restart) {
                runtime_restart();
            }
        } else if (id == "quit") {
            std::exit(0);
        }
    }

    void ActivateCurrent(int dir = 1) {
        ClampSelection();
        if (active_tab_ == "trophies") {
            if (selected_index_ >= 0 && selected_index_ < int(trophies_.size())) {
                PreviewTrophyById(trophies_[selected_index_].id);
            }
        } else if (active_tab_ == "settings") {
            if (selected_index_ >= 0 && selected_index_ < int(settings_.size())) {
                CycleSettingById(settings_[selected_index_].id, dir);
            }
        } else if (active_tab_ == "system") {
            if (selected_index_ >= 0 && selected_index_ < int(actions_.size())) {
                ExecuteActionById(actions_[selected_index_].id);
            }
        }
    }

    void CycleTrophyFilter(int dir) {
        static const char* const kFilters[3] = {"all", "unlocked", "locked"};
        int cur = filter_mode_ == "unlocked" ? 1 : (filter_mode_ == "locked" ? 2 : 0);
        cur = (cur + dir + 3) % 3;
        filter_mode_ = kFilters[cur];
        selected_index_ = 0;
        RefreshTrophies();
        model_.DirtyVariable("filter_mode");
    }

    void UpdateAndRender(u32 w, u32 h, float dt) {
        if (!context_) return;
        if (w != last_w_ || h != last_h_) {
            last_w_ = w;
            last_h_ = h;
            context_->SetDimensions(Rml::Vector2i(int(w), int(h)));
            const float dp = std::clamp(float(h) / 1080.0f, 0.8f, 2.5f);
            context_->SetDensityIndependentPixelRatio(dp);
        }
        if (g_needs_refresh.exchange(false)) {
            RefreshAll();
        }

        // Update live FPS / resolution pill every 0.25s
        perf_accum_ += dt;
        if (perf_accum_ >= 0.25f) {
            perf_accum_ = 0.0f;
            const auto& tel = BbSettings::Telemetry();
            const float gfps = tel.game_fps.load(std::memory_order_relaxed);
            const int rw = tel.active_render_width.load(std::memory_order_relaxed);
            const int rh = tel.active_render_height.load(std::memory_order_relaxed);
            const int fg = std::max(1, tel.frame_gen_now.load(std::memory_order_relaxed));
            const float shown_fps = gfps * float(fg);
            perf_text_ = shown_fps > 0.5f
                ? fmt::format("{:.0f} FPS • {}x{}", shown_fps, rw, rh)
                : fmt::format("{}x{}", rw, rh);
            model_.DirtyVariable("perf_text");
        }

        context_->Update();
        context_->Render();
    }

    Rml::Context* Context() const { return context_; }
    const Rml::String& ActiveTab() const { return active_tab_; }
    void ToggleSpoilers() {
        reveal_hidden_ = !reveal_hidden_;
        RefreshTrophies();
        model_.DirtyVariable("reveal_hidden");
    }

private:
    Rml::Context* context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    Rml::DataModelHandle model_{};
    u32 last_w_ = 0, last_h_ = 0;
    float perf_accum_ = 1.0f;
    int selected_index_ = 0;

    Rml::String active_tab_ = "trophies";
    Rml::String filter_mode_ = "all";
    bool reveal_hidden_ = false;
    Rml::String header_tag_ = "PLAYSTATION 5 CONTROL CENTER";
    Rml::String trophy_style_badge_ = "Style: PS5";
    Rml::String perf_text_ = "1920x1080";
    Rml::String trophy_summary_text_ = "0 / 40 (0%)";
    Rml::String trophy_progress_pct_ = "0";
    Rml::String count_p_ = "0/1";
    Rml::String count_g_ = "0/7";
    Rml::String count_s_ = "0/8";
    Rml::String count_b_ = "0/24";
    Rml::String tr_tab_trophies_ = "TROPHIES";
    Rml::String tr_tab_settings_ = "GRAPHICS";
    Rml::String tr_tab_system_ = "SYSTEM";
    Rml::String tr_filter_all_ = "All";
    Rml::String tr_filter_unlocked_ = "Unlocked";
    Rml::String tr_filter_locked_ = "Locked";
    Rml::String tr_reveal_hidden_ = "Spoilers";
    Rml::String footer_nav_;
    Rml::String footer_close_;

    std::vector<BbOverlay::TrophyEntryView> raw_catalog_;
    std::vector<QuickTrophyItem> trophies_;
    std::vector<QuickSettingItem> settings_;
    std::vector<QuickActionItem> actions_;
};

std::unique_ptr<QuickMenuSystemInterface> g_sys_interface;
std::unique_ptr<QuickMenuRenderInterface> g_render_interface;
std::unique_ptr<QuickMenuController> g_controller;

struct PendingAction {
    enum Type {
        None,
        MoveUp,
        MoveDown,
        MoveLeft,
        MoveRight,
        TabPrev,
        TabNext,
        Activate,
        ToggleSpoilers,
        MouseMove,
        MouseDown,
        MouseUp,
        MouseWheel,
    } type = None;
    float x = 0.0f, y = 0.0f;
    int button = 0;
};

std::mutex g_event_mutex;
std::vector<PendingAction> g_pending_actions;

void PushAction(PendingAction a) {
    std::scoped_lock lock{g_event_mutex};
    g_pending_actions.push_back(a);
}

} // namespace

bool IsOpen() {
    return g_open.load(std::memory_order_relaxed);
}

void SetOpen(bool open) {
    const bool prev = g_open.exchange(open);
    if (open && !prev) {
        g_needs_refresh.store(true, std::memory_order_relaxed);
    }
}

void Toggle() {
    SetOpen(!IsOpen());
}

bool HandleEvent(const SDL_Event& event) {
    const bool is_open = IsOpen();

    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 btn = event.gbutton.button;
        if (btn == SDL_GAMEPAD_BUTTON_GUIDE) {
            Toggle();
            return true;
        }
        if (!is_open) {
            return false;
        }
        switch (btn) {
        case SDL_GAMEPAD_BUTTON_EAST:
            SetOpen(false);
            return true;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            PushAction({PendingAction::MoveUp});
            return true;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            PushAction({PendingAction::MoveDown});
            return true;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            PushAction({PendingAction::MoveLeft});
            return true;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            PushAction({PendingAction::MoveRight});
            return true;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            PushAction({PendingAction::TabPrev});
            return true;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            PushAction({PendingAction::TabNext});
            return true;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            PushAction({PendingAction::Activate});
            return true;
        case SDL_GAMEPAD_BUTTON_NORTH:
            PushAction({PendingAction::ToggleSpoilers});
            return true;
        default:
            return true;
        }
    }

    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
            return true;
        }
        return is_open;
    }

    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && is_open) {
        // Left stick vertical navigation with hysteresis
        static int stick_state = 0;
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            const int v = event.gaxis.value;
            if (v < -18000 && stick_state >= 0) {
                stick_state = -1;
                PushAction({PendingAction::MoveUp});
            } else if (v > 18000 && stick_state <= 0) {
                stick_state = 1;
                PushAction({PendingAction::MoveDown});
            } else if (v > -8000 && v < 8000) {
                stick_state = 0;
            }
        }
        return true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (!event.key.repeat && (event.key.key == SDLK_F1 || event.key.key == SDLK_HOME)) {
            Toggle();
            return true;
        }
        if (!is_open) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
            SetOpen(false);
            return true;
        case SDLK_UP:
        case SDLK_W:
            PushAction({PendingAction::MoveUp});
            return true;
        case SDLK_DOWN:
        case SDLK_S:
            PushAction({PendingAction::MoveDown});
            return true;
        case SDLK_LEFT:
        case SDLK_A:
            PushAction({PendingAction::MoveLeft});
            return true;
        case SDLK_RIGHT:
        case SDLK_D:
            PushAction({PendingAction::MoveRight});
            return true;
        case SDLK_Q:
        case SDLK_PAGEUP:
            PushAction({PendingAction::TabPrev});
            return true;
        case SDLK_E:
        case SDLK_PAGEDOWN:
        case SDLK_TAB:
            PushAction({PendingAction::TabNext});
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            PushAction({PendingAction::Activate});
            return true;
        default:
            return true;
        }
    }

    if (event.type == SDL_EVENT_KEY_UP) {
        return is_open;
    }

    if (!is_open) {
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        PushAction({PendingAction::MouseMove, event.motion.x, event.motion.y, 0});
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        const int btn = event.button.button == SDL_BUTTON_LEFT    ? 0
                        : event.button.button == SDL_BUTTON_RIGHT ? 1
                                                                  : 2;
        PushAction({event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PendingAction::MouseDown
                                                              : PendingAction::MouseUp,
                    event.button.x, event.button.y, btn});
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        PushAction({PendingAction::MouseWheel, event.wheel.x, -event.wheel.y, 0});
        return true;
    }

    return false;
}

void Render(u32 display_w, u32 display_h, float dt) {
    if (!IsOpen() || display_w == 0 || display_h == 0) {
        return;
    }

    if (!g_rml_initialized) {
        g_sys_interface = std::make_unique<QuickMenuSystemInterface>();
        g_render_interface = std::make_unique<QuickMenuRenderInterface>();
        Rml::SetSystemInterface(g_sys_interface.get());
        Rml::SetRenderInterface(g_render_interface.get());
        g_controller = std::make_unique<QuickMenuController>();
        if (!g_controller->Init(display_w, display_h)) {
            std::fprintf(stderr, "Overlay: RmlUi PS4 Quick Menu initialization failed\n");
            g_open = false;
            return;
        }
        g_rml_initialized = true;
        std::printf("Overlay: PS4 Quick Menu ready (Home/Guide button or F1)\n");
    }

    std::vector<PendingAction> actions;
    {
        std::scoped_lock lock{g_event_mutex};
        actions.swap(g_pending_actions);
    }

    for (const auto& a : actions) {
        switch (a.type) {
        case PendingAction::MoveUp:
            g_controller->MoveSelection(-1);
            break;
        case PendingAction::MoveDown:
            g_controller->MoveSelection(+1);
            break;
        case PendingAction::MoveLeft:
            if (g_controller->ActiveTab() == "trophies") {
                g_controller->CycleTrophyFilter(-1);
            } else {
                g_controller->ActivateCurrent(-1);
            }
            break;
        case PendingAction::MoveRight:
            if (g_controller->ActiveTab() == "trophies") {
                g_controller->CycleTrophyFilter(+1);
            } else {
                g_controller->ActivateCurrent(+1);
            }
            break;
        case PendingAction::TabPrev:
            g_controller->StepTab(-1);
            break;
        case PendingAction::TabNext:
            g_controller->StepTab(+1);
            break;
        case PendingAction::Activate:
            g_controller->ActivateCurrent(+1);
            break;
        case PendingAction::ToggleSpoilers:
            g_controller->ToggleSpoilers();
            break;
        case PendingAction::MouseMove:
            if (auto* ctx = g_controller->Context()) {
                ctx->ProcessMouseMove(int(a.x), int(a.y), 0);
            }
            break;
        case PendingAction::MouseDown:
            if (auto* ctx = g_controller->Context()) {
                ctx->ProcessMouseMove(int(a.x), int(a.y), 0);
                ctx->ProcessMouseButtonDown(a.button, 0);
            }
            break;
        case PendingAction::MouseUp:
            if (auto* ctx = g_controller->Context()) {
                ctx->ProcessMouseMove(int(a.x), int(a.y), 0);
                ctx->ProcessMouseButtonUp(a.button, 0);
            }
            break;
        case PendingAction::MouseWheel:
            if (auto* ctx = g_controller->Context()) {
                ctx->ProcessMouseWheel(Rml::Vector2f(a.x, a.y), 0);
            }
            break;
        default:
            break;
        }
    }

    g_controller->UpdateAndRender(display_w, display_h, dt);
}

} // namespace BbQuickMenu
