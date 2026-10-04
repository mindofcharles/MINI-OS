#include <wolfssh/ssh.h>

int main(void)
{
    WOLFSSH_CTX *context;
    WOLFSSH *session;

    if (wolfSSH_Init() != WS_SUCCESS) {
        return 1;
    }
    context = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (context == NULL) {
        wolfSSH_Cleanup();
        return 2;
    }
    session = wolfSSH_new(context);
    if (session == NULL) {
        wolfSSH_CTX_free(context);
        wolfSSH_Cleanup();
        return 3;
    }
    wolfSSH_free(session);
    wolfSSH_CTX_free(context);
    wolfSSH_Cleanup();
    return 0;
}
