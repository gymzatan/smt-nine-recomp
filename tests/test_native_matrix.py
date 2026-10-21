"""Check native matrix arithmetic and transpose bits, including aliases."""
from pathlib import Path
import shutil,subprocess

ROOT=Path(__file__).resolve().parents[1]

def test_native_matrix_matches_original_cpu(tmp_path):
    compiler=shutil.which('cl');assembler=shutil.which('ml64')
    assert compiler and assembler,'requires the existing x64 MSVC environment'
    obj=tmp_path/'native_matrix.obj';exe=tmp_path/'native_matrix_test.exe'
    subprocess.run([assembler,'/nologo','/c','/Fo'+str(obj),str(ROOT/'game/src/native_matrix.asm')],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([compiler,'/nologo','/O2','/D_CRT_SECURE_NO_WARNINGS',
                   str(ROOT/'game/tests/native_matrix_test.c'),str(obj),'/Fe:'+str(exe)],
                   cwd=tmp_path,check=True,capture_output=True)
    subprocess.run([str(exe),str(ROOT/'game/tests/data/native_matrix_vectors.bin')],
                   cwd=tmp_path,check=True,capture_output=True)
