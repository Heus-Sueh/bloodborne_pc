# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""GTK-free launcher core tests: GPU selection, probe caching, and mod/patch profiles."""
from paths import ROOT
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / "launcher"))
import bbport_launcher_core as core

GPUS = ("0\t0x8086\tintegrated\tIntel(R) UHD Graphics 630\t0\n"
        "1\t0x10de\tdiscrete\tNVIDIA GeForce RTX 4060 Laptop GPU\t1\n"
        "2\t0x10005\tcpu\tllvmpipe (LLVM 21.1.8, 256 bits)\t0\n")


class LauncherCoreGpuTests(unittest.TestCase):
    def setUp(self):
        core.clear_gpu_cache()
        self.addCleanup(core.clear_gpu_cache)
        run = mock.patch.object(core.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 0, stdout=GPUS, stderr=""))
        self.run = run.start()
        self.addCleanup(run.stop)
        self.settings = dict(core.DEFAULTS, game_dir="/games/CUSA03173", user_dir="")

    def test_core_does_not_import_gtk(self):
        self.assertNotIn("gi", sys.modules)

    def test_gpus_listed_in_vulkans_order(self):
        gpus = core.connected_gpus()
        self.assertEqual([(index, name, best) for index, _vendor, _kind, name, best in gpus],
                         [(0, "Intel(R) UHD Graphics 630", False),
                          (1, "NVIDIA GeForce RTX 4060 Laptop GPU", True),
                          (2, "llvmpipe (LLVM 21.1.8, 256 bits)", False)])
        self.assertEqual(self.run.call_args.args[0][1:], ["--gpus"])

    def test_gpu_probe_is_cached_until_forced_refresh(self):
        first = core.connected_gpus()
        second = core.connected_gpus()
        self.assertEqual(first, second)
        self.assertEqual(self.run.call_count, 1)

        core.connected_gpus(force_refresh=True)
        self.assertEqual(self.run.call_count, 2)

    def test_chosen_gpu_passed_by_index(self):
        self.settings["gpu"] = "Intel(R) UHD Graphics 630"
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("BB_GPU_ID", None)
            self.assertEqual(core.game_environment(self.settings).get("BB_GPU_ID"), "0")

    def test_auto_and_missing_gpu_leave_the_choice_to_the_port(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("BB_GPU_ID", None)
            self.assertNotIn("BB_GPU_ID", core.game_environment(self.settings))
            self.settings["gpu"] = "AMD Radeon RX 580"
            self.assertNotIn("BB_GPU_ID", core.game_environment(self.settings))

    def test_shell_gpu_id_wins(self):
        self.settings["gpu"] = "Intel(R) UHD Graphics 630"
        with mock.patch.dict(os.environ, {"BB_GPU_ID": "1"}):
            self.assertEqual(core.game_environment(self.settings)["BB_GPU_ID"], "1")

    def test_online_disables_skip_network_choice(self):
        self.settings["online"] = True
        self.settings["skip_network_choice"] = True
        with mock.patch.object(core, "online_module", return_value=True):
            env = core.game_environment(self.settings)
        self.assertEqual(env["BB_ONLINE"], "1")
        self.assertEqual(env["BB_SKIP_NETWORK_CHOICE"], "0")


class LauncherCoreProfileTests(unittest.TestCase):
    def test_mod_profile_round_trip_and_discovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mods_folder = root / "mods"
            data_dir = root / "data"
            (mods_folder / "Alpha" / "chr").mkdir(parents=True)
            (mods_folder / "Beta" / "parts").mkdir(parents=True)
            (mods_folder / "Gamma" / "dvdroot_ps4" / "param").mkdir(parents=True)

            core.save_mod_profile([("Gamma", True), ("Alpha", False)], data_dir=data_dir)
            loaded = core.load_mod_profile(mods_folder, data_dir=data_dir)
            self.assertEqual(loaded, [("Gamma", True), ("Alpha", False), ("Beta", True)])

    def test_patch_profile_stores_only_deltas_and_preserves_other_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data_dir = root / "data"
            data_dir.mkdir()
            (data_dir / "patches.json").write_text(
                json.dumps({"enabled": ["other.xml/KeepMe"], "disabled": ["other.xml/SkipMe"]}),
                encoding="utf-8",
            )
            patches_folder = root / "patches"
            patches_folder.mkdir()
            (patches_folder / "pack.xml").write_text(
                '<?xml version="1.0"?>\n'
                '<Patch>\n'
                '  <Metadata Title="Bloodborne" Name="DefaultOff" AppVer="01.09" isEnabled="false">\n'
                '    <PatchList><Line Type="bytes" Address="0x1000" Value="90"/></PatchList>\n'
                '  </Metadata>\n'
                '  <Metadata Title="Bloodborne" Name="DefaultOn" AppVer="01.09" isEnabled="true">\n'
                '    <PatchList><Line Type="bytes" Address="0x2000" Value="90"/></PatchList>\n'
                '  </Metadata>\n'
                '</Patch>\n',
                encoding="utf-8",
            )

            # Flip both patches away from their XML defaults.
            core.save_patch_profile([
                ("pack.xml/DefaultOff", True, False),
                ("pack.xml/DefaultOn", False, True),
            ], data_dir=data_dir)

            saved = json.loads((data_dir / "patches.json").read_text(encoding="utf-8"))
            self.assertEqual(saved["enabled"], ["other.xml/KeepMe", "pack.xml/DefaultOff"])
            self.assertEqual(saved["disabled"], ["other.xml/SkipMe", "pack.xml/DefaultOn"])

            loaded = core.load_patch_profile(patches_folder, data_dir=data_dir)
            self.assertEqual(
                [(key, default, active) for key, _meta, default, active in loaded],
                [("pack.xml/DefaultOff", False, True), ("pack.xml/DefaultOn", True, False)],
            )


if __name__ == "__main__":
    unittest.main()
