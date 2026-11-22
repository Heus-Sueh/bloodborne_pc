# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Composed GTK page adapter for the launcher's mods and external patches lists."""

from bbport_i18n import tr
from bbport_launcher_common import Adw, FolderList, GLib, choose_folder, flat_button, open_folder
from bbport_launcher_core import (
    load_mod_profile,
    load_patch_profile,
    mods_dir,
    patches_dir,
    save_mod_profile,
    save_patch_profile,
)


class ContentPage:
    """Owns the Mods and External Patches preference groups inside LauncherWindow."""

    def __init__(self, window, settings, on_store):
        self.window = window
        self.settings = settings
        self.on_store = on_store
        self.mods_enabled_row = None
        self.mods_folder_row = None
        self.mod_list = None
        self.patches_folder_row = None
        self.patch_list = None

    def build_groups(self):
        mods_group = Adw.PreferencesGroup(
            title=tr("Моды"),
            description=tr(
                "Распакуйте каждый мод в отдельную папку (с dvdroot_ps4 или сразу с chr/, parts/ и т. п.). "
                "При совпадении файлов побеждает мод ниже в списке. Применяется при запуске."))
        self.mods_enabled_row = Adw.SwitchRow(title=tr("Загружать моды"),
                                              active=self.settings["mods_enabled"])
        mods_group.add(self.mods_enabled_row)
        self.mods_folder_row = Adw.ActionRow(title=tr("Папка модов"))
        self.mods_folder_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку модов"),
                                                    self.on_choose_mods))
        self.mods_folder_row.add_suffix(flat_button("system-file-manager-symbolic", tr("Открыть папку модов"),
                                                    lambda _b: open_folder(self.window, mods_dir(self.settings))))
        self.mods_folder_row.add_suffix(flat_button("view-refresh-symbolic", tr("Обновить список"),
                                                    self.on_refresh_mods))
        mods_group.add(self.mods_folder_row)
        self.mod_list = FolderList(mods_group)
        self.refresh_mods()

        patches_group = Adw.PreferencesGroup(
            title=tr("Сторонние патчи"),
            description=tr("XML-патчи в формате shadPS4 для версии 01.09 из папки патчей. "
                           "Применяются при запуске."))
        self.patches_folder_row = Adw.ActionRow(title=tr("Папка патчей"))
        self.patches_folder_row.add_suffix(flat_button("folder-open-symbolic", tr("Выбрать папку патчей"),
                                                       self.on_choose_patches))
        self.patches_folder_row.add_suffix(flat_button(
            "system-file-manager-symbolic", tr("Открыть папку патчей"),
            lambda _b: open_folder(self.window, patches_dir(self.settings))))
        self.patches_folder_row.add_suffix(flat_button("view-refresh-symbolic", tr("Обновить список"),
                                                       self.on_refresh_patches))
        patches_group.add(self.patches_folder_row)
        self.patch_list = FolderList(patches_group)
        self.refresh_patches()
        return mods_group, patches_group

    def described_rows(self):
        return {"mods_enabled": self.mods_enabled_row}

    def read_into(self, settings):
        settings["mods_enabled"] = self.mods_enabled_row.get_active()
        self.save_profiles()

    def save_profiles(self):
        self.save_mod_profile()
        self.save_patch_profile()

    # --- mods ----------------------------------------------------------------------------

    def save_mod_profile(self):
        save_mod_profile([(name, row.get_active()) for name, row in self.mod_list.rows])

    def refresh_mods(self):
        self.mod_list.clear()
        directory = mods_dir(self.settings)
        self.mods_folder_row.set_subtitle(str(directory))
        entries = load_mod_profile(directory)
        for name, enabled in entries:
            row = Adw.SwitchRow(title=name, active=enabled)
            row.add_suffix(flat_button("go-up-symbolic", tr("Загрузить раньше"),
                                       lambda _b, n=name: self.move_mod(n, -1)))
            row.add_suffix(flat_button("go-down-symbolic", tr("Загрузить позже"),
                                       lambda _b, n=name: self.move_mod(n, 1)))
            self.mod_list.add(name, row)
        if not entries:
            self.mods_folder_row.set_subtitle(f"{directory} — {tr('Модов нет')}")

    def move_mod(self, name, direction):
        rows = self.mod_list.rows
        index = next(i for i, (n, _) in enumerate(rows) if n == name)
        target = index + direction
        if 0 <= target < len(rows):
            rows[index], rows[target] = rows[target], rows[index]
            self.save_mod_profile()
            self.refresh_mods()

    def on_refresh_mods(self, _button):
        self.save_mod_profile()
        self.refresh_mods()

    def on_choose_mods(self, _button):
        self.save_mod_profile()

        def chosen(path):
            self.settings["mods_dir"] = path
            self.refresh_mods()
            self.on_store()
        choose_folder(self.window, tr("Папка модов"), mods_dir(self.settings), chosen)

    # --- external patches ----------------------------------------------------------------

    def save_patch_profile(self):
        save_patch_profile([(key, row.get_active(), row.default) for key, row in self.patch_list.rows])

    def refresh_patches(self):
        self.patch_list.clear()
        directory = patches_dir(self.settings)
        entries = load_patch_profile(directory)
        for key, meta, default, active in entries:
            subtitle = key.split("/", 1)[0]
            if meta.get("Author"):
                subtitle += " · " + tr("Автор: {}").format(meta.get("Author"))
            row = Adw.SwitchRow(title=GLib.markup_escape_text(meta.get("Name") or key),
                                subtitle=GLib.markup_escape_text(subtitle), active=active)
            if meta.get("Note"):
                row.set_tooltip_text(meta.get("Note").replace("\\n", "\n"))
            row.default = default
            self.patch_list.add(key, row)
        self.patches_folder_row.set_subtitle(
            str(directory) if entries else f"{directory} — {tr('Патчей нет')}")

    def on_refresh_patches(self, _button):
        self.save_patch_profile()
        self.refresh_patches()

    def on_choose_patches(self, _button):
        self.save_patch_profile()

        def chosen(path):
            self.settings["patches_dir"] = path
            self.refresh_patches()
            self.on_store()
        choose_folder(self.window, tr("Папка патчей"), patches_dir(self.settings), chosen)
