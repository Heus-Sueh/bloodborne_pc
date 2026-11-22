# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Direct interface tests for scripts/launch_plan.py and run.py's Windows restart seam."""
from paths import ROOT
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'scripts'))

import launch_plan
import run as win_run


def _seed_minimal_game(root: Path) -> tuple[Path, Path]:
    out = root / 'out'
    out.mkdir(parents=True, exist_ok=True)
    elf = bytearray(120)
    struct.pack_into('<Q', elf, 0x20, 64)
    struct.pack_into('<HH', elf, 0x36, 56, 1)
    struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0, 0, 0, 0x6000000, 0)
    (out / 'eboot.elf').write_bytes(elf)
    (root / 'eboot.bin').touch()
    return root, out


class LaunchPlannerTests(unittest.TestCase):
    def test_auto_monitor_resolution_and_large_scene_memory(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            _seed_minimal_game(data)
            config = data / 'bbport.ini'
            config.write_text('upscaler=fsr3\npreset=0\noutput_res=auto\n', encoding='utf-8')

            plan = launch_plan.plan_and_prepare(
                {'BB_GAME_DIR': str(data), 'BB_CONFIG': str(config), 'BB_DISPLAY': 'DP-2'},
                port_dir=ROOT,
                data_dir=data,
                skip_prepare=True,
                skip_online=True,
                query_displays=lambda _caps, _env: ['DP-1\t1920x1080\t1', 'DP-2\t3840x2160\t0'],
                log=lambda _m: None,
            )

            self.assertEqual(plan.env_set['BB_MONITOR_SIZE'], '3840x2160')
            self.assertEqual(plan.env_set['BB_OUTPUT_RES'], '3840x2160')
            self.assertEqual(plan.env_set['BB_RENDER_RES'], '3840x2160')
            self.assertEqual(plan.env_set['BB_AUTO_RENDER_RES'], '1')
            self.assertEqual(plan.env_set['BB_LIVE_HYBRID'], '1')
            self.assertEqual(plan.env_set['BB_DMEM_MB'], '9152')
            self.assertTrue((data / 'out' / 'patches.bin').is_file())

    def test_thd_online_unsets_shadnet_without_sed(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            _seed_minimal_game(data)
            config = data / 'bbport.ini'
            config.write_text('upscaler=fsr3\npreset=1\noutput_res=1920x1080\n', encoding='utf-8')

            plan = launch_plan.plan_and_prepare(
                {
                    'BB_GAME_DIR': str(data),
                    'BB_CONFIG': str(config),
                    'BB_ONLINE': '1',
                    'BB_NET_BACKEND': 'thd',
                    'BB_BBSC': '1',
                    'BB_THD_TOKEN': 'tok',
                    'BB_THD_ONLINE_ID': 'Hunter',
                    'BB_COMMUNITY_SERVER': 'https://thehuntersdream.com',
                    'BB_SHADNET_SERVER': 'old:31313',
                    'BB_SHADNET_PASSWORD': 'secret',
                },
                port_dir=ROOT,
                data_dir=data,
                skip_prepare=True,
                log=lambda _m: None,
            )

            self.assertEqual(plan.env_set['BB_THD_SERVER'], 'https://thehuntersdream.com:18671')
            self.assertEqual(plan.env_set['BB_THD_AUTH'], 'https://thehuntersdream.com')
            self.assertTrue(Path(plan.env_set['SHADPS4_HTTP_HOST_OVERRIDES_JSON']).is_file())
            self.assertIn('BB_BBSC', plan.env_unset)
            self.assertIn('BB_SHADNET_SERVER', plan.env_unset)
            self.assertIn('BB_SHADNET_PASSWORD', plan.env_unset)

    def test_windows_run_recomputes_plan_on_restart_exit_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp)
            _seed_minimal_game(data)
            config = data / 'bbport.ini'
            config.write_text('upscaler=fsr3\npreset=1\noutput_res=1280x720\n', encoding='utf-8')

            seen = []

            def fake_probe(_args, env, _cwd, _on_line):
                seen.append({
                    'render': env.get('BB_RENDER_RES'),
                    'output': env.get('BB_OUTPUT_RES'),
                    'restart_by_exit': env.get('BB_RESTART_BY_EXIT'),
                })
                if len(seen) == 1:
                    # Simulate the in-game menu changing preset/resolution before restarting.
                    config.write_text('upscaler=fsr3\npreset=4\noutput_res=1280x720\n', encoding='utf-8')
                    return launch_plan.RESTART_EXIT_CODE
                return 0

            code = win_run.run(
                env={
                    'BB_DATA_DIR': str(data),
                    'BB_GAME_DIR': str(data),
                    'BB_CONFIG': str(config),
                    'BB_SKIP_PREPARE': '1',
                },
                spawn_probe=fake_probe,
            )

            self.assertEqual(code, 0)
            self.assertEqual(len(seen), 2)
            self.assertEqual(seen[0], {'render': '856x480', 'output': '1280x720', 'restart_by_exit': '1'})
            self.assertEqual(seen[1], {'render': '424x240', 'output': '1280x720', 'restart_by_exit': '1'})


if __name__ == '__main__':
    unittest.main()
