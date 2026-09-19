align 4, db 0
ne2k_state:
    dd NET_DEVICE_UNAVAILABLE
ne2k_mac:
    times 6 db 0
    times 2 db 0
ne2k_tx_frames             dd 0
ne2k_rx_frames             dd 0
ne2k_malformed_lengths     dd 0
ne2k_rx_capacity_drops     dd 0
ne2k_rx_overruns           dd 0
ne2k_dma_timeouts          dd 0
ne2k_tx_timeouts           dd 0
ne2k_reset_timeouts        dd 0
ne2k_resets                dd 0
ne2k_fatal_failures        dd 0
ne2k_tx_length             dd 0
ne2k_rx_destination        dd 0
ne2k_rx_capacity           dd 0
ne2k_rx_length             dd 0
ne2k_rx_header             dd 0
ne2k_dma_dummy_word        dd 0
ne2k_current_page          db 0
ne2k_packet_page           db 0
    times 2 db 0
ne2k_state_end:

%ifdef KERNEL_TEST_NET_DMA_TIMEOUT_ONCE
ne2k_test_dma_timeout_once db 1
%endif
%ifdef KERNEL_TEST_NET_TX_TIMEOUT_ONCE
ne2k_test_tx_timeout_once db 1
%endif
%ifdef KERNEL_TEST_NET_OVERRUN_ONCE
ne2k_test_overrun_once db 1
%endif
%ifdef KERNEL_TEST_NET_RESET_TIMEOUT_ONCE
ne2k_test_reset_timeout_once db 1
%endif
%ifdef NE2K_TEST_HOOKS
ne2k_test_hook_marker db "MINI_OS_PHASE_C_FAULT_INJECTION_ONLY", 0
%endif
