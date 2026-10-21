"""Use the pinned lifter and explicit project edits to reproduce accepted code."""
from pathlib import Path
import argparse
import collections
import json
import re
import shutil
import sys
import tempfile
from common import ROOT, digest, write_if_changed

sys.path.insert(0, str(ROOT/'vendor/xboxrecomp'))
from tools.recomp import config
from tools.recomp.translator import BatchTranslator


def normalize(body):
    if '    #define fp_stack_top g_fp_top\n' in body:
        body = body.replace('    #define fp_stack_top g_fp_top\n', '')
        body = body.replace('    #undef fp_stack_top\n', '')
        body = body.replace('fp_stack_top', 'g_fp_top')
    return body


def apply_project_edits(body, row):
    previous_end=0
    for offset,removed,inserted,before,after in row['edits']:
        end=offset+len(removed)
        if offset<previous_end or end>len(body) or body[max(0,offset-len(before)):end+len(after)]!=before+removed+after:
            raise ValueError('Project codegen edit context differs at '+row['name'])
        previous_end=end
    for offset,removed,inserted,before,after in reversed(row['edits']):
        body=body[:offset]+inserted+body[offset+len(removed):]
    return body


def lower_exception_continuation(body):
    """Resume an original catch without replaying a discarded native callee."""
    prologue, separator, _ = body.partition('\nloc_')
    if '{"ebp", &ebp, sizeof ebp}' not in prologue:
        return body
    if not separator:
        raise ValueError('Missing body label for an EBP frame')
    first_label=len(prologue)
    before='    default: recomp_cont_invalid();\n    }'
    at=body.rfind(before,0,first_label)
    if at<0:
        raise ValueError('Missing continuation dispatch for an EBP frame')
    after=('    default:\n'
           '        if ((_cont_resume >> 32) != 7) recomp_cont_invalid();\n'
           '        RECOMP_ITAIL((uint32_t)_cont_resume);\n'
           '        recomp_cont_leave(&_cont_frame); return;\n'
           '    }')
    return body[:at]+after+body[at+len(before):]


def generate(image, destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    recipe = json.loads((ROOT/'config/codegen.json').read_text())
    metadata = ROOT/'config'
    config.configure_from_xbe(str(image))
    batch = BatchTranslator(str(image), str(metadata/'functions.json'), str(metadata/'labels.json'),
        str(metadata/'identified_functions.json'), str(metadata/'abi_functions.json'), continuations=True,
        trace_functions={int(a,16) for a in recipe['trace_functions']},
        protected_function_starts=set(recipe['manual_protected']))
    units = collections.defaultdict(list)
    for index, row in enumerate(recipe['functions']):
        generated = batch.translate_single(row['address'])
        if not generated:
            raise RuntimeError('The existing lifter did not emit '+row['name'])
        definition = re.search(r'^void \w+\(void\)\n\{',generated,re.M)
        if definition is None:
            raise ValueError('Generated definition not found: '+row['name'])
        body = generated[definition.start():].strip()
        body = re.sub(r'^void \w+\(void\)', 'void '+row['name']+'(void)', body, count=1)
        body = normalize(body)
        body=lower_exception_continuation(apply_project_edits(body,row))
        units[row['file']].append(body)
        if index % 1000 == 0:
            print(f'Generated {index+1}/{len(recipe["functions"])} functions', flush=True)
    units['recomp_stubs_unresolved.c'].extend(
        f'void sub_{address}(void) {{ recomp_icall_fail_log(0x{address}u); }}' for address in recipe['unresolved_stops'])
    declarations = {row['name'] for row in recipe['functions']}
    declarations.update('sub_'+a for a in recipe['unresolved_stops'])
    declarations.update(name for _,name in recipe['dispatch'])
    header = '#pragma once\n#include "recomp_types.h"\n'+''.join('void '+name+'(void);\n' for name in sorted(declarations))
    write_if_changed(destination/'recomp_funcs.h', header)
    write_if_changed(destination/'recomp_types.h', (ROOT/'config/recomp_types.h').read_bytes())
    prefix = '#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n#include "kernel/recomp_continuation.h"\n#include <math.h>\n'
    for filename, bodies in units.items():
        write_if_changed(destination/filename, prefix+'\n\n'.join(bodies)+'\n')
    with tempfile.TemporaryDirectory() as temporary:
        dispatch = Path(temporary)/'recomp_dispatch.c'
        batch._write_dispatch_table([(int(address,16),name,'') for address,name in recipe['dispatch']],
                                   str(dispatch), 'recomp_funcs.h')
        write_if_changed(destination/'recomp_dispatch.c', dispatch.read_bytes())
    receipt = {'image_sha256':digest(image), 'functions':len(recipe['functions']), 'source':'Pinned BatchTranslator plus config/codegen.json project edits',
               'files':{p.name:digest(p) for p in destination.iterdir() if p.suffix in ('.c','.h')}}
    write_if_changed(destination/'receipt.json', json.dumps(receipt, indent=2)+'\n')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    generate(args.image, args.destination)
