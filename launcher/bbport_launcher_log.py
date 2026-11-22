# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Composed GTK page adapter for the launcher's log page: game output, copy and export."""

import time
from pathlib import Path

from bbport_i18n import tr
from bbport_launcher_common import GLib, Gtk
from bbport_launcher_core import MAX_LOG_LINES


class LogPage:
    """Owns the Log view and its copy/export controls inside LauncherWindow."""

    def __init__(self, window, show_toast):
        self.window = window
        self.show_toast = show_toast
        self.log_view = None
        self.log_scroller = None
        self.copy_log_button = None
        self.export_log_button = None

    def build(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.log_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False,
                                     wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.log_view.set_top_margin(8)
        self.log_view.set_left_margin(8)
        self.log_view.set_right_margin(8)
        scroller = Gtk.ScrolledWindow(vexpand=True, child=self.log_view)
        self.log_scroller = scroller
        box.append(scroller)
        bar = Gtk.ActionBar()
        self.export_log_button = Gtk.Button(label=tr("Экспорт журнала…"))
        self.export_log_button.connect("clicked", self.on_export_log)
        bar.pack_end(self.export_log_button)
        self.copy_log_button = Gtk.Button(label=tr("Копировать"))
        self.copy_log_button.connect("clicked", self.on_copy_log)
        bar.pack_end(self.copy_log_button)
        box.append(bar)
        buffer = self.log_view.get_buffer()
        buffer.connect("changed", self.on_log_changed)
        self.on_log_changed(buffer)
        return box

    def on_log_changed(self, buffer):
        has_text = buffer.get_char_count() > 0
        self.copy_log_button.set_sensitive(has_text)
        self.export_log_button.set_sensitive(has_text)

    def text(self):
        if self.log_view is None:
            return ""
        buffer = self.log_view.get_buffer()
        return buffer.get_text(*buffer.get_bounds(), False)

    def set_text(self, value):
        if self.log_view is not None:
            self.log_view.get_buffer().set_text(value)

    def on_copy_log(self, _button):
        self.window.get_clipboard().set(self.text())
        self.show_toast(tr("Журнал скопирован"))

    def on_export_log(self, _button):
        text = self.text()
        dialog = Gtk.FileDialog(title=tr("Экспорт журнала"))
        dialog.set_initial_name(f"bbport-{time.strftime('%Y%m%d_%H%M%S')}.log")

        def finish(dialog, result):
            try:
                chosen = dialog.save_finish(result)
            except GLib.Error:
                return
            if not chosen or not chosen.get_path():
                return
            try:
                Path(chosen.get_path()).write_text(text, encoding="utf-8")
            except OSError as error:
                self.show_toast(tr("Не удалось сохранить журнал: {}").format(error.strerror or error))
                return
            self.show_toast(tr("Журнал сохранён: {}").format(chosen.get_path()))
        dialog.save(self.window, None, finish)

    def append(self, text):
        buffer = self.log_view.get_buffer()
        buffer.insert(buffer.get_end_iter(), text)
        extra = buffer.get_line_count() - MAX_LOG_LINES
        if extra > 0:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(extra)[1])
        adj = self.log_scroller.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper()) and False)
