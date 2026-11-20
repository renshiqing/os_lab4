#!/usr/bin/env python3
"""Real QEMU/GDB observations of page operations and first kernel-thread switch."""
from pathlib import Path
import argparse
import json
import re
import subprocess
import tempfile


def check(expression, message):
    return [f'if !({expression})', f'  printf "FAIL: {message}\\n"', '  quit 1', 'end']


def run_session(root, out, qemu, port, name, commands):
    commands = ['set pagination off', 'set confirm off', 'set print pretty on', 'set trace-commands on',
                'set architecture riscv:rv64', f'target remote localhost:{port}'] + commands
    commands += ['detach', 'quit']
    (out / f'{name}.gdb').write_text('\n'.join(commands) + '\n')
    qargs = [qemu, '-machine', 'virt', '-nographic', '-bios', 'default',
             '-device', 'loader,file=bin/ucore.img,addr=0x80200000', '-S', '-gdb', f'tcp:127.0.0.1:{port}']
    with (out / f'{name}-qemu.txt').open('w') as serial:
        process = subprocess.Popen(qargs, cwd=root, stdin=subprocess.DEVNULL, stdout=serial, stderr=subprocess.STDOUT)
        try:
            result = subprocess.run(['riscv64-unknown-elf-gdb', '-q', '-nx', '-batch', 'bin/kernel',
                                     '-ex', 'set remotetimeout 10', '-x', str(out / f'{name}.gdb')],
                                    cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=40)
            (out / f'{name}.txt').write_text(result.stdout)
            result.check_returncode()
            assert f'PASS: {name}' in result.stdout
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--qemu', default='qemu-system-riscv64')
    parser.add_argument('--port', type=int, default=1238)
    opt = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = opt.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    pmm_lines = (root / 'kern/mm/pmm.c').read_text().splitlines()
    remove_line = pmm_lines.index('    page_remove(boot_pgdir_va, 0x0);') + 1
    mapping = ['thbreak *page_insert', 'continue', 'printf "FIRST PAGE INSERT ENTRY\\n"',
               'set $root = (unsigned long *)$a0', 'set $mapped = (struct Page *)$a1',
               'set $insert_return = $ra', 'p/x $a2', 'p/x $a3', 'p $mapped->ref']
    mapping += check('$a2 == 0 && $mapped->ref == 0', 'expected first check_pgdir insertion')
    mapping += ['thbreak *$insert_return', 'continue', 'printf "FIRST PAGE INSERT RETURN\\n"',
                'p $mapped->ref', 'p/x $root[0]',
                'set $middle = (unsigned long *)((($root[0] >> 10) << 12) + va_pa_offset)',
                'set $leaf = (unsigned long *)((($middle[0] >> 10) << 12) + va_pa_offset)',
                'p/x $leaf[0]']
    mapping += check('$a0 == 0 && $mapped->ref == 1 && ($leaf[0] & 1) && ($leaf[0] >> 10) == (($mapped - pages) + nbase)', 'PPN/ref after insertion')
    # -O2 may inline page_remove and --gc-sections discards its standalone body.
    # Observe its real inlined invocation at the protected test source lines.
    mapping += [f'thbreak kern/mm/pmm.c:{remove_line}', 'continue', 'printf "FIRST PAGE REMOVE ENTRY\\n"', 'p $mapped->ref']
    mapping += check('$mapped->ref == 2', 'expected aliased page before removal')
    # The following assert's line table may point to its cold failure block.
    # Step the actual inlined path until both data effects are visible instead.
    mapping += ['set $remove_steps = 0', 'while (($mapped->ref != 1 || $leaf[0] != 0) && $remove_steps < 300)',
                '  si', '  set $remove_steps = $remove_steps + 1', 'end',
                'printf "FIRST PAGE REMOVE EFFECTS\\n"', 'p $remove_steps', 'p $mapped->ref', 'p/x $leaf[0]']
    mapping += check('$mapped->ref == 1 && $leaf[0] == 0', 'remove cleared PTE and retained referenced page')
    mapping += ['printf "PASS: mappings - actual insertion, PPN/ref and alias removal\\n"']
    run_session(root, out, opt.qemu, opt.port, 'mappings', mapping)

    assembly = subprocess.check_output(['riscv64-unknown-elf-objdump', '-d', 'bin/kernel'], cwd=root, text=True)
    body = assembly.split('<__trapret>:', 1)[1].split('\n\n', 1)[0]
    match = re.search(r'^\s*([0-9a-f]+):.*\bsret\b', body, re.M)
    assert match
    sret = int(match.group(1), 16)
    regs = ['ra', 'sp'] + [f's{i}' for i in range(12)]
    commands = ['thbreak *proc_init', 'continue', 'printf "PROCESS INITIALIZATION\\n"',
                'thbreak *cpu_idle', 'continue', 'set $idle = idleproc', 'set $newproc = initproc',
                'set $newtf = initproc->tf', 'printf "IDLE AND INIT READY\\n"', 'p current->pid',
                'p nr_process', 'p sizeof(struct context)', 'p sizeof(struct trapframe)',
                'p/x $newproc->context', 'p/x *$newtf']
    commands += check('current == $idle && $idle->pid == 0 && $newproc->pid == 1 && nr_process == 2', 'initial process identity')
    commands += check('sizeof(struct context) == 112 && sizeof(struct trapframe) == 288', 'context/frame size')
    commands += check('$newproc->context.ra == (unsigned long)forkret && $newproc->context.sp == (unsigned long)$newtf', 'initial context chain')
    commands += ['thbreak *proc_run', 'continue', 'printf "SCHEDULER SELECTED INIT\\n"', 'bt', 'p current->pid', 'p ((struct proc_struct *)$a0)->pid']
    commands += check('current == $idle && $a0 == (unsigned long)$newproc', 'schedule target')
    commands += ['thbreak *switch_to', 'continue', 'printf "SWITCH ENTRY AND SV39 SATP\\n"',
                 'set $from = (struct context *)$a0', 'set $to = (struct context *)$a1',
                 'p current->pid', 'p/x $satp', 'p/x $a0', 'p/x $a1', 'p/x $sp']
    commands += check('current == $newproc && $from == &$idle->context && $to == &$newproc->context', 'current/context consistency')
    commands += check('((unsigned long)$satp >> 60) == 8 && ($satp & 0xfffffffffff) == (boot_pgdir_pa >> 12)', 'Sv39 root page number')
    for reg in regs:
        commands += [f'set $old_{reg} = ${reg}', f'set $new_{reg} = $to->{reg}']
    # 14 actual stores followed by 14 actual loads, with interrupts disabled.
    commands += ['si'] * 28
    commands += ['printf "AFTER 28 SWITCH INSTRUCTIONS\\n"', 'p/x *$from', 'p/x $sp', 'p/x $ra']
    for reg in regs:
        commands += check(f'$from->{reg} == $old_{reg} && ${reg} == $new_{reg}', 'switch register ' + reg)
    commands += ['thbreak *forkret', 'si', 'printf "FIRST FORKRET\\n"', 'p/x $pc',
                 'thbreak *forkrets', 'continue', 'p/x $a0']
    commands += check('$a0 == (unsigned long)$newtf', 'forkrets frame argument')
    commands += ['thbreak *__trapret', 'continue', 'p/x $sp']
    commands += check('$sp == (unsigned long)$newtf', 'forkrets installed the new frame')
    commands += [f'thbreak *{sret:#x}', 'continue', 'printf "BEFORE FIRST SRET\\n"', 'p/x $sp', 'p/x $sepc', 'p/x $sstatus', 'p/x $s0', 'p/x $s1']
    commands += check('$sp == $newproc->kstack + 8192 && $sepc == (unsigned long)kernel_thread_entry', 'restored kernel stack/EPC')
    commands += check('$s0 == (unsigned long)init_main && $s1 == $newtf->gpr.s1 && ($sstatus & 0x122) == 0x120', 'function/argument/SPP/SPIE/SIE')
    commands += ['thbreak *kernel_thread_entry', 'si', 'printf "KERNEL THREAD ENTRY AFTER SRET\\n"', 'p/x $pc', 'p/x $sstatus']
    commands += check('$pc == (unsigned long)kernel_thread_entry && ($sstatus & 0x102) == 2', 'sret return state')
    commands += ['si']
    commands += check('$a0 == $newtf->gpr.s1', 'thread argument moved into a0')
    commands += ['thbreak *init_main', 'si', 'printf "INIT MAIN EXECUTES\\n"', 'p current->pid', 'x/s $a0']
    commands += check('current == $newproc && $pc == (unsigned long)init_main', 'init_main reached')
    commands += ['printf "PASS: threads - Sv39, 14 context registers, first frame/sret and init argument\\n"']
    run_session(root, out, opt.qemu, opt.port, 'threads', commands)
    summary = {'passed': True, 'actual_mapping_operations': True, 'alias_ref_before_remove': 2, 'alias_ref_after_remove': 1,
               'context_bytes': 112, 'trapframe_bytes': 288, 'switch_registers_checked': 14,
               'switch_instructions_stepped': 28, 'satp_mode': 8, 'satp_root_checked': True,
               'first_run_chain': ['schedule', 'proc_run', 'switch_to', 'forkret', 'forkrets', '__trapret', 'sret', 'kernel_thread_entry', 'init_main'],
               'sret_address': hex(sret), 'init_argument': 'Hello world!!',
               'expected_end': 'original do_exit panic -> kmonitor -> SBI shutdown'}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2))
    print('PASS GDB Lab4: mappings/refcounts, Sv39, 28 switch steps/14 registers, first frame/sret and init_main')


if __name__ == '__main__':
    main()
