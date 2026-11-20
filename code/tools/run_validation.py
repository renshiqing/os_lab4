#!/usr/bin/env python3
"""Constructive Lab4 tests in an isolated tree; official tests stay unchanged."""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--qemu', default='qemu-system-riscv64')
    opt = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    opt.out = opt.out.resolve()
    opt.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='lab4-verify-') as directory:
        build = Path(directory) / 'code'
        shutil.copytree(root, build, ignore=shutil.ignore_patterns('bin', 'obj', '__pycache__', '.qemu.out', '.gdb.in'))
        verify = build / 'kern/verify'
        verify.mkdir()
        for name in ('maps.c', 'switch_check.S'):
            shutil.copyfile(root / 'tools/verification' / name, verify / name)
        init = build / 'kern/init/init.c'
        text = init.read_text()
        text = text.replace('    proc_init();', '    extern void lab4_map_checks(void), lab4_proc_probe(void);\n    lab4_map_checks();\n    proc_init();\n    lab4_proc_probe();')
        init.write_text(text)
        proc = build / 'kern/process/proc.c'
        text = proc.read_text()
        text = text.replace('//LAB4 function2 2412396', '''static int probe_fail_pcb, probe_fail_stack, probe_fail_mm;
static void *probe_kmalloc(size_t n) { return probe_fail_pcb ? NULL : kmalloc(n); }
//LAB4 function2 2412396''')
        assert text.count('struct proc_struct *proc = kmalloc(sizeof(*proc));') == 1
        text = text.replace('struct proc_struct *proc = kmalloc(sizeof(*proc));', 'struct proc_struct *proc = probe_kmalloc(sizeof(*proc));')
        text = text.replace('int error = setup_kstack(proc);', 'int error = probe_fail_stack ? -E_NO_MEM : setup_kstack(proc);')
        text = text.replace('    assert(current->mm == NULL);', '    if (probe_fail_mm) return -E_NO_MEM;\n    assert(current->mm == NULL);')
        text = text.replace('static int\ninit_main(void *arg)\n{', 'static int\ninit_main(void *arg)\n{\n    extern void lab4_schedule_probe(void);\n    lab4_schedule_probe();')
        text += '\n' + (root / 'tools/verification/proc_probe.inc').read_text()
        proc.write_text(text)
        make = build / 'Makefile'
        text = make.read_text().replace('KCFLAGS\t\t+=', 'KINCLUDE += kern/verify\nKSRCDIR += kern/verify\n\nKCFLAGS\t\t+=')
        make.write_text(text)
        result = subprocess.run(['make', '-j4'], cwd=build, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (opt.out / 'build.txt').write_text('$ make -j4\n' + result.stdout)
        result.check_returncode()
        args = [opt.qemu, '-machine', 'virt', '-nographic', '-bios', 'default',
                '-device', 'loader,file=bin/ucore.img,addr=0x80200000']
        try:
            result = subprocess.run(args, cwd=build, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=25)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b''
            if isinstance(output, bytes): output = output.decode(errors='replace')
            (opt.out / 'qemu.txt').write_text(output)
            raise RuntimeError('Lab4 supplementary kernel timed out') from error
        (opt.out / 'qemu.txt').write_text('$ ' + ' '.join(args) + '\n' + result.stdout)
        result.check_returncode()
        assert 'Lab4 extra: ALL PASS' in result.stdout
        assert 'assertion failed' not in result.stdout
        assert result.stdout.count('kernel panic at') == 1 and 'process exit!!.' in result.stdout
        summary = {'passed': True, 'same_page_reinsert': True, 'aliases_and_replacement': True,
                   'idempotent_remove': True, 'actual_load_remap_store': True, 'table_failure_budgets': [0, 1],
                   'existing_parent_preserved_on_oom': True, 'mapping_test_all_pages_recovered': True,
                   'pcb_stack_mm_failures': True, 'process_limit': True, 'stack_and_frame_alignment': True,
                   'worker_count': 2, 'yields_per_worker': 25, 'callee_saved_registers': 12,
                   'stack_sentinels': True, 'skip_sleeping_zombie': True,
                   'idle_fallback': 'synthetic selection, no actual idle switch', 'self_selection': True,
                   'interrupt_state_preserved': True, 'qemu_exit': result.returncode,
                   'expected_end': 'teacher do_exit panic -> original kmonitor -> SBI shutdown',
                   'scope': 'single-hart kernel threads; no user fork, preemptive scheduling or process reaping'}
        (opt.out / 'summary.json').write_text(json.dumps(summary, indent=2))
        print('PASS supplementary Lab4: mapping/TLB, OOM rollback, PCB/stack failures, two workers x 25, registers/stack and scheduler boundaries')


if __name__ == '__main__':
    main()
