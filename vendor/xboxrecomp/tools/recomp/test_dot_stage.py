from pathlib import Path
import sys,os,subprocess,pytest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_checkpoint_gpu import native_gpu
@pytest.mark.parametrize('hardware,backend',[('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_dot_texture_stage(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    p=subprocess.run([str(executable),'dot-texture-stage',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=180)
    assert p.returncode==0,p.stdout+p.stderr
    assert '[DOT STAGE]' in p.stderr
