[bits 32]

; Keep the NE2000 implementation ordered explicitly: constants and build-time
; checks, device lifecycle, Remote DMA, public frame operations, then state.
%include "OS_src/kernel/net/ne2k/definitions.asm"
%include "OS_src/kernel/net/ne2k/lifecycle.asm"
%include "OS_src/kernel/net/ne2k/dma.asm"
%include "OS_src/kernel/net/ne2k/api.asm"
%include "OS_src/kernel/net/ne2k/state.asm"
