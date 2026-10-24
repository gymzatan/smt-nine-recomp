from pathlib import Path
import sys,os,subprocess,pytest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_checkpoint_gpu import native_gpu
@pytest.mark.parametrize('threads',[1,4])
def test_software_depth_uses_actual_ram_and_existing_workers(native_gpu,tmp_path,threads):
    executable,xbe=native_gpu
    p=subprocess.run([str(executable),'software-depth-ram',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11','RECOMP_RASTER_THREADS':str(threads)},capture_output=True,text=True,errors='replace',timeout=60)
    assert p.returncode==0,p.stdout+p.stderr
    assert '[SOFTWARE DEPTH RAM]' in p.stderr
