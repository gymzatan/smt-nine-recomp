"""Exercise the same floating compare helper used by generated game code."""
from pathlib import Path
import shutil,subprocess

ROOT=Path(__file__).resolve().parents[1]

def test_native_fcmp_operands_and_status(tmp_path):
    compiler=shutil.which('cl')
    assert compiler,'requires the existing x64 MSVC environment'
    exe=tmp_path/'native_fcmp_test.exe'
    subprocess.run([compiler,'/nologo','/O2','/D_CRT_SECURE_NO_WARNINGS',
                   '/I'+str(ROOT/'config'),'/I'+str(ROOT/'vendor/xboxrecomp/src'),
                   str(ROOT/'game/tests/native_fcmp_test.c'),'/Fe:'+str(exe)],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True,capture_output=True)
