#!/bin/sh
# The teacher batch script connects without loading an ELF. A multiarch GDB
# therefore needs an explicit target architecture before reading remote state.
debugger=$1
shift
exec "$debugger" -ex 'set architecture riscv:rv64' "$@"
