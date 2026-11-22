#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Cross-platform Launch Planner shared by run.sh (Linux) and run.py (Windows).

Runs the platform-independent boot preparation steps in-process (mods merged view,
eboot/module/content preparation, monitor resolution + hybrid preset negotiation,
graphics heap sizing, patch compilation, runtime defaults, and online host overrides)
so neither run.sh nor run.py duplicates launch policy or spawns patches.py four times.
"""
from __future__ import annotations

import argparse
import contextlib
from dataclasses import dataclass, field
import os
from pathlib import Path
import re
import subprocess
import sys

SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import content_profile
import game_check
import link_libc
import link_modules
import mods as mods_mod
import online as online_mod
import patches as patches_mod
import prepare as prepare_mod

# Dedicated exit code returned by bb-probe when BB_RESTART_BY_EXIT=1 so run.py
# loops cleanly and replans patches/resolution from the updated bbport.ini.
RESTART_EXIT_CODE = 75

_RES_RE = re.compile(r'^[1-9][0-9]*x[1-9][0-9]*$')
_NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


@dataclass
class LaunchPlan:
    game_dir: Path
    original_game: Path
    mod_game: Path | None
    out_dir: Path
    user_dir: Path
    env_set: dict[str, str] = field(default_factory=dict)
    env_unset: list[str] = field(default_factory=list)
    bbsc_shadnet: str = ''
    probe_args: list[str] = field(default_factory=list)


def _setdefault(env: dict[str, str], key: str, value: str) -> None:
    if not env.get(key):
        env[key] = value


def _large_scene_memory(env: dict[str, str], scene: str) -> None:
    width, _, height = scene.partition('x')
    if width.isdigit() and height.isdigit() and int(width) * int(height) > 1920 * 1080:
        _setdefault(env, 'BB_DMEM_MB', '9152')


def _default_query_displays(caps_path: Path | None, env: dict[str, str]) -> list[str]:
    if caps_path is None or not caps_path.is_file():
        return []
    try:
        done = subprocess.run(
            [str(caps_path), '--displays'],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding='utf-8',
            errors='replace',
            env=env,
            creationflags=_NO_WINDOW,
        )
        return done.stdout.splitlines()
    except OSError:
        return []


def _select_monitor_size(rows: list[str], wanted_display: str) -> str:
    chosen = ''
    for number, row in enumerate(rows, 1):
        parts = row.split('\t')
        if len(parts) != 3:
            continue
        name, size, primary = parts
        if wanted_display:
            if wanted_display in (name, str(number)):
                chosen = size
        elif primary == '1':
            chosen = size
    return chosen if _RES_RE.match(chosen) else ''


def _prepare_mods(game: Path, out: Path, data: Path, user: Path, env: dict[str, str]) -> tuple[Path, Path | None]:
    original_resolved = game.resolve()
    mods_dir = Path(env.get('BB_MODS_DIR') or data / 'mods')
    mods_config = Path(env.get('BB_MODS_CONFIG') or data / 'mods.json')
    enabled = env.get('BB_MODS_ENABLED', '1') != '0'
    active = mods_mod.selected(mods_dir, mods_config) if enabled else []
    mods_mod.backup_saves(user / 'savedata', data / 'mods.last', active)
    if not active:
        return original_resolved, None
    overlay = mods_mod.build_overlay(original_resolved, out, [(name, mods_dir / name) for name in active])
    return overlay, overlay


@contextlib.contextmanager
def _scoped_environ(updates: dict[str, str], unsets: list[str]):
    saved: dict[str, str | None] = {}
    for k in set(updates) | set(unsets):
        saved[k] = os.environ.get(k)
    try:
        for k in unsets:
            os.environ.pop(k, None)
        for k, v in updates.items():
            os.environ[k] = v
        yield
    finally:
        for k, old in saved.items():
            if old is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = old


def _plan_online(env: dict[str, str], unset: list[str], port_dir: Path, user: Path,
                 platform: str, log) -> str:
    """Configure online environment variables and host overrides. Returns bbsc_shadnet if local BBSC is needed."""
    if env.get('BB_ONLINE') != '1':
        return ''

    if platform == 'win32':
        default_module = port_dir / 'online' / 'bbnet20.dll'
        module = Path(env.get('BB_NET_MODULE') or default_module)
        if not module.is_file():
            log(f'Online: no {module}: offline play')
            env.pop('BB_ONLINE', None)
            unset.append('BB_ONLINE')
            return ''
        env['BB_NET_MODULE'] = str(module)
    else:
        if not env.get('BB_NET_MODULE'):
            sub = 'bin' if env.get('BB_PREBUILT') else 'out'
            env['BB_NET_MODULE'] = str((port_dir / sub / 'gpu' / 'libbbnet20.so').resolve())
        if not env.get('SSL_CERT_FILE') or not os.access(env['SSL_CERT_FILE'], os.R_OK):
            for bundle in (
                env.get('NIX_SSL_CERT_FILE', ''),
                '/etc/ssl/certs/ca-certificates.crt',
                '/etc/pki/tls/certs/ca-bundle.crt',
                '/etc/ssl/cert.pem',
                '/etc/ssl/ca-bundle.pem',
            ):
                if bundle and os.access(bundle, os.R_OK):
                    env['SSL_CERT_FILE'] = bundle
                    break

    if env.get('BB_NET_BACKEND') == 'thd':
        if 'BB_BBSC' in env:
            env.pop('BB_BBSC', None)
            unset.append('BB_BBSC')
        if not env.get('BB_THD_TOKEN') or not env.get('BB_THD_ONLINE_ID'):
            raise RuntimeError("The Hunter's Dream: not signed in (launcher: Online -> Sign in with Discord).")
        overrides_path, base = online_mod.write_thd_overrides(user, env.get('BB_COMMUNITY_SERVER', ''))
        env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(overrides_path)
        _setdefault(env, 'BB_THD_SERVER', base + ':18671')
        _setdefault(env, 'BB_THD_AUTH', base)
        for name in ('BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID',
                     'BB_SHADNET_PASSWORD', 'BB_SHADNET_TOKEN'):
            env.pop(name, None)
            unset.append(name)
        if platform == 'win32':
            log(f"Online: {Path(env['BB_NET_MODULE']).name}, The Hunter's Dream {env['BB_THD_SERVER']} as {env['BB_THD_ONLINE_ID']}")
        return ''

    _setdefault(env, 'BB_SHADNET_SERVER', online_mod.SHADNET_SERVER)
    _setdefault(env, 'BB_SHADNET_WEBAPI', 'http://' + env['BB_SHADNET_SERVER'].rsplit(':', 1)[0] + ':31315')

    bbsc_shadnet = ''
    server_script = port_dir / 'scripts' / 'bbsc_gameserver' / 'bbsc_gameserver.py'
    if env.get('BB_BBSC') == '1' and server_script.is_file():
        with _scoped_environ(env, unset):
            bbsc_shadnet = online_mod.bbsc_local_server() or ''
        if bbsc_shadnet:
            env['BB_COMMUNITY_SERVER'] = 'http://127.0.0.1'

    with _scoped_environ(env, unset):
        try:
            overrides_path = online_mod.write_overrides(
                user,
                env.get('BB_COMMUNITY_SERVER', ''),
                env.get('BB_SHADNET_WEBAPI', ''),
                plain=bool(env.get('BB_NET_MODULE')),
            )
            env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(overrides_path)
        except Exception as err:
            log(f'Online: no host overrides ({err})')

    if platform == 'win32':
        log(f'Online: {Path(env["BB_NET_MODULE"]).name}, shadNet {env["BB_SHADNET_SERVER"]}, '
            f'WebAPI {env["BB_SHADNET_WEBAPI"]}' + (', BBSC' if env.get('BB_BBSC') == '1' else ''))
    return bbsc_shadnet


def plan_and_prepare(
    base_env: dict[str, str],
    *,
    port_dir: Path,
    data_dir: Path,
    platform: str = 'linux',
    caps_path: Path | None = None,
    skip_prepare: bool = False,
    skip_online: bool = False,
    query_displays=None,
    log=print,
) -> LaunchPlan:
    """Run all cross-platform launch preparation in-process and return a LaunchPlan."""
    port_dir = Path(port_dir).resolve()
    data_dir = Path(data_dir).resolve()
    out_dir = data_dir / 'out'
    out_dir.mkdir(parents=True, exist_ok=True)

    original_env = dict(base_env)
    env = dict(base_env)
    unset: list[str] = []

    _setdefault(env, 'BB_CONFIG', str(data_dir / 'bbport.ini'))
    user_dir = Path(env.get('BB_USER_DIR') or data_dir / 'user')
    _setdefault(env, 'BB_GPU_USER_DIR', str(user_dir))
    user_dir.mkdir(parents=True, exist_ok=True)

    game_default = port_dir.parent / 'CUSA03173'
    game = Path(env.get('BB_GAME_DIR') or game_default)
    if not (game / 'eboot.bin').is_file():
        raise RuntimeError(f'No eboot.bin in {game} (set BB_GAME_DIR).')

    original_game = game.resolve()
    game, mod_game = _prepare_mods(game, out_dir, data_dir, user_dir, env)

    if not skip_prepare:
        prepare_mod.prepare(game, out_dir)
        link_libc.link(game, out_dir)
        link_modules.link(game, out_dir)
        content_profile.prepare(game, out_dir, sku=env.get('BB_CONTENT_SKU') or 'full')

    # Recompute automatic resolution sizes on an in-game restart.
    if env.get('BB_AUTO_RENDER_RES') == '1':
        for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
            env.pop(key, None)
            unset.append(key)

    fps = env.get('BB_FPS') or 'uncap'
    config_path = Path(env['BB_CONFIG'])
    ini_settings = patches_mod.read_settings(config_path)

    # output_res=auto: pick the requested BB_DISPLAY or primary monitor size once.
    if ini_settings.get('output_res', 'auto').lower() == 'auto' and not env.get('BB_MONITOR_SIZE'):
        if query_displays is None:
            rows = _default_query_displays(caps_path, env)
        else:
            rows = query_displays(caps_path, env)
        size = _select_monitor_size(rows, env.get('BB_DISPLAY', ''))
        if size:
            env['BB_MONITOR_SIZE'] = size
            log(f"Output: the monitor's resolution, {size} (output_res=auto)")

    # Resolution & hybrid preset negotiation in-process (zero patches.py subprocesses).
    with _scoped_environ(env, unset):
        scaled_render = scaled_output = ''
        if not env.get('BB_RENDER_RES'):
            sizes = patches_mod.scaled_sizes(ini_settings, align=2)
            if sizes:
                scaled_render = f'{sizes[0][0]}x{sizes[0][1]}'
                scaled_output = f'{sizes[1][0]}x{sizes[1][1]}'

        live = False
        live_hybrid_enabled = env.get('BB_LIVE_HYBRID', '1') == '1'
        if not scaled_output and not env.get('BB_RENDER_RES') and live_hybrid_enabled:
            sizes = patches_mod.scaled_sizes(ini_settings, align=8)
            if sizes:
                scaled_render = f'{sizes[0][0]}x{sizes[0][1]}'
                scaled_output = f'{sizes[1][0]}x{sizes[1][1]}'
                live = True

        if scaled_output and not live:
            live = env.get('BB_LIVE_RES', '1') == '1'

        if live and live_hybrid_enabled:
            sizes = patches_mod.scaled_sizes(ini_settings, align=8)
            if sizes:
                scaled_render = f'{sizes[0][0]}x{sizes[0][1]}'
                scaled_output = f'{sizes[1][0]}x{sizes[1][1]}'
            env['BB_RENDER_RES'] = scaled_render
            env['BB_OUTPUT_RES'] = scaled_output
            env['BB_AUTO_RENDER_RES'] = '1'
            env['BB_LIVE_HYBRID'] = '1'
            _large_scene_memory(env, scaled_render)
            log(f'Output {scaled_output}: scene {scaled_render}, live preset changes over it (BB_LIVE_HYBRID=0: proxies only)')
        elif live:
            log(f'Output {scaled_output}: live resolution changes (BB_LIVE_HYBRID=0: proxies only)')
        elif scaled_output:
            env['BB_RENDER_RES'] = scaled_render
            env['BB_OUTPUT_RES'] = scaled_output
            env['BB_AUTO_RENDER_RES'] = '1'
            _large_scene_memory(env, scaled_render)
            log(f'Output {scaled_output}: scene {scaled_render}, direct memory {env.get("BB_DMEM_MB") or "5056"} MiB (BB_LIVE_RES=0: no live changes)')

        # Texture pack heap detection (#240).
        large_heaps_raw = env.get('BB_LARGE_HEAPS', '')
        pack_desc = ''
        if large_heaps_raw not in ('0', '1'):
            if skip_prepare:
                large_heaps_raw = '0'
            else:
                found = game_check.texture_pack(game, out_dir / 'texture-pack.json')
                if found:
                    large_heaps_raw = '1'
                    pack_desc = f'a texture pack: {found[0]} has {found[1]}-pixel textures, the retail game {game_check.RETAIL_TEXTURE_SIDE} at most'
                else:
                    large_heaps_raw = '0'
        large_heaps = (large_heaps_raw == '1')
        if large_heaps:
            _setdefault(env, 'BB_DMEM_MB', '9152')
            log(f'Graphics heaps: larger ({pack_desc or "BB_LARGE_HEAPS=1"}), direct memory {env["BB_DMEM_MB"]} MiB (BB_LARGE_HEAPS=0: the game\'s own sizes)')

        if not skip_prepare or (out_dir / 'eboot.elf').is_file():
            patches_mod.compile_all(
                out=out_dir,
                fps=fps,
                extra=env.get('BB_PATCHES', ''),
                settings_path=config_path,
                game_dir=game,
                render_res=env.get('BB_RENDER_RES', ''),
                output_res=env.get('BB_OUTPUT_RES', ''),
                patches_dir=Path(env.get('BB_PATCHES_DIR') or data_dir / 'patches'),
                patches_config=Path(env.get('BB_PATCHES_CONFIG') or data_dir / 'patches.json'),
                large_heaps=large_heaps,
                xml=port_dir / 'patches' / 'Bloodborne.xml',
                env=env,
            )

    _setdefault(env, 'BB_COPY_GPU_BUFFERS', '1')
    _setdefault(env, 'BB_GPU_WRITE_TWINS', '1')
    _setdefault(env, 'BB_GPU_WRITE_TWINS_MAX', '65536')
    _setdefault(env, 'BB_VBLANK_HZ', {'uncap': '1000', '90': '90'}.get(fps, '60'))
    _setdefault(env, 'BB_FPS', fps)

    if 'BB_SHADER_RECIPES' not in env and (port_dir / 'shader_recipes.bin').is_file():
        env['BB_SHADER_RECIPES'] = str(port_dir / 'shader_recipes.bin')
    if 'BB_IMAGE_SHAPES' not in env and (port_dir / 'image_shapes.txt').is_file():
        env['BB_IMAGE_SHAPES'] = str(port_dir / 'image_shapes.txt')
    _setdefault(env, 'BB_SHADER_GAME_DIR', str(original_game))

    bbsc_shadnet = ''
    if not skip_online:
        bbsc_shadnet = _plan_online(env, unset, port_dir, user_dir, platform, log)

    probe_args = [
        str(out_dir / 'boot-linked.bin'),
        '--content-profile', str(out_dir / 'content.bin'),
        '--patches', str(out_dir / 'patches.bin'),
        '--app0', str(game),
        '--user', str(user_dir),
        '--timeout', env.get('BB_TIMEOUT') or '0',
    ]

    # Compute minimal delta for shell consumers (every new/changed key + every explicit unset).
    env_set = {k: v for k, v in env.items() if original_env.get(k) != v}
    env_unset = [k for k in dict.fromkeys(unset) if k not in env]

    return LaunchPlan(
        game_dir=game,
        original_game=original_game,
        mod_game=mod_game,
        out_dir=out_dir,
        user_dir=user_dir,
        env_set=env_set,
        env_unset=env_unset,
        bbsc_shadnet=bbsc_shadnet,
        probe_args=probe_args,
    )


def write_nul_plan(plan: LaunchPlan, dest: Path) -> None:
    """Write NUL-delimited records (KIND\\0KEY\\0VALUE\\0) parsed by bash builtins without sed/eval."""
    parts: list[bytes] = []

    def add(kind: str, key: str, value: str) -> None:
        parts.append(kind.encode('utf-8') + b'\0' + key.encode('utf-8') + b'\0' + value.encode('utf-8') + b'\0')

    for k in plan.env_unset:
        add('UNSET', k, '')
    for k, v in sorted(plan.env_set.items()):
        add('SET', k, v)
    add('META', 'game', str(plan.game_dir))
    add('META', 'original_game', str(plan.original_game))
    add('META', 'mod_game', str(plan.mod_game) if plan.mod_game else '')
    add('META', 'bbsc_shadnet', plan.bbsc_shadnet)
    for arg in plan.probe_args:
        add('ARG', '', arg)

    dest.write_bytes(b''.join(parts))


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port-dir', type=Path, default=SCRIPTS_DIR.parent)
    p.add_argument('--data-dir', type=Path, default=None)
    p.add_argument('--caps', type=Path, default=None)
    p.add_argument('--plan-file', type=Path, required=True)
    p.add_argument('--skip-prepare', action='store_true')
    p.add_argument('--skip-online', action='store_true')
    a = p.parse_args(argv)

    env = dict(os.environ)
    data_dir = a.data_dir if a.data_dir is not None else Path(env.get('BB_DATA_DIR') or a.port_dir)
    skip_prepare = a.skip_prepare or env.get('BB_SKIP_PREPARE') == '1'
    try:
        plan = plan_and_prepare(
            env,
            port_dir=a.port_dir,
            data_dir=data_dir,
            platform='linux',
            caps_path=a.caps,
            skip_prepare=skip_prepare,
            skip_online=a.skip_online,
            log=print,
        )
    except RuntimeError as err:
        print(str(err), file=sys.stderr)
        return 1
    write_nul_plan(plan, a.plan_file)
    return 0


if __name__ == '__main__':
    sys.exit(main())
