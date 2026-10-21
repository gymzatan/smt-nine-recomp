"""Validate targeted project edits without locking the entire generated body."""
from pathlib import Path
import importlib.util,json,sys
import pytest

ROOT=Path(__file__).resolve().parents[1]

@pytest.fixture
def generator():
    sys.path.insert(0,str(ROOT/'tools'))
    spec=importlib.util.spec_from_file_location('project_generator',ROOT/'tools/generate.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module

def test_edits_allow_unrelated_generated_changes(generator):
    row={'name':'sub_test','edits':[[4,'old','new','0123',' end']]}
    assert generator.apply_project_edits('0123old end changed elsewhere',row)=='0123new end changed elsewhere'
    with pytest.raises(ValueError,match='context differs'):
        generator.apply_project_edits('0123BAD end changed elsewhere',row)

def test_generation_uses_current_lifter_body_without_fixed_hash(generator,tmp_path,monkeypatch):
    (tmp_path/'config').mkdir()
    recipe={'trace_functions':[],'manual_protected':[],'unresolved_stops':[],'dispatch':[],
            'functions':[{'address':4096,'name':'sub_test','file':'recomp.c','edits':[]}]}
    (tmp_path/'config/codegen.json').write_text(json.dumps(recipe))
    (tmp_path/'config/recomp_types.h').write_text('/* fixture */')
    image=tmp_path/'game.xbe';image.write_bytes(b'fixture')
    body='void lifted(void)\n{\n    new_lifter_output();\n}'
    class Batch:
        def __init__(self,*args,**kwargs):pass
        def translate_single(self,address):return body
        def _write_dispatch_table(self,rows,path,header):Path(path).write_text('/* dispatch */')
    monkeypatch.setattr(generator,'ROOT',tmp_path)
    monkeypatch.setattr(generator,'BatchTranslator',Batch)
    monkeypatch.setattr(generator.config,'configure_from_xbe',lambda path:None)
    receipt=generator.generate(image,tmp_path/'generated')
    assert receipt['functions']==1
    assert 'new_lifter_output();' in (tmp_path/'generated/recomp.c').read_text()


@pytest.mark.parametrize('suffix', ['', '\nloc_00001000:\n    return;'])
def test_exception_lowering_rejects_incomplete_ebp_frame(generator, suffix):
    body='void sub_test(void)\n{\n    {"ebp", &ebp, sizeof ebp};'+suffix+'\n}'
    with pytest.raises(ValueError, match='Missing .* for an EBP frame'):
        generator.lower_exception_continuation(body)
