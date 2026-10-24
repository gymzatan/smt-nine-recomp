from pathlib import Path
import shutil, subprocess, sys
import pytest

@pytest.mark.skipif(sys.platform != "win32", reason="Uses real Windows file handles")
def test_file_flush_reports_actual_host_failure(tmp_path):
    root=Path(__file__).resolve().parents[2]
    compiler=shutil.which("cl")
    assert compiler, "Run in the existing MSVC environment"
    fixture=tmp_path/"flush.c"
    fixture.write_bytes((root/"tools/recomp/test_file_flush.c.inc").read_bytes())
    command=[compiler,"/nologo","/O2","/Gy","/Gw","/utf-8","/D_CRT_SECURE_NO_WARNINGS",
             "/I"+str(root/"src"),"/I"+str(root/"src/kernel"),"/I"+str(root/"include"),
             "/I"+str(root.parents[1]/"config"),str(root/"src/kernel/kernel_file.c"),str(fixture),
             "/Fe:"+str(tmp_path/"flush.exe"),"/link","/OPT:REF","/INCREMENTAL:NO"]
    result=subprocess.run(command,cwd=tmp_path,capture_output=True,text=True,errors="replace")
    assert result.returncode==0,result.stdout+result.stderr
    result=subprocess.run([str(tmp_path/"flush.exe"),str(tmp_path/"data.bin")],
                          cwd=tmp_path,capture_output=True,text=True,errors="replace")
    assert result.returncode==0,result.stdout+result.stderr
    assert "flush success/failure pass" in result.stdout
