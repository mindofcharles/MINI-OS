; National Semiconductor DP8390D / NE2000-compatible polling transport.
; The programmed packet RAM layout assumes the standard 16 KiB NE2000
; window at 0x4000..0x7fff. IRQ delivery remains disabled in both the NIC
; IMR and the 8259 masks; completion is observed through bounded ISR polls.

NE2K_IO_BASE             equ 0x300
NE2K_IRQ                 equ 9
NE2K_DATA_PORT           equ NE2K_IO_BASE + 0x10
NE2K_RESET_PORT          equ NE2K_IO_BASE + 0x1F

NE2K_REG_CR              equ NE2K_IO_BASE + 0x00
NE2K_REG_PSTART          equ NE2K_IO_BASE + 0x01
NE2K_REG_PSTOP           equ NE2K_IO_BASE + 0x02
NE2K_REG_BNRY            equ NE2K_IO_BASE + 0x03
NE2K_REG_TPSR            equ NE2K_IO_BASE + 0x04
NE2K_REG_TBCR0           equ NE2K_IO_BASE + 0x05
NE2K_REG_TBCR1           equ NE2K_IO_BASE + 0x06
NE2K_REG_ISR             equ NE2K_IO_BASE + 0x07
NE2K_REG_RSAR0           equ NE2K_IO_BASE + 0x08
NE2K_REG_RSAR1           equ NE2K_IO_BASE + 0x09
NE2K_REG_RBCR0           equ NE2K_IO_BASE + 0x0A
NE2K_REG_RBCR1           equ NE2K_IO_BASE + 0x0B
NE2K_REG_RCR             equ NE2K_IO_BASE + 0x0C
NE2K_REG_TCR             equ NE2K_IO_BASE + 0x0D
NE2K_REG_DCR             equ NE2K_IO_BASE + 0x0E
NE2K_REG_IMR             equ NE2K_IO_BASE + 0x0F

NE2K_PAGE1_PAR0          equ NE2K_IO_BASE + 0x01
NE2K_PAGE1_CURR          equ NE2K_IO_BASE + 0x07
NE2K_PAGE1_MAR0          equ NE2K_IO_BASE + 0x08

NE2K_CR_STOP             equ 0x01
NE2K_CR_START            equ 0x02
NE2K_CR_TRANSMIT         equ 0x04
NE2K_CR_REMOTE_READ      equ 0x08
NE2K_CR_REMOTE_WRITE     equ 0x10
NE2K_CR_NO_DMA           equ 0x20
NE2K_CR_PAGE0            equ 0x00
NE2K_CR_PAGE1            equ 0x40

NE2K_CMD_STOP            equ NE2K_CR_STOP | NE2K_CR_NO_DMA | NE2K_CR_PAGE0
NE2K_CMD_START           equ NE2K_CR_START | NE2K_CR_NO_DMA | NE2K_CR_PAGE0
NE2K_CMD_PAGE1_STOP      equ NE2K_CR_STOP | NE2K_CR_NO_DMA | NE2K_CR_PAGE1
NE2K_CMD_PAGE1_START     equ NE2K_CR_START | NE2K_CR_NO_DMA | NE2K_CR_PAGE1
NE2K_CMD_DMA_READ        equ NE2K_CR_START | NE2K_CR_REMOTE_READ
NE2K_CMD_DMA_WRITE       equ NE2K_CR_START | NE2K_CR_REMOTE_WRITE

NE2K_ISR_PRX             equ 0x01
NE2K_ISR_PTX             equ 0x02
NE2K_ISR_RXE             equ 0x04
NE2K_ISR_TXE             equ 0x08
NE2K_ISR_OVW             equ 0x10
NE2K_ISR_RDC             equ 0x40
NE2K_ISR_RST             equ 0x80

NE2K_RSR_PRX             equ 0x01
NE2K_RSR_ERROR_MASK      equ 0x1E
NE2K_RCR_BROADCAST       equ 0x04
NE2K_RCR_MONITOR         equ 0x20
NE2K_TCR_LOOPBACK        equ 0x02
NE2K_DCR_WORD_WIDE       equ 0x49

NE2K_TX_START_PAGE       equ 0x40
NE2K_TX_PAGE_COUNT       equ 6
NE2K_RX_START_PAGE       equ NE2K_TX_START_PAGE + NE2K_TX_PAGE_COUNT
NE2K_RX_STOP_PAGE        equ 0x80
NE2K_RX_FIRST_PAGE       equ NE2K_RX_START_PAGE + 1
NE2K_RX_PAGE_COUNT       equ NE2K_RX_STOP_PAGE - NE2K_RX_START_PAGE

NE2K_PROM_BYTES          equ 32
NE2K_RESET_TIMEOUT_MS    equ 50
NE2K_DMA_TIMEOUT_MS      equ 50
NE2K_TX_TIMEOUT_MS       equ 250
NE2K_WAIT_ITERATIONS     equ 1000000

%ifdef KERNEL_TEST_NET_DMA_TIMEOUT_ONCE
    %ifndef NE2K_TEST_HOOKS
        %define NE2K_TEST_HOOKS 1
    %endif
%endif
%ifdef KERNEL_TEST_NET_TX_TIMEOUT_ONCE
    %ifndef NE2K_TEST_HOOKS
        %define NE2K_TEST_HOOKS 1
    %endif
%endif
%ifdef KERNEL_TEST_NET_OVERRUN_ONCE
    %ifndef NE2K_TEST_HOOKS
        %define NE2K_TEST_HOOKS 1
    %endif
%endif
%ifdef KERNEL_TEST_NET_RESET_TIMEOUT_ONCE
    %ifndef NE2K_TEST_HOOKS
        %define NE2K_TEST_HOOKS 1
    %endif
%endif

%if NET_FRAME_BUFFER_SIZE < NET_FRAME_MAX + 1
    %error "network frame buffers need private word-alignment padding"
%endif
%if NE2K_TX_PAGE_COUNT * 256 < NET_FRAME_MAX
    %error "NE2000 transmit pages cannot hold a maximum frame"
%endif
%if NE2K_RX_PAGE_COUNT < 8
    %error "NE2000 receive ring is unexpectedly small"
%endif
%if (PIC_MASTER_MASK & (1 << 2)) == 0
    %error "NE2000 polling requires the slave-PIC cascade to remain masked"
%endif
%if (PIC_SLAVE_MASK & (1 << (NE2K_IRQ - 8))) == 0
    %error "NE2000 polling requires IRQ9 to remain masked"
%endif

