#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Starts the game on Windows using the shared scripts/launch_plan.py Launch Planner.

Prepares the game via launch_plan.plan_and_prepare(), then runs bin/bb-probe.exe.
When bb-probe.exe requests an in-game restart (exit code RESTART_EXIT_CODE with
BB_RESTART_BY_EXIT=1), run() loops and replans from the updated bbport.ini so
resolution/preset patches and memory sizing recompute cleanly.
"""
import contextlib
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

PORT_DIR = Path(getattr(sys, '_MEIPASS', '')) if getattr(sys, 'frozen', False) else Path(__file__).resolve().parent
# A frozen launcher keeps the scripts next to Bloodborne.exe (the package's folder).
PACKAGE_DIR = Path(sys.executable).resolve().parent if getattr(sys, 'frozen', False) else PORT_DIR
SCRIPTS = PACKAGE_DIR / 'scripts'
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import launch_plan

_log_file = None
_bbsc_server = None
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def log(line):
    if sys.stdout:  # a windowed Bloodborne.exe has none
        print(line, flush=True)
    if _log_file:
        _log_file.write(line + '\n')
        _log_file.flush()


def setdefault(env, name, value):
    if not env.get(name):
        env[name] = value


def start_bbsc_local_server(shadnet, user):
    """Starts scripts/bbsc_gameserver locally for the duration of this game session."""
    global _bbsc_server
    server_script = SCRIPTS / 'bbsc_gameserver' / 'bbsc_gameserver.py'
    if not shadnet or not server_script.is_file():
        return
    try:
        socket.create_connection(('127.0.0.1', 20443), timeout=1).close()
        log('Co-op: a game server answers on 127.0.0.1:20443 already: used')
        return
    except OSError:
        pass

    folder = Path(user) / 'bbsc'
    (folder / 'logs').mkdir(parents=True, exist_ok=True)
    if getattr(sys, 'frozen', False):
        command = [sys.executable, '--run-script', str(server_script)]
    else:
        command = [sys.executable, str(server_script)]
    command += [
        '--shadnet', shadnet, '--host', '127.0.0.1', '--port', '20443', '--relay-port', '0',
        '--no-bbhost', '--no-playlog', '--data', str(folder / 'online-data.json'),
        '--log-dir', str(folder / 'logs'),
    ]
    console = open(folder / 'logs' / 'gameserver-console.log', 'w', encoding='utf-8', errors='replace')
    try:
        _bbsc_server = subprocess.Popen(
            command, stdout=console, stderr=subprocess.STDOUT,
            cwd=str(server_script.parent), creationflags=NO_WINDOW,
        )
    except OSError as error:
        log(f'Co-op: the local BBSC game server did not start ({error})')
        return
    finally:
        console.close()
    log(f'Co-op: BBSC game server on 127.0.0.1:20443 (summons -> {shadnet}:31315)')


def stop_bbsc_local_server():
    global _bbsc_server
    if _bbsc_server is not None:
        with contextlib.suppress(OSError):
            _bbsc_server.terminate()
        with contextlib.suppress(Exception):
            _bbsc_server.wait(timeout=5)
        _bbsc_server = None


def _apply_windows_env(env, data, logs, stamp):
    test_env = PACKAGE_DIR / 'test-env.txt'
    test_keys = []
    if test_env.is_file():
        for line in test_env.read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, value = line.strip().partition('=')
            if sep and not key.startswith('#'):
                test_keys.append(key.strip())
                setdefault(env, key.strip(), value.strip())
    if env.get('BB_SAVE_LOG') == '1':
        env['BB_FRAME_STATS'] = '1'
        setdefault(env, 'BB_FRAME_LOG', str(logs / f'{stamp}.frames.csv'))
    elif sys.platform == 'win32':
        if env.get('BB_FRAME_STATS') != 'off' and env.get('BB_SAVE_LOG') != '0':
            setdefault(env, 'BB_FRAME_STATS', '1')
            setdefault(env, 'BB_FRAME_LOG', str(logs / f'{stamp}.frames.csv'))
            setdefault(env, 'BB_SLOW_BIND_LOG', '20000')
        elif env.get('BB_FRAME_STATS') == 'off':
            env.pop('BB_FRAME_STATS', None)
    if sys.platform == 'win32':
        if (PACKAGE_DIR / 'fsr4_shaders').is_dir():
            setdefault(env, 'BB_FSR4_DIR', str(PACKAGE_DIR / 'fsr4_shaders'))
        if (data / 'fsr4_411').is_dir():
            setdefault(env, 'BB_FSR411_DIR', str(data / 'fsr4_411'))
    return test_env.name, test_keys


def run(extra_args=(), env=None, on_line=None, *, spawn_probe=None):
    """Prepares and runs the game; returns the exit code. Loops on RESTART_EXIT_CODE."""
    global _log_file
    current_env = dict(os.environ if env is None else env)
    data = Path(current_env.get('BB_DATA_DIR') or PACKAGE_DIR)
    logs = data / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d_%H%M%S')
    _log_file = open(logs / f'{stamp}.log', 'w', encoding='utf-8', errors='replace')
    test_env_name, test_keys = _apply_windows_env(current_env, data, logs, stamp)
    log(f'Log: {logs / f"{stamp}.log"}')
    if (PACKAGE_DIR / 'VERSION').is_file():
        log(f"bbport {(PACKAGE_DIR / 'VERSION').read_text().strip()}")
    if test_keys:
        log(f'Test settings ({test_env_name}): ' + ' '.join(f'{key}={current_env[key]}' for key in test_keys))

    bindir = PACKAGE_DIR / 'bin'
    caps = bindir / ('bb-gpu-capabilities.exe' if sys.platform == 'win32' else 'bb-gpu-capabilities')
    current_env['BB_RESTART_BY_EXIT'] = '1'

    try:
        while True:
            mod_game = None
            try:
                plan = launch_plan.plan_and_prepare(
                    current_env,
                    port_dir=PACKAGE_DIR,
                    data_dir=data,
                    platform='win32',
                    caps_path=caps,
                    skip_prepare=(current_env.get('BB_SKIP_PREPARE') == '1'),
                    log=log,
                )
                mod_game = plan.mod_game
                for key in plan.env_unset:
                    current_env.pop(key, None)
                current_env.update(plan.env_set)

                if plan.bbsc_shadnet:
                    start_bbsc_local_server(plan.bbsc_shadnet, plan.user_dir)

                probe = Path(current_env.get('BB_PROBE') or bindir / 'bb-probe.exe')
                args = [str(probe), *plan.probe_args, *extra_args]
                log('Starting ' + ' '.join(args))

                if spawn_probe is not None:
                    code = spawn_probe(args, dict(current_env), str(data), on_line)
                else:
                    with subprocess.Popen(
                        args, env=current_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        cwd=str(data), creationflags=NO_WINDOW,
                    ) as game_process:
                        for raw in game_process.stdout:
                            line = raw.decode('utf-8', 'replace').rstrip('\r\n')
                            log(line)
                            if on_line:
                                on_line(line)
                        code = game_process.wait()

                if code == launch_plan.RESTART_EXIT_CODE:
                    log('— restarting through launch planner —')
                    continue

                log(f'— the game exited (code {code}) —')
                return code
            finally:
                stop_bbsc_local_server()
                if mod_game is not None:
                    shutil.rmtree(mod_game, ignore_errors=True)
    except (OSError, RuntimeError) as error:
        log(f'STOP: {error}')
        return 1
    finally:
        if _log_file is not None:
            _log_file.close()
            _log_file = None


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
