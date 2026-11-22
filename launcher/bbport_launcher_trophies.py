# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Composed GTK page adapter for the launcher's trophies page."""

import datetime

from bbport_i18n import tr
from bbport_launcher_common import Adw, GLib, Gtk
from bbport_launcher_core import trophy_list


class TrophiesPage:
    """Owns the Trophies preferences page inside LauncherWindow."""

    TROPHY_GRADES = {"P": "Платиновый", "G": "Золотой", "S": "Серебряный", "B": "Бронзовый"}

    def __init__(self, settings, get_user_dir):
        self.settings = settings
        self.get_user_dir = get_user_dir
        self.page = None
        self.trophy_key_row = None
        self.trophy_groups = []

    def build(self):
        """The trophies the port keeps (scripts/trophy_list.py) and the player's trophy key."""
        page = Adw.PreferencesPage(description=tr(
            "Трофеи, полученные в этой папке сохранений. Названия, описания и значки игра берёт из "
            "своего пакета трофеев: для этого нужен ключ трофеев — тот же, что просит shadPS4."))
        key = Adw.PreferencesGroup(title=tr("Ключ трофеев"))
        self.trophy_key_row = Adw.PasswordEntryRow(title=tr("Ключ (32 шестнадцатеричные цифры)"))
        self.trophy_key_row.set_text(self.settings.get("trophy_key", ""))
        self.trophy_key_row.connect("changed", self.on_trophy_key_changed)
        key.add(self.trophy_key_row)
        self.on_trophy_key_changed(self.trophy_key_row)
        page.add(key)
        self.page = page
        self.trophy_groups = []
        self.refresh()
        return page

    def read_into(self, settings):
        settings["trophy_key"] = self.trophy_key_row.get_text().strip()

    def on_trophy_key_changed(self, row):
        text = row.get_text().strip()
        if text and not trophy_list.valid_key(text):
            row.add_css_class("error")
        else:
            row.remove_css_class("error")
        self.settings["trophy_key"] = text

    def refresh(self):
        if self.page is None:
            return
        for group in self.trophy_groups:
            self.page.remove(group)
        self.trophy_groups = []
        content = trophy_list.load(self.get_user_dir(), self.settings.get("language", "1"))
        summary = Adw.PreferencesGroup(title=tr("Прогресс"))
        if content["total"] == 0:
            summary.add(Adw.ActionRow(title=tr("Трофеев пока нет"), subtitle=tr(
                "Они появятся здесь после первого полученного в игре трофея.")))
        else:
            got = Adw.ActionRow(title=tr("Получено {} из {}").format(content["unlocked"], content["total"])
                                if content["named"] else tr("Получено: {}").format(content["unlocked"]))
            if content["named"]:
                bar = Gtk.LevelBar(min_value=0, max_value=content["total"], value=content["unlocked"],
                                   valign=Gtk.Align.CENTER, width_request=160)
                got.add_suffix(bar)
                got.set_subtitle(", ".join(
                    f"{tr(self.TROPHY_GRADES[grade])}: {content['grades'][grade][0]}/{content['grades'][grade][1]}"
                    for grade in "PGSB" if grade in content["grades"]))
            summary.add(got)
            if not content["named"]:
                summary.add(Adw.ActionRow(title=tr("Названия неизвестны"), subtitle=tr(
                    "Укажите ключ трофеев выше и запустите игру: она расшифрует названия и значки.")))
        self.page.add(summary)
        self.trophy_groups.append(summary)
        if content["trophies"]:
            group = Adw.PreferencesGroup(title=tr("Трофеи"))
            for trophy in content["trophies"]:
                group.add(self.trophy_row(trophy, content["named"]))
            self.page.add(group)
            self.trophy_groups.append(group)

    def trophy_row(self, trophy, named):
        secret = trophy["hidden"] and not trophy["unlocked"]
        if not named:
            title = tr("Трофей №{}").format(trophy["id"])
        elif secret:
            title = tr("Скрытый трофей")
        else:
            title = trophy["name"] or tr("Трофей №{}").format(trophy["id"])
        subtitle = tr("Откроется, когда будет получен") if secret else trophy["detail"]
        row = Adw.ActionRow(title=GLib.markup_escape_text(title), subtitle=GLib.markup_escape_text(subtitle))
        if trophy["icon"] and not secret:
            icon = Gtk.Image.new_from_file(str(trophy["icon"]))
        else:
            icon = Gtk.Image(icon_name="starred-symbolic")
        icon.set_pixel_size(48)
        row.add_prefix(icon)
        info = []
        if trophy["grade"] in self.TROPHY_GRADES:
            info.append(tr(self.TROPHY_GRADES[trophy["grade"]]))
        if trophy["unlocked"]:
            info.append(self.trophy_time(trophy["time"]) if trophy["time"] else tr("Получен"))
        label = Gtk.Label(label="\n".join(info), justify=Gtk.Justification.RIGHT, xalign=1)
        label.add_css_class("dim-label")
        row.add_suffix(label)
        if not trophy["unlocked"]:
            row.set_opacity(0.55)
        return row

    @staticmethod
    def trophy_time(stamp):
        """The unlock time (trophies.jsonl, UTC) in local time."""
        try:
            moment = datetime.datetime.fromisoformat(stamp.replace("Z", "+00:00"))
            return moment.astimezone().strftime("%d.%m.%Y %H:%M")
        except ValueError:
            return stamp
