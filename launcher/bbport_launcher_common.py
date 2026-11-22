# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""GTK4 / libadwaita UI helpers shared by the launcher window and its page adapters.

All non-GTK logic (settings, INI/mod/patch persistence, hardware probes, environment
construction) lives in bbport_launcher_core.py.
"""

from pathlib import Path

from bbport_i18n import tr

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402


def open_url(url):
    """Opens url in the user's browser: GIO, then Python's webbrowser (xdg-open and the like);
    True when one of them took it."""
    try:
        if Gio.AppInfo.launch_default_for_uri(url, None):
            return True
    except GLib.Error:
        pass
    try:
        import webbrowser
        return bool(webbrowser.open(url))
    except Exception:
        return False


def flat_button(icon, tooltip, handler):
    button = Gtk.Button(icon_name=icon, valign=Gtk.Align.CENTER, tooltip_text=tooltip)
    button.add_css_class("flat")
    button.connect("clicked", handler)
    return button


def open_folder(window, path):
    """Opens `path` in the file manager (created first, so that a new saves folder opens)."""
    try:
        Path(path).mkdir(parents=True, exist_ok=True)
    except OSError:
        return
    Gtk.FileLauncher.new(Gio.File.new_for_path(str(path))).launch(window, None, None)


def choose_folder(window, title, current, done):
    """Opens a folder chooser dialog starting at `current`, calling `done(path)` on selection."""
    dialog = Gtk.FileDialog(title=title)
    if Path(current).is_dir():
        dialog.set_initial_folder(Gio.File.new_for_path(str(current)))

    def finish(dialog, result):
        try:
            folder = dialog.select_folder_finish(result)
        except GLib.Error:
            return
        if folder:
            done(folder.get_path())
    dialog.select_folder(window, None, finish)


def show_alert(window, toasts, heading, body):
    """Presents an Adw.AlertDialog when available, falling back to a toast."""
    if hasattr(Adw, "AlertDialog"):
        dialog = Adw.AlertDialog(heading=heading, body=body)
        dialog.add_response("ok", "OK")
        dialog.present(window)
    elif toasts is not None:
        toasts.add_toast(Adw.Toast(title=f"{heading}: {body}", timeout=10))


def whole_label_factory():
    """Choices shown whole: the default ellipsizes the selected one next to a long description."""
    factory = Gtk.SignalListItemFactory()
    factory.connect("setup", lambda _f, item: item.set_child(Gtk.Label(xalign=0)))
    factory.connect("bind", lambda _f, item: item.get_child().set_label(item.get_item().get_string()))
    return factory


def combo_row(title, subtitle, choices, current):
    model = Gtk.StringList.new([tr(choice[0]) for choice in choices])
    row = Adw.ComboRow(title=title, model=model, factory=whole_label_factory())
    values = [choice[1] for choice in choices]
    notes = [tr(choice[2]) if len(choice) > 2 else None for choice in choices]

    def show_note(*_):
        selected = row.get_selected()
        note = row.notes[selected] if selected < len(row.notes) else None
        lines = [line for line in (row.description, note, row.hint) if line]
        row.set_subtitle("\n".join(lines))

    row.description, row.notes, row.hint = subtitle, notes, None
    row.show_note = show_note
    row.set_selected(values.index(current) if current in values else 0)
    show_note()
    row.connect("notify::selected", show_note)
    row.values = values
    return row


def combo_value(row):
    return row.values[row.get_selected()]


class FolderList:
    """A preferences group with a folder row and one switch row per item below it."""

    def __init__(self, group):
        self.group = group
        self.rows = []

    def clear(self):
        for _, row in self.rows:
            self.group.remove(row)
        self.rows = []

    def add(self, key, row):
        self.group.add(row)
        self.rows.append((key, row))
