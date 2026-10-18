"""Declared Windows build tools shared by build and language import."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_if_changed(path, content):
    path = Path(path)
    if isinstance(content, str):
        content = content.replace('\n', os.linesep).encode('utf-8')
    if not path.exists() or path.read_bytes() != content:
        path.write_bytes(content)


def required_program(name):
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f'Required tool is not on PATH: {name}')
    return Path(path)


def visual_studio():
    if 'NINE_VS_ROOT' in os.environ:
        installation = Path(os.environ['NINE_VS_ROOT'])
    else:
        installer = Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
        if not installer.is_file():
            raise RuntimeError('Visual Studio Installer/vswhere is absent. Set NINE_VS_ROOT explicitly for a portable Build Tools installation.')
        result = subprocess.run([str(installer), '-latest', '-products', '*', '-requires',
            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath', '-utf8'],
            capture_output=True, text=True, encoding='utf-8', check=True)
        if not result.stdout.strip():
            raise RuntimeError('No registered Visual Studio C++ installation was found.')
        installation = Path(result.stdout.strip())
    vcvars = installation/'VC/Auxiliary/Build/vcvars64.bat'
    cmake = installation/'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    ninja = installation/'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    for path in (vcvars, cmake, ninja):
        if not path.is_file():
            raise RuntimeError(f'Install Visual Studio C++ tools with CMake and Ninja: {path}')
    with tempfile.TemporaryDirectory(prefix='nine-msvc-') as temporary:
        script = Path(temporary)/'environment.cmd'
        script.write_text('@echo off\ncall "'+str(vcvars)+'" > "'+str(Path(temporary)/'vcvars.log')+'" 2>&1\nif errorlevel 1 exit /b %errorlevel%\nset\n', encoding='utf-8')
        result = subprocess.run('cmd.exe /u /d /s /c ""'+str(script)+'""', capture_output=True, text=True, encoding='utf-16-le', check=True)
    environment = dict(os.environ)
    for line in result.stdout.splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            environment[key.upper()] = value
    return cmake, ninja, environment


def run(command, **kwargs):
    print('Running:', subprocess.list2cmdline([str(x) for x in command]), flush=True)
    subprocess.run([str(x) for x in command], check=True, **kwargs)
