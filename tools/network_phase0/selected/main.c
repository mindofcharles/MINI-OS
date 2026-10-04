#include "libssh2.h"

int main(void)
{
    LIBSSH2_SESSION *session;

    if (libssh2_init(0) != 0) {
        return 1;
    }
    session = libssh2_session_init();
    if (session == 0) {
        libssh2_exit();
        return 2;
    }
    libssh2_session_free(session);
    libssh2_exit();
    return 0;
}
