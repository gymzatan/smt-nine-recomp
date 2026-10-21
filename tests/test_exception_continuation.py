"""Guest catch transfer retains callers and supports their existing checkpoints."""
from pathlib import Path
import shutil,subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_exception_transfer_preserves_callers_and_checkpoint(tmp_path):
    compiler=shutil.which('cl');assert compiler
    exe=tmp_path/'exception_continuation.exe'
    result=subprocess.run([compiler,'/nologo','/O2',
        '/I'+str(ROOT/'vendor/xboxrecomp/src'),
        str(ROOT/'game/tests/data/exception_continuation.c'),
        str(ROOT/'vendor/xboxrecomp/src/kernel/recomp_continuation.c'),
        '/Fe:'+str(exe)],cwd=tmp_path,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    assert subprocess.run([str(exe)],cwd=tmp_path).returncode==0
    assert subprocess.run([str(exe),'missing-owner'],cwd=tmp_path).returncode==5
