#include "vigil/outbox.h"
#include <stdio.h>
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: vigil-outbox-init ABSOLUTE_PATH\n");
        return 2;
    }
    struct vigil_outbox *o = NULL;
    enum vigil_store_result rc = vigil_outbox_open(&o, argv[1], true);
    if (rc == VIGIL_STORE_OK) rc = vigil_outbox_close(o);
    if (rc != VIGIL_STORE_OK)
        fprintf(stderr, "outbox initialization failed: %s\n", vigil_store_name(rc));
    return rc == VIGIL_STORE_OK ? 0 : 2;
}
