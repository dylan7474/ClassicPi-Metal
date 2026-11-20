# cpubench: Basilisk II's CPU core vs umac's, on a real Pi

A throwaway Circle kernel (2026-11-20) to decide whether a Basilisk II
(Mac II class, 68030) version of ClassicPi-Metal would be fast enough on a
Zero 2 W. It runs the same 68000 loop (`bench_loop.h`, 389 instructions per
iteration) on:

- umac's Musashi core (68000), with umac's own `m68kconf.h`, and
- Basilisk II's UAE interpreter core (`uae_cpu_2021`, 68030 mode, no JIT:
  the JIT is x86-only),

15 s each, at the Pi's default ARM clock and again at maximum
(`CCPUThrottle`), and writes `cpubench-<board>.txt` to the card.

For scale (estimates, not measured): a real Mac Plus ~1,600-2,000
iterations/s, a real IIci (25 MHz 68030) ~10,000-13,000.

## Building

Needs a configured circle-stdlib (e.g. `../../build/pi3/circle-stdlib`
after a ClassicPi-Metal build) and Basilisk II's source
(github.com/kanjitalk755/macemu, tested at 0407cbd) with its CPU tables
generated on the PC:

    cd macemu/BasiliskII && mkdir gen && cd gen
    # config.h for the host: SIZEOF_* (8-byte long/pointer), HAVE_UNISTD_H,
    # DIRECT_ADDRESSING, USE_CPU_EMUL_SERVICES, FPU_IEEE
    gcc -I. -I../src/Unix -I../src/include -I../src/uae_cpu_2021 -o build68k ../src/uae_cpu_2021/build68k.c
    ./build68k < ../src/uae_cpu_2021/table68k > cpudefs.cpp
    g++ -I. -I../src/Unix -I../src/include -I../src/uae_cpu_2021 -o gencpu \
        ../src/uae_cpu_2021/gencpu.c ../src/uae_cpu_2021/readcpu.cpp cpudefs.cpp
    ./gencpu        # cpuemu.cpp cpustbl.cpp cpufunctbl.cpp cputbl.h

Copy into `uae/`: those four plus `cpudefs.cpp`, and from
`src/uae_cpu_2021/`: `newcpu.cpp readcpu.cpp memory.cpp fpu/fpu_ieee.cpp`.
Copy Musashi (`external/umac/external/Musashi`, all `.c`/`.h`, `softfloat/`)
and the target's generated `m68kops.c/.h` into `mus/`. Then:

    make BASILISK=/path/to/macemu/BasiliskII/src TARGET_STDLIB=../../build/pi3/circle-stdlib

Licences: Basilisk II is GPL v2 or later (compatible with Circle's GPLv3);
umac/Musashi are permissive.
