from pathlib import Path
import shutil,subprocess,pytest
from tools.recomp.test_continuations import translate
@pytest.fixture(scope='module')
def shutdown_fixture(tmp_path_factory):
    root=Path(__file__).resolve().parents[2];work=tmp_path_factory.mktemp('shutdown');compiler=shutil.which('cl');assert compiler
    program={0x11000:bytes.fromhex('5589e56a02ff1524000200c9c3'),0x11200:bytes.fromhex('5589e5ff050c00020068800002006a006a00ff1520000200ff0510000200c9c20400')}
    patch=pytest.MonkeyPatch()
    try:body=translate(patch,True,program)
    finally:patch.undo()
    (work/'shutdown.c').write_text((root/'tools/recomp/test_shutdown_notifications.c.inc').read_text()+body+(root/'tools/recomp/test_shutdown_notifications_main.c.inc').read_text())
    libraries=list((root.parents[1]/'build/native/xboxrecomp/src').glob('*/*.lib'));assert len(libraries)==8
    libraries.append(root.parents[1]/'build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib')
    command=[compiler,'/nologo','/O2','/Gy','/MD','/utf-8','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/I'+str(root/'src'),'/I'+str(root.parents[1]/'config'),str(work/'shutdown.c'),'/Fe:'+str(work/'shutdown.exe'),'/link','/OPT:REF',*[str(p) for p in libraries],'kernel32.lib','user32.lib','gdi32.lib','advapi32.lib','ole32.lib','shell32.lib','d3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib','winmm.lib','xaudio2_8.lib','xinput.lib','bcrypt.lib','ntdll.lib','userenv.lib','ws2_32.lib']
    p=subprocess.run(command,cwd=work,capture_output=True,text=True,errors='replace');assert p.returncode==0,p.stdout+p.stderr
    return work/'shutdown.exe',root/'tools/conformance/test.xbe'
def run(shutdown_fixture,tmp_path,mode,native=0):
    exe,xbe=shutdown_fixture
    return subprocess.run([str(exe),mode,str(xbe),str(tmp_path/'shutdown.state'),str(native)],capture_output=True,text=True,errors='replace',timeout=15)
def test_shutdown_original_priority_mutation_and_irql(shutdown_fixture,tmp_path):
    p=run(shutdown_fixture,tmp_path,'order');assert p.returncode==0,p.stdout+p.stderr;assert 'order/unregister/mutation/IRQL passed' in p.stdout
@pytest.mark.parametrize('native',[0,1])
def test_shutdown_guest_callbacks_restore_once(shutdown_fixture,tmp_path,native):
    p=run(shutdown_fixture,tmp_path,'run',native);assert p.returncode==0,p.stdout+p.stderr;assert p.stdout=='shutdown complete once\n'
    p=run(shutdown_fixture,tmp_path,'save',native);assert p.returncode==23,p.stdout+p.stderr
    p=run(shutdown_fixture,tmp_path,'load',native);assert p.returncode==0,p.stdout+p.stderr;assert p.stdout=='shutdown complete once\n'
def test_other_firmware_routine_does_not_notify(shutdown_fixture,tmp_path):
    p=run(shutdown_fixture,tmp_path,'other');assert p.returncode==0,p.stdout+p.stderr;assert not p.stdout
def test_shutdown_same_program_roundtrip(shutdown_fixture,tmp_path):
    p=run(shutdown_fixture,tmp_path,'roundtrip');assert p.returncode==0,p.stdout+p.stderr;assert p.stdout=='shutdown complete once\n'*2
