#include "internal.h"

_Static_assert(sizeof(struct net_context) >= NET_FRAME_MAX * 2U,
               "network context must contain two complete frame buffers");
_Static_assert(sizeof(struct net_context) <= 4096U,
               "Phase D network context must remain within 4 KiB");

struct net_context net_global_context;
