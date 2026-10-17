"""Use the pinned lifter and explicit project edits to reproduce accepted code."""
from pathlib import Path
import argparse
import collections
import hashlib
import json
import re
import shutil
import sys
from common import ROOT, digest

sys.path.insert(0, str(ROOT/'vendor/xboxrecomp'))
from tools.recomp import config
from tools.recomp.translator import BatchTranslator


def normalize(body):
    if '    #define fp_stack_top g_fp_top\n' in body:
        body = body.replace('    #define fp_stack_top g_fp_top\n', '')
        body = body.replace('    #undef fp_stack_top\n', '')
        body = body.replace('fp_stack_top', 'g_fp_top')
    return body


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
        if hashlib.sha256(body.encode()).hexdigest() != row['generated_sha256']:
            raise ValueError('Codegen input/template differs at '+row['name'])
        for offset, removed, inserted in reversed(row['edits']):
            body = body[:offset]+inserted+body[offset+removed:]
        if hashlib.sha256(body.encode()).hexdigest() != row['accepted_sha256']:
            raise ValueError('Project codegen edit does not reproduce '+row['name'])
        units[row['file']].append(body)
        if index % 1000 == 0:
            print(f'Generated {index+1}/{len(recipe["functions"])} functions', flush=True)
    units['recomp_stubs_unresolved.c'].extend(
        f'void sub_{address}(void) {{ recomp_icall_fail_log(0x{address}u); }}' for address in recipe['unresolved_stops'])
    declarations = {row['name'] for row in recipe['functions']}
    declarations.update('sub_'+a for a in recipe['unresolved_stops'])
    declarations.update(name for _,name in recipe['dispatch'])
    header = '#pragma once\n#include "recomp_types.h"\n'+''.join('void '+name+'(void);\n' for name in sorted(declarations))
    (destination/'recomp_funcs.h').write_text(header)
    shutil.copyfile(ROOT/'config/recomp_types.h', destination/'recomp_types.h')
    prefix = '#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n#include "kernel/recomp_continuation.h"\n#include <math.h>\n'
    for filename, bodies in units.items():
        (destination/filename).write_text(prefix+'\n\n'.join(bodies)+'\n')
    batch._write_dispatch_table([(int(address,16),name,'') for address,name in recipe['dispatch']],
                               str(destination/'recomp_dispatch.c'), 'recomp_funcs.h')
    receipt = {'image_sha256':digest(image), 'functions':len(recipe['functions']), 'source':'Pinned BatchTranslator plus config/codegen.json project edits',
               'files':{p.name:digest(p) for p in destination.iterdir() if p.suffix in ('.c','.h')}}
    (destination/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    generate(args.image, args.destination)
