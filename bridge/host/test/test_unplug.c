// Kill the fake bridge after init: the library must flag in_error promptly and not spin.
#include "libpinedio-usb.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
    struct pinedio_inst inst = {0};
    if (pinedio_init(&inst, NULL) != 0) { printf("FAIL init\n"); return 1; }
    kill((pid_t)atoi(argv[1]), SIGKILL);  // "unplug"
    int ms = 0;
    while (!inst.in_error && ms < 2000) { usleep(10000); ms += 10; }
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    double cpu = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
    printf("in_error=%d after %d ms, cpu=%.3fs\n", inst.in_error, ms, cpu);
    pinedio_deinit(&inst);
    if (!inst.in_error || cpu > 0.5) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
