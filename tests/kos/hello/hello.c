/* First entry of the KallistiOS test corpus (WP1.5). Prints over the serial/dcload console and
 * exits, so it can run under the translator's bare harness, under Flycast, and on hardware. */
#include <kos.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("dream-recomp kos hello: sizeof(int)=%d\n", (int)sizeof(int));
    return 0;
}
