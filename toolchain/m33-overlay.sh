#!/usr/bin/env bash
# M33 additions to the sysroot's libc overlay (seeded from Resonance's; idempotent), as rpcs3's
# toolchain/rpcs3-overlay.sh:
#
# - ucontext_t: on the PS4 the FreeBSD machine context starts at byte 0x40 of the signal context
#   (measured on hardware by DolphinPS4's probe; DeCast-S4 v01.04), not at 0x10 where the overlay's
#   FreeBSD layout put it. PPSSPP's fault handler (Common/ExceptionHandlerSetup.cpp, fast memory)
#   reads and edits registers through uc_mcontext.mc_* (Common/MachineContext.h), so the type
#   itself must match the hardware.
# - machine/fpu.h: FreeBSD's struct savefpu (the XMM registers in mc_fpstate).
set -euo pipefail
PSB="${PSB:-$HOME/m33-build}"
export PS4_SYSROOT="$PSB/sysroot"
source "$(dirname "$0")/env.sh"
O="$PS4_SYSROOT/libc-overlay"

if ! grep -q __m33_ps4_pad "$O/bits/signal.h"; then
    python3 - "$O/bits/signal.h" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = "\tsigset_t uc_sigmask;\n\tmcontext_t uc_mcontext;\n"
new = ("\tsigset_t uc_sigmask;\n"
       "\tchar __m33_ps4_pad[0x30]; /* M33: the PS4 kernel's mcontext is at byte 0x40 */\n"
       "\tmcontext_t uc_mcontext;\n")
assert s.count(old) == 1, "unexpected bits/signal.h"
open(p, "w").write(s.replace(old, new))
EOF
fi

mkdir -p "$O/machine"
cat > "$O/machine/fpu.h" <<'EOF'
/* M33 (from rpcs3): FreeBSD/amd64 <machine/fpu.h> layout of the FXSAVE area in mcontext_t.mc_fpstate. */
#ifndef _M33_MACHINE_FPU_H
#define _M33_MACHINE_FPU_H
#include <stdint.h>
struct envxmm {
	uint16_t en_cw;
	uint16_t en_sw;
	uint8_t en_tw;
	uint8_t en_zero;
	uint16_t en_opcode;
	uint64_t en_rip;
	uint64_t en_rdp;
	uint32_t en_mxcsr;
	uint32_t en_mxcsr_mask;
};
struct fpacc87 {
	uint8_t fp_bytes[10];
};
struct xmmacc {
	uint8_t xmm_bytes[16];
};
struct savefpu {
	struct envxmm sv_env;
	struct {
		struct fpacc87 fp_acc;
		uint8_t fp_pad[6];
	} sv_fp[8];
	struct xmmacc sv_xmm[16];
	uint8_t sv_pad[96];
} __attribute__((aligned(16)));
#endif
EOF

# The layout the code will see, checked by the compiler
cat > /tmp/m33-ucontext-check.c <<'EOF'
#include <stddef.h>
#include <signal.h>
#include <machine/fpu.h>
_Static_assert(offsetof(ucontext_t, uc_mcontext) == 0x40, "uc_mcontext must be at 0x40");
_Static_assert(offsetof(ucontext_t, uc_mcontext.mc_rip) == 0x40 + 20 * 8, "mc_rip must be word 20");
_Static_assert(offsetof(struct savefpu, sv_xmm) == 160, "sv_xmm at 160");
int main(void) { return 0; }
EOF
"$PS4_CC" $PS4_BASE_FLAGS -fsyntax-only /tmp/m33-ucontext-check.c && echo "overlay: ucontext/savefpu layout OK"
