from pathlib import Path
import sys,os,subprocess,pytest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_checkpoint_gpu import native_gpu
@pytest.mark.parametrize('hardware,backend',[('0','D3D11'),('1','D3D11'),('1','D3D12'),('1','Vulkan'),('1','OpenGL')])
def test_volume_texture(native_gpu,tmp_path,hardware,backend):
    executable,xbe=native_gpu
    p=subprocess.run([str(executable),'volume-texture',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),hardware],env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=180)
    assert p.returncode==0,p.stdout+p.stderr
    assert '[VOLUME TEXTURE]' in p.stderr
@pytest.mark.parametrize('linear',[0,1])
def test_volume_missing_source_stops(native_gpu,tmp_path,linear):
    executable,xbe=native_gpu
    p=subprocess.run([str(executable),'volume-missing-palette-'+str(linear),str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'0'],env={**os.environ,'RECOMP_GPU_BACKEND':'D3D11'},capture_output=True,text=True,errors='replace',timeout=180)
    assert p.returncode==5,p.stdout+p.stderr
    assert '[STOP] volume texel has no decoded source' in p.stderr
@pytest.mark.parametrize('backend',['D3D11','Vulkan'])
def test_standing_volume_w(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    p=subprocess.run([str(executable),'standing-volume-w',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],env={**os.environ,'RECOMP_GPU_BACKEND':backend},capture_output=True,text=True,errors='replace',timeout=180)
    assert p.returncode==0,p.stdout+p.stderr
    assert '[STANDING VOLUME W]' in p.stderr
