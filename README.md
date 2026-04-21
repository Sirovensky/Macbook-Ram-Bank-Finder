# a1990-memtest

Bootable USB/ISO memory tester that identifies which individual BGA RAM package
has failed on MacBook Pro A1990 (15" 2018/2019).

Base: fork of [memtest86plus](https://github.com/memtest86plus/memtest86plus).
Added: Coffee Lake-H IMC physical-address decoder + per-package chip mapping
keyed on A1990 board topology.

## Status

Pre-alpha. Scaffolding in place. Bring-up required on target hardware.

See [docs/PLAN.md](docs/PLAN.md) for milestones and [docs/IMC_NOTES.md](docs/IMC_NOTES.md)
for register research.

## Layout

```
memtest86plus/      upstream, pristine clone
src/                our additions (copied into memtest86plus by apply.sh)
  cfl_decode.[ch]     PA -> (channel, rank, bank, row, col) for Coffee Lake client
  a1990_topology.[ch] SMBIOS match + BGA-package map per variant
  error_hook.c        injected into error.c common_err path
patches/            diffs against memtest86plus tree
scripts/            build helpers
docs/               plan + research notes
```

## Prerequisites (macOS host)

```
brew install x86_64-elf-gcc x86_64-elf-binutils xorriso
# grub-mkrescue: build grub2 from source or use linux VM
```

ISO build currently requires Linux host (or Docker) because `grub-mkrescue`
is not reliably packaged on macOS.

## Quick start

```
./scripts/apply.sh           # copy src/ into memtest86plus
cd memtest86plus/build       # or build64
make                         # produce memtest.efi
cd ../..
./scripts/build-iso.sh       # produce a1990-memtest.iso
```

## Target

- MacBook Pro A1990 (15" 2018/2019), Coffee Lake-H i7/i9.
- T2 Security Utility: set to "No Security" + allow external boot before use.
- UEFI boot only.

## License

GPL-2.0 (inherits from memtest86plus).
