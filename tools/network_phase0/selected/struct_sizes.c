#include "libssh2_priv.h"

unsigned char mini_os_phase0_session_size[sizeof(LIBSSH2_SESSION)];
unsigned char mini_os_phase0_transport_size[sizeof(struct transportpacket)];
