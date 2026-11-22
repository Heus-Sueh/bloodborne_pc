# SPDX-FileCopyrightText: Copyright 2026 bbport contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guard the C <-> C++ Runtime-GPU ABI seam (src/runtime_gpu_abi.h).

Ensures:
1. No file under gpu/ introduces ad-hoc `extern "C"` declarations of `runtime_*` symbols
   (all cross-seam runtime declarations live in src/runtime_gpu_abi.h).
2. src/runtime_gpu_abi.h compiles cleanly as both strict C11 and C++20.
3. tests/test_gpu_runtime_stubs.cpp includes src/runtime_gpu_abi.h and compiles cleanly
   against it (catching type mismatches like uint32_t vs uint64_t at compile time).
"""
from paths import ROOT
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest


class RuntimeGpuAbiSeamTests(unittest.TestCase):
    def test_no_adhoc_extern_c_runtime_declarations_in_gpu(self):
        offenders = []
        for path in sorted((ROOT / 'gpu').rglob('*')):
            if path.suffix not in ('.h', '.hpp', '.cpp', '.c') or 'third_party' in path.parts:
                continue
            raw_text = path.read_text(encoding='utf-8', errors='replace')
            # Preserve line numbers while stripping /* ... */ block comments.
            cleaned = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), raw_text, flags=re.S)
            lines = cleaned.splitlines()
            in_block = False
            depth = 0
            for idx, raw in enumerate(lines, 1):
                code = raw.split('//', 1)[0]
                if re.search(r'extern\s+"C"\s*\{', code):
                    in_block = True
                    depth = code.count('{') - code.count('}')
                    if re.search(r'\bruntime_[A-Za-z0-9_]+\b', code) and ';' in code:
                        offenders.append(f'{path.relative_to(ROOT)}:{idx}: {raw.strip()}')
                    if depth <= 0:
                        in_block = False
                elif in_block:
                    if re.search(r'\bruntime_[A-Za-z0-9_]+\b', code) and ';' in code and '{' not in code:
                        offenders.append(f'{path.relative_to(ROOT)}:{idx}: {raw.strip()}')
                    depth += code.count('{') - code.count('}')
                    if depth <= 0:
                        in_block = False
                elif re.search(r'extern\s+"C"\b.*\bruntime_[A-Za-z0-9_]+\b', code):
                    # Allow actual definitions (opening brace) such as bbnet_glue.cpp forwarding stubs.
                    if '{' not in code:
                        offenders.append(f'{path.relative_to(ROOT)}:{idx}: {raw.strip()}')

        self.assertEqual(
            offenders,
            [],
            'Found ad-hoc extern "C" runtime_* declarations in gpu/ (use #include "runtime_gpu_abi.h"):\n'
            + '\n'.join(offenders),
        )

    def test_header_and_gpu_stubs_compile_in_c_and_cpp(self):
        cc = shutil.which('gcc') or shutil.which('cc')
        cxx = shutil.which('g++') or shutil.which('c++')
        if not cc or not cxx:
            self.skipTest('No C/C++ compiler found')

        with tempfile.TemporaryDirectory() as tmp:
            c_probe = pathlib.Path(tmp) / 'abi_check.c'
            c_probe.write_text('#include "runtime_gpu_abi.h"\nint main(void) { return 0; }\n', encoding='utf-8')
            subprocess.run(
                [cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                 f'-I{ROOT / "src"}', str(c_probe)],
                check=True,
            )

            subprocess.run(
                [cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                 f'-I{ROOT / "src"}', str(ROOT / 'tests' / 'test_gpu_runtime_stubs.cpp')],
                check=True,
            )


if __name__ == '__main__':
    unittest.main()
