#include "libpinedio-usb.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static struct pinedio_inst inst;
static volatile int fired, cb_read = -1;
static void cb(void) { cb_read = pinedio_digital_read(&inst, 6); fired++; }  // talks to the radio from the callback
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
int main(void) {
    CHECK(pinedio_init(&inst, NULL) == 0);
    printf("serial='%s' product='%s'\n", inst.serial_number, inst.product_string);
    uint8_t out[300], in[300];
    for (int i = 0; i < 300; i++) out[i] = (uint8_t)i;
    CHECK(pinedio_transceive_select(&inst, out, in, 4) == 4);
    CHECK(in[0] == 0xFF && in[3] == 0xFC);
    CHECK(pinedio_transceive(&inst, out, in, 300) == 300 && in[299] == (uint8_t)~(uint8_t)299);
    CHECK(pinedio_digital_read(&inst, 4) == 0);
    CHECK(pinedio_attach_interrupt(&inst, 6, PINEDIO_INT_MODE_RISING, cb) == 0);
    CHECK(pinedio_digital_write(&inst, 1, 0) == 0);    // fake raises DIO1
    for (int i = 0; i < 100 && !fired; i++) usleep(1000);
    CHECK(fired == 1 && cb_read == 1);
    // event arriving while a request is reading its reply
    fired = 0;
    CHECK(pinedio_digital_write(&inst, 1, 0) == 0);
    CHECK(pinedio_transceive_select(&inst, out, in, 2) == 2 && in[1] == 0xFE);
    for (int i = 0; i < 100 && !fired; i++) usleep(1000);
    CHECK(fired == 1);
    // timing: 1000 BUSY reads
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 1000; i++) pinedio_digital_read(&inst, 4);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("round trip over pty: %.1f us\n", ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e3 / 1000);
    pinedio_deinit(&inst);
    printf("ALL PASS\n");
    return 0;
}
