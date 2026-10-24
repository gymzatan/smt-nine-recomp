from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_checkpoint_gpu import native_gpu
import os,subprocess,pytest
@pytest.mark.parametrize('backend,scale',[(b,s) for b in ('D3D11','D3D12','Vulkan','OpenGL') for s in (1,2)])
def test_depth_ram_alias_writeback_resize_and_checkpoint(native_gpu,tmp_path,backend,scale):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'depth-ram-coherence',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':str(scale),'DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[DEPTH RAM]' in result.stderr

@pytest.mark.parametrize('backend',['D3D11','D3D12','Vulkan','OpenGL'])
def test_depth_read_only_and_repeated_dirty_draw_guard(native_gpu,tmp_path,backend):
    executable,xbe=native_gpu
    result=subprocess.run([str(executable),'depth-write-guards',str(xbe),str(tmp_path/'gpu.state'),str(tmp_path/'memory.state'),'1'],env={**os.environ,'RECOMP_GPU_BACKEND':backend,'RECOMP_RENDER_SCALE':'1','DXVK_LOG_PATH':str(tmp_path)},capture_output=True,text=True,errors='replace',timeout=90)
    assert result.returncode==0,result.stdout+result.stderr
    assert '[DEPTH GUARD]' in result.stderr
