# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Composed GTK adapter for the launcher's upscaler libraries: FSR 4.1.1/4.1.2 builds and NVIDIA DLSS."""

import os
import re
import shutil
import signal
from pathlib import Path

from bbport_i18n import tr
from bbport_launcher_common import Adw, Gio, GLib, Gtk, flat_button
from bbport_launcher_core import (
    DLSS_SDK_URL,
    FSR411_BUILD_ERRORS,
    FSR4CAP_DIR,
    NGX_FG_PREFIX,
    NGX_PREFIX,
    PORT_DIR,
    binaries_dir,
    fsr411_build_command,
    ngx_libraries,
    ngx_prefix_of,
)


class UpscalerPanel:
    """Owns the AMD/NVIDIA libraries group, FSR 4.1.x build process, and DLSS install flow."""

    def __init__(self, window, settings, get_user_dir, append_log, show_toast, show_alert,
                 on_build_state_changed, on_upscaler_changed, on_fsr411_ready):
        self.window = window
        self.settings = settings
        self.get_user_dir = get_user_dir
        self.append_log = append_log
        self.show_toast = show_toast
        self.show_alert = show_alert
        self.on_build_state_changed = on_build_state_changed
        self.on_upscaler_changed = on_upscaler_changed
        self.on_fsr411_ready = on_fsr411_ready
        self.build_process = None
        self.build_cancelled = False
        self.build_version = "4.1.1"
        self.fsr411_row = None
        self.fsr411_button = None
        self.fsr412_row = None
        self.fsr412_button = None
        self.dlss_row = None
        self.dlss_reset_button = None

    @property
    def is_building(self):
        return self.build_process is not None

    def set_game_running(self, running):
        if self.fsr411_button is not None:
            self.fsr411_button.set_sensitive(not running)
        if self.fsr412_button is not None:
            self.fsr412_button.set_sensitive(not running)

    def build_group(self):
        libraries = Adw.PreferencesGroup(title=tr("Библиотеки AMD и NVIDIA"), description=tr(
            "Свои файлы AMD и NVIDIA: порт их не распространяет"))
        # FSR 4.1.1 is built from the user's own AMD DLL (nothing of AMD's is shipped).
        self.fsr411_row = Adw.ActionRow(title=tr("FSR 4.1.x из своей DLL AMD"))
        self.fsr411_button = Gtk.Button(valign=Gtk.Align.CENTER)
        self.fsr411_button.connect("clicked", self.on_fsr411_button)
        self.fsr411_row.add_suffix(self.fsr411_button)
        self.fsr411_row.set_visible((FSR4CAP_DIR / "build_assets.sh").is_file())
        libraries.add(self.fsr411_row)
        # FSR 4.1.2's models (INT8 RDNA2, INT8 Lite) from FSR SDK 2.4 or AMD's driver.
        self.fsr412_row = Adw.ActionRow(title=tr("Модели FSR 4.1.2 (INT8 RDNA2, INT8 Lite)"))
        self.fsr412_button = Gtk.Button(valign=Gtk.Align.CENTER, label=tr("Выбрать файл…"))
        self.fsr412_button.connect("clicked", self.on_fsr412_button)
        self.fsr412_row.add_suffix(self.fsr412_button)
        self.fsr412_row.set_subtitle(tr(
            "Одна amd_fidelityfx_upscaler_dx12.dll из AMD FSR SDK 2.4 или DLL 4.1.x вместе с драйверным "
            "amdxcffx64.dll (Adrenalin 26.10.1+, или его установщик .exe); ~4 минуты. Модель выбирается "
            "в меню игры"))
        self.fsr412_row.set_visible((FSR4CAP_DIR / "driver_model.py").is_file())
        libraries.add(self.fsr412_row)
        self.update_fsr411_row()
        # DLSS: NVIDIA's library, chosen like the FSR 4.1.1 DLL (the package may bring one).
        self.dlss_row = Adw.ActionRow(title=tr("DLSS: библиотека NVIDIA"))
        self.dlss_row.add_suffix(flat_button("document-open-symbolic", tr("Выбрать файл…"),
                                             lambda _b: self.choose_ngx()))
        self.dlss_reset_button = flat_button("edit-undo-symbolic", tr("Убрать выбранную"),
                                             lambda _b: self.reset_ngx())
        self.dlss_row.add_suffix(self.dlss_reset_button)
        libraries.add(self.dlss_row)
        self.update_dlss_row(notify=False)
        return libraries

    def update_fsr411_row(self, progress=None):
        building = self.build_process is not None
        self.fsr411_button.set_label(tr("Отменить") if building else tr("Выбрать DLL…"))
        self.fsr412_button.set_label(tr("Отменить") if building else tr("Выбрать файл…"))
        if progress:
            self.fsr411_row.set_subtitle(progress)
        elif not building:
            self.fsr411_row.set_subtitle(tr(
                "Собрать FSR 4.1.1 из amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: папка "
                "FSR4_LATEST, или из игры с FSR 4.1). Нужен GE-Proton 10+, Proton Experimental или "
                "Proton-CachyOS; 2–5 минут (RDNA4: вдвое дольше, ещё и вариант FP8)"))

    def update_dlss_row(self, notify=True):
        own = ngx_libraries(self.get_user_dir() / "dlss")
        bundled = ngx_libraries(binaries_dir())
        own_fg = ngx_libraries(self.get_user_dir() / "dlss", NGX_FG_PREFIX)
        bundled_fg = ngx_libraries(binaries_dir(), NGX_FG_PREFIX)
        bridge = (binaries_dir() / "libbbport_dlss.so").is_file()
        if own:
            text = tr("Своя: {}").format(own[-1].name[len(NGX_PREFIX):])
        elif bundled:
            text = tr("Из сборки: {}").format(bundled[-1].name[len(NGX_PREFIX):])
        else:
            text = tr("Нет: выберите libnvidia-ngx-dlss.so.* из DLSS SDK NVIDIA ({})").format(DLSS_SDK_URL)
        # The Frame Generation library (the same SDK folder): the game searches both places.
        if own_fg:
            text += "\n" + tr("Генерация кадров: своя {}").format(own_fg[-1].name[len(NGX_FG_PREFIX):])
        elif bundled_fg:
            text += "\n" + tr("Генерация кадров: из сборки {}").format(bundled_fg[-1].name[len(NGX_FG_PREFIX):])
        else:
            text += "\n" + tr("Генерация кадров: нет libnvidia-ngx-dlssg.so.* (выберите её так же; DLSS SDK 310.7 или новее)")
        if not bridge:
            text += "\n" + tr("Эта сборка без моста DLSS (libbbport_dlss.so): DLSS работать не будет")
        self.dlss_row.set_subtitle(text)
        self.dlss_reset_button.set_sensitive(bool(own or own_fg))
        if notify:
            self.on_upscaler_changed()

    def choose_ngx(self):
        dialog = Gtk.FileDialog(title=tr("Библиотека DLSS NVIDIA (libnvidia-ngx-dlss.so.*, libnvidia-ngx-dlssg.so.*)"))
        files = Gtk.FileFilter()
        files.set_name("libnvidia-ngx-dlss*.so.*, nvngx_dlss*.dll")
        for pattern in ("libnvidia-ngx-dlss.so*", "libnvidia-ngx-dlssg.so*", "*.dll", "*.DLL"):
            files.add_pattern(pattern)
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(files)
        dialog.set_filters(filters)
        dialog.set_default_filter(files)

        def finish(dialog, result):
            try:
                chosen = dialog.open_finish(result)
            except GLib.Error:
                return
            if chosen and chosen.get_path():
                self.install_ngx(Path(chosen.get_path()))
        dialog.open(self.window, None, finish)

    def install_ngx(self, path):
        if path.suffix.lower() == ".dll":
            self.show_alert(tr("Нужна библиотека для Linux"), tr(
                "{} — библиотека DLSS для Windows; на Linux NVIDIA её не загружает. Нужна "
                "libnvidia-ngx-dlss.so.<версия> из DLSS SDK NVIDIA: {}").format(path.name, DLSS_SDK_URL))
            return
        try:
            with open(path, "rb") as f:
                elf = f.read(4) == b"\x7fELF"
        except OSError as error:
            self.show_alert(tr("Не удалось прочитать файл"), str(error))
            return
        prefix = ngx_prefix_of(path.name)
        if prefix is None or not elf:
            self.show_alert(tr("Это не библиотека DLSS"), tr(
                "Нужен файл libnvidia-ngx-dlss.so.<версия> (DLSS) или libnvidia-ngx-dlssg.so.<версия> "
                "(генерация кадров) из DLSS SDK NVIDIA: {}").format(DLSS_SDK_URL))
            return
        target = self.get_user_dir() / "dlss"
        try:
            target.mkdir(parents=True, exist_ok=True)
            for old in ngx_libraries(target, prefix):  # one of each kind
                old.unlink()
            shutil.copy2(path, target / path.name)
        except OSError as error:
            self.show_alert(tr("Не удалось скопировать библиотеку"), str(error))
            return
        version = path.name[len(prefix):]
        self.show_toast((
            tr("DLSS Frame Generation {}: готово") if prefix == NGX_FG_PREFIX else tr("DLSS {}: готово")
        ).format(version))
        self.update_dlss_row()

    def reset_ngx(self):
        for prefix in (NGX_PREFIX, NGX_FG_PREFIX):
            for old in ngx_libraries(self.get_user_dir() / "dlss", prefix):
                try:
                    old.unlink()
                except OSError:
                    pass
        self.update_dlss_row()

    def choose_dll(self, title, done):
        dialog = Gtk.FileDialog(title=title)
        dll = Gtk.FileFilter()
        dll.set_name("DLL")
        dll.add_pattern("*.dll")
        dll.add_pattern("*.DLL")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(dll)
        dialog.set_filters(filters)
        dialog.set_default_filter(dll)
        folder = self.settings.get("fsr411_dll_dir", "")
        if folder and Path(folder).is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(folder))

        def finish(dialog, result):
            try:
                chosen = dialog.open_finish(result)
            except GLib.Error:
                return
            if chosen and chosen.get_path():
                done(chosen.get_path())
        dialog.open(self.window, None, finish)

    def on_fsr411_button(self, _button):
        if self.build_process:
            self.stop_build()
            return
        self.choose_dll(tr("DLL апскейлера AMD (amd_fidelityfx_upscaler_dx12.dll)"),
                        self.on_fsr411_dll)

    def on_fsr412_button(self, _button):
        if self.build_process:
            self.stop_build()
            return
        dialog = Gtk.FileDialog(title=tr("DLL AMD FSR SDK 2.4 (amd_fidelityfx_upscaler_dx12.dll 4.1.2), установщик Adrenalin (.exe) или amdxcffx64.dll"))
        files = Gtk.FileFilter()
        files.set_name("EXE, DLL")
        for pattern in ("*.exe", "*.EXE", "*.dll", "*.DLL"):
            files.add_pattern(pattern)
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(files)
        dialog.set_filters(filters)
        dialog.set_default_filter(files)

        def finish(dialog, result):
            try:
                chosen = dialog.open_finish(result)
            except GLib.Error:
                return
            if chosen and chosen.get_path():
                self.on_fsr412_driver(chosen.get_path())
        dialog.open(self.window, None, finish)

    def on_fsr412_driver(self, driver):
        """FSR 4.1.2's models: AMD's FSR SDK 2.4 upscaler DLL holds them itself (built from it
        alone); the driver's FSR 4 module answers through AMD's upscaler DLL: the one FSR 4.1.1 was
        built from, else chosen now."""
        if Path(driver).name.lower() == "amd_fidelityfx_upscaler_dx12.dll":
            # build_assets.sh's driver form reads its second file as the upscaler DLL (not a
            # loader); with BB_FSR412=1 the first is this DLL too: it is both, holding the models.
            self.start_build(driver, driver, version="4.1.2", fsr412=True)
            return
        upscaler = self.settings.get("fsr411_dll", "")
        if upscaler and Path(upscaler).is_file():
            self.start_build(driver, upscaler, version="4.1.2")
            return
        self.show_toast(tr("Ещё нужна DLL FSR 4.1.x: выберите amd_fidelityfx_upscaler_dx12.dll"), timeout=8)
        self.choose_dll(tr("DLL апскейлера AMD (amd_fidelityfx_upscaler_dx12.dll)"),
                        lambda path: self.start_build(driver, path, version="4.1.2"))

    def on_fsr411_dll(self, upscaler, loader=None):
        import dll_info
        self.settings["fsr411_dll_dir"] = str(Path(upscaler).parent)
        self.settings["fsr411_dll"] = str(upscaler)
        status, _problem, details = dll_info.inspect(upscaler, loader)
        kind = details.get("kind")
        version = details.get("version", "?")
        if status == dll_info.UNSUPPORTED:
            if kind == "fsr4_0":
                body = tr(
                    "{}: FSR {}. Это официальная FSR 4.0.x от AMD: она включается только на "
                    "видеокартах RDNA4 (RX 9000), а под Proton на других видеокартах не "
                    "запускается, поэтому записать её нельзя.\n\nНужна FSR 4.1.x: например, "
                    "FSR4_LATEST из OptiScaler или DLL из игры с FSR 4.1.").format(
                        Path(upscaler).name, version)
            elif kind == "not_fsr4":
                body = tr("{}: в этой DLL нет FSR 4. Нужна amd_fidelityfx_upscaler_dx12.dll "
                          "версии 4.1.x.").format(Path(upscaler).name)
            else:
                models = ", ".join(details.get("models", [])) or tr("нет")
                body = tr(
                    "{}: версия {}, модели FSR 4: {}.\n\nbbport повторяет только официальную FSR 4.1.x "
                    "от AMD (модель v07_fp8_no_scale): например, FSR4_LATEST из OptiScaler или "
                    "amd_fidelityfx_upscaler_dx12.dll из игры с FSR 4.1. Сборки сообщества (4.0.2b и "
                    "подобные) устроены иначе и пока не поддерживаются.").format(
                        Path(upscaler).name, version, models)
            self.show_alert(tr("Эта DLL не подходит"), body)
            return
        if status == dll_info.NO_LOADER:
            if kind == "loader_is_upscaler":
                self.show_alert(tr("Это не загрузчик"), tr(
                    "{} — это DLL апскейлера. Загрузчик называется amd_fidelityfx_loader_dx12.dll "
                    "(у OptiScaler — amd_fidelityfx_dx12.dll) и лежит в той же папке игры, что и "
                    "DLL апскейлера.").format(Path(loader).name))
                return
            if loader:
                self.show_alert(tr("Загрузчик не подходит"), tr(
                    "{}: версия {}. Нужен загрузчик FidelityFX 2.x — из той же игры, что и DLL "
                    "апскейлера.").format(Path(loader).name, details.get("loader_version", "?")))
                return
            # A DLL without the FidelityFX API exports (AMD's own export it: no loader needed), and no
            # loader next to it or one folder up: ask for it.
            self.show_toast(tr(
                "Рядом с DLL нет загрузчика: выберите amd_fidelityfx_loader_dx12.dll из папки игры"),
                timeout=8)
            self.choose_dll(tr("Загрузчик FidelityFX (amd_fidelityfx_loader_dx12.dll или "
                               "amd_fidelityfx_dx12.dll)"),
                            lambda path: self.on_fsr411_dll(upscaler, path))
            return
        self.start_build(upscaler, details.get("loader"))

    def fsr_text(self, text):
        """An FSR 4.1.1 build message, translated, with the version of this build (4.1.2 models).
        The five build messages' translations must keep the literal "4.1.1" for this."""
        return tr(text).replace("4.1.1", self.build_version)

    def start_build(self, upscaler, loader, version="4.1.1", fsr412=False):
        """version: what the log and toasts call the build (4.1.2: its models from SDK 2.4 or the
        driver); fsr412: the upscaler DLL itself holds FSR 4.1.2's models (BB_FSR412=1)."""
        command, env = fsr411_build_command(upscaler, loader)
        if fsr412:
            env["BB_FSR412"] = "1"
        self.build_version = version
        launcher = Gio.SubprocessLauncher.new(
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in env.items()])
        launcher.set_cwd(str(PORT_DIR))
        try:
            # setsid: Proton and its helpers form one process group, cancelled together.
            self.build_process = launcher.spawnv(["setsid", *command])
        except GLib.Error as error:
            self.show_toast(tr("Не удалось запустить: {}").format(error.message))
            return
        self.append_log(self.fsr_text("\n— сборка FSR 4.1.1 из {} —\n").format(upscaler))
        stream = Gio.DataInputStream.new(self.build_process.get_stdout_pipe())
        stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_build_line)
        self.build_process.wait_async(None, self.on_built)
        self.update_fsr411_row(tr("Подготовка…"))
        self.on_build_state_changed()

    def on_build_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None:
            return
        self.append_log(line + "\n")
        if self.build_process:
            step = re.match(r"\[\s*(\d+)/(\d+)\]", line)
            if step:
                self.update_fsr411_row(tr("Запись проходов DLL под Proton: {} из {}").format(*step.groups()))
            elif line.startswith("Translating"):
                self.update_fsr411_row(tr("Перевод проходов в SPIR-V…"))
        stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_build_line)

    def stop_build(self):
        if not self.build_process:
            return
        pid = int(self.build_process.get_identifier())
        self.build_cancelled = True
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.build_process.force_exit()

    def on_built(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        cancelled = self.build_cancelled
        self.build_cancelled = False
        self.build_process = None
        self.update_fsr411_row()
        self.on_build_state_changed()
        self.append_log(self.fsr_text("— сборка FSR 4.1.1 завершилась (код {}) —\n").format(status))
        if status == 0:
            self.on_fsr411_ready()
            self.show_toast(self.fsr_text("FSR 4.1.1 собран и выбран"))
        elif cancelled:
            self.show_toast(self.fsr_text("Сборка FSR 4.1.1 отменена"))
        else:
            message = FSR411_BUILD_ERRORS.get(status, "Сборка не удалась (код {}): подробности в журнале")
            self.show_alert(self.fsr_text("FSR 4.1.1 не собран"), tr(message).format(status))
