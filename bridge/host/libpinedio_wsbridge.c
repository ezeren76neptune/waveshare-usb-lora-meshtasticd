// libpinedio-usb API over the Waveshare USB-TO-LoRa bridge firmware (../protocol.h).
// Lets meshtasticd's unmodified Ch341Hal drive the stick's SX1262 as if it were a MeshStick.
//
// Device: the tty whose USB parent matches the VID/PID (and serial, if searched for) that
// Ch341Hal sets from the yaml's USB_VID/USB_PID/USB_Serialnum. WSBRIDGE_PORT overrides it.
//
// Threads: requests run on the caller's thread under io_mutex and read their own reply.
// DIO1 events can arrive at any time between replies; whoever is reading the tty then
// notes them, and the irq thread runs the callback with no lock held, since RadioLib's
// handler talks to the radio itself.

// Built only in meshtasticd's native-wsbridge env (and standalone, for the tests)
#if !defined(ARCH_PORTDUINO) || defined(MESHTASTIC_WSBRIDGE)

#include "libpinedio-usb.h"
#include "protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define REPLY_TIMEOUT_MS 250

static int fd = -1;
static int wake_fd = -1;
static pthread_mutex_t io_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t irq_thread;
static bool irq_thread_running;
static atomic_bool irq_thread_exit;
static atomic_bool irq_pending;
static struct pinedio_inst *irq_inst;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_exact(uint8_t *p, size_t n, int64_t deadline)
{
    while (n) {
        int left = (int)(deadline - now_ms());
        if (left <= 0)
            return -ETIMEDOUT;
        struct pollfd pfd = {fd, POLLIN, 0};
        int r = poll(&pfd, 1, left);
        if (r < 0 && errno != EINTR)
            return -errno;
        if (r <= 0)
            continue;
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) && !(pfd.revents & POLLIN))
            return -ENODEV; // unplugged
        ssize_t got = read(fd, p, n);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -errno;
        }
        if (got == 0)
            return -EIO;
        p += got;
        n -= (size_t)got;
    }
    return 0;
}

static int write_all(const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -errno;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void note_irq(void)
{
    atomic_store(&irq_pending, true);
    uint64_t one = 1;
    if (write(wake_fd, &one, sizeof(one)) < 0) {
    }
}

// Reads frames until one with `want` arrives, noting any IRQ events on the way. Caller holds io_mutex.
static int read_reply(uint8_t want, uint8_t *payload, uint16_t maxlen, uint16_t *outlen)
{
    int64_t deadline = now_ms() + REPLY_TIMEOUT_MS;
    for (;;) {
        uint8_t sync;
        int r = read_exact(&sync, 1, deadline);
        if (r)
            return r;
        if (sync != WSB_SYNC)
            continue;
        uint8_t hdr[3];
        if ((r = read_exact(hdr, 3, deadline)))
            return r;
        uint16_t len = hdr[1] | (hdr[2] << 8);
        if (len > WSB_MAX_PAYLOAD)
            continue;
        uint8_t tmp[WSB_MAX_PAYLOAD];
        if ((r = read_exact(tmp, len, deadline)))
            return r;
        if (hdr[0] == WSB_EVT_IRQ) {
            note_irq();
            if (want == WSB_EVT_IRQ)
                return 0;
            continue;
        }
        if (hdr[0] != want)
            continue; // stale reply from a request that timed out
        if (len > maxlen)
            len = maxlen;
        memcpy(payload, tmp, len);
        if (outlen)
            *outlen = len;
        return 0;
    }
}

static int send_frame(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t frame[4 + WSB_MAX_PAYLOAD];
    frame[0] = WSB_SYNC;
    frame[1] = cmd;
    frame[2] = len & 0xFF;
    frame[3] = len >> 8;
    if (len)
        memcpy(frame + 4, data, len);
    return write_all(frame, 4u + len);
}

static int fail(struct pinedio_inst *inst, const char *what, int err)
{
    fprintf(stderr, "wsbridge: %s failed: %s\n", what, strerror(-err));
    inst->in_error = true;
    return -1;
}

// One SPI transfer, CS asserted around it when `select`. Longer than one frame: CS held over chunks.
static int32_t xfer(struct pinedio_inst *inst, uint8_t *wr, uint8_t *rd, uint32_t count, bool select)
{
    if (inst->in_error || fd < 0)
        return -1;
    pthread_mutex_lock(&io_mutex);
    int r = 0;
    if (count <= WSB_MAX_PAYLOAD) {
        uint8_t cmd = select ? WSB_CMD_XFER_CS : WSB_CMD_XFER;
        uint8_t in[WSB_MAX_PAYLOAD];
        if (wr)
            memcpy(in, wr, count);
        else
            memset(in, 0, count);
        if (!(r = send_frame(cmd, in, (uint16_t)count)))
            r = read_reply(WSB_REPLY(cmd), in, (uint16_t)count, NULL);
        if (!r && rd)
            memcpy(rd, in, count);
    } else {
        uint8_t lo[2] = {WSB_PIN_CS, 0}, hi[2] = {WSB_PIN_CS, 1};
        if (select)
            r = send_frame(WSB_CMD_WRITE, lo, 2);
        for (uint32_t off = 0; !r && off < count; off += WSB_MAX_PAYLOAD) {
            uint16_t n = (uint16_t)((count - off) < WSB_MAX_PAYLOAD ? (count - off) : WSB_MAX_PAYLOAD);
            uint8_t in[WSB_MAX_PAYLOAD];
            if (wr)
                memcpy(in, wr + off, n);
            else
                memset(in, 0, n);
            if (!(r = send_frame(WSB_CMD_XFER, in, n)))
                r = read_reply(WSB_REPLY(WSB_CMD_XFER), in, n, NULL);
            if (!r && rd)
                memcpy(rd + off, in, n);
        }
        if (select && !r)
            r = send_frame(WSB_CMD_WRITE, hi, 2);
    }
    pthread_mutex_unlock(&io_mutex);
    return r ? fail(inst, "SPI transfer", r) : (int32_t)count;
}

static void *irq_thread_fn(void *arg)
{
    (void)arg;
    while (!atomic_load(&irq_thread_exit)) {
        struct pollfd pfds[2] = {{fd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
        int r = poll(pfds, 2, 100);
        if (r < 0 && errno != EINTR)
            break;
        if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            // Unplugged: flag it so meshtasticd's recovery runs, and stop polling a dead fd
            fprintf(stderr, "wsbridge: device gone\n");
            if (irq_inst)
                irq_inst->in_error = true;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            uint64_t v;
            if (read(wake_fd, &v, sizeof(v)) < 0) {
            }
        }
        if (pfds[0].revents & POLLIN) {
            // Nobody else is reading while we hold the lock, so what's waiting can only be events
            pthread_mutex_lock(&io_mutex);
            struct pollfd p = {fd, POLLIN, 0};
            while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
                if (read_reply(WSB_EVT_IRQ, NULL, 0, NULL))
                    break;
            }
            pthread_mutex_unlock(&io_mutex);
        }
        if (atomic_exchange(&irq_pending, false)) {
            struct pinedio_inst *inst = irq_inst;
            void (*cb)(void) = inst ? inst->interrupts[WSB_PIN_IRQ].callback : NULL;
            if (cb)
                cb();
        }
    }
    return NULL;
}

static bool read_sysfs(const char *dir, const char *name, char *out, size_t outlen)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fgets(out, (int)outlen, f) != NULL;
    fclose(f);
    if (ok)
        out[strcspn(out, "\r\n")] = '\0';
    return ok;
}

// Finds /dev/ttyACM*/ttyUSB* on the USB device matching VID/PID (and serial suffix, when searched)
static bool find_port(struct pinedio_inst *inst, char *dev, size_t devlen, char *serial, size_t seriallen)
{
    const char *env = getenv("WSBRIDGE_PORT");
    if (env && *env) {
        snprintf(dev, devlen, "%s", env);
        serial[0] = '\0';
        return true;
    }
    DIR *d = opendir("/sys/class/tty");
    if (!d)
        return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, "ttyACM", 6) && strncmp(e->d_name, "ttyUSB", 6))
            continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/tty/%s/device", e->d_name);
        if (!realpath(link, real))
            continue;
        // device -> USB interface; its parent is the USB device with idVendor/idProduct
        char *slash = strrchr(real, '/');
        if (!slash)
            continue;
        *slash = '\0';
        char vid[16], pid[16], ser[64] = "";
        if (!read_sysfs(real, "idVendor", vid, sizeof(vid)) || !read_sysfs(real, "idProduct", pid, sizeof(pid)))
            continue;
        read_sysfs(real, "serial", ser, sizeof(ser));
        if (inst->options[PINEDIO_OPTION_VID] && strtoul(vid, NULL, 16) != inst->options[PINEDIO_OPTION_VID])
            continue;
        if (inst->options[PINEDIO_OPTION_PID] && strtoul(pid, NULL, 16) != inst->options[PINEDIO_OPTION_PID])
            continue;
        size_t sl = strlen(ser);
        const char *tail = sl > 8 ? ser + sl - 8 : ser;
        if (inst->options[PINEDIO_OPTION_SEARCH_SERIAL] && strncmp(tail, inst->serial_number, 8) != 0)
            continue;
        snprintf(dev, devlen, "/dev/%s", e->d_name);
        snprintf(serial, seriallen, "%s", tail);
        found = true;
    }
    closedir(d);
    return found;
}

int32_t pinedio_set_option(struct pinedio_inst *inst, enum pinedio_option option, uint32_t value)
{
    if (option >= PINEDIO_OPTION_MAX)
        return -1;
    inst->options[option] = value;
    return 0;
}

int32_t pinedio_init(struct pinedio_inst *inst, void *driver)
{
    (void)driver;
    inst->in_error = false;
    for (int i = 0; i < PINEDIO_INT_PIN_MAX; i++)
        inst->interrupts[i].callback = NULL;

    char dev[PATH_MAX], serial[16];
    if (!find_port(inst, dev, sizeof(dev), serial, sizeof(serial))) {
        fprintf(stderr, "wsbridge: no tty for USB %04x:%04x\n", inst->options[PINEDIO_OPTION_VID],
                inst->options[PINEDIO_OPTION_PID]);
        return -1;
    }
    fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "wsbridge: open %s: %s\n", dev, strerror(errno));
        return -1;
    }
    // Exclusive: Meshtastic apps probe serial ports, and their wake bytes would land in our stream
    if (ioctl(fd, TIOCEXCL) != 0)
        fprintf(stderr, "wsbridge: can't take %s exclusively: %s\n", dev, strerror(errno));
    struct termios tio;
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CRTSCTS | HUPCL);
    cfsetispeed(&tio, B2000000);
    cfsetospeed(&tio, B2000000);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio)) {
        fprintf(stderr, "wsbridge: can't set %s to %d baud: %s\n", dev, WSB_BAUD, strerror(errno));
        close(fd);
        fd = -1;
        return -1;
    }
    usleep(20000);
    tcflush(fd, TCIOFLUSH);

    uint8_t hello[16];
    uint16_t n = 0;
    int r = send_frame(WSB_CMD_HELLO, NULL, 0);
    if (!r)
        r = read_reply(WSB_REPLY(WSB_CMD_HELLO), hello, sizeof(hello), &n);
    if (r || n < 6 || memcmp(hello, "WSBR", 4) != 0) {
        fprintf(stderr, "wsbridge: %s did not answer as bridge firmware (%s)\n", dev, r ? strerror(-r) : "bad hello");
        close(fd);
        fd = -1;
        return -1;
    }
    fprintf(stderr, "wsbridge: %s, firmware v%u, %s\n", dev, hello[4], hello[5] ? "TCXO" : "XTAL");

    snprintf(inst->serial_number, sizeof(inst->serial_number), "%s", serial);
    snprintf(inst->product_string, sizeof(inst->product_string), "Waveshare USB-TO-LoRa (wsbridge)");

    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    irq_inst = inst;
    atomic_store(&irq_thread_exit, false);
    atomic_store(&irq_pending, false);
    irq_thread_running = pthread_create(&irq_thread, NULL, irq_thread_fn, NULL) == 0;
    return irq_thread_running ? 0 : -1;
}

void pinedio_deinit(struct pinedio_inst *inst)
{
    if (irq_thread_running) {
        atomic_store(&irq_thread_exit, true);
        uint64_t one = 1;
        if (write(wake_fd, &one, sizeof(one)) < 0) {
        }
        if (!pthread_equal(pthread_self(), irq_thread))
            pthread_join(irq_thread, NULL);
        irq_thread_running = false;
    }
    irq_inst = NULL;
    if (fd >= 0)
        close(fd);
    if (wake_fd >= 0)
        close(wake_fd);
    fd = wake_fd = -1;
    (void)inst;
}

static int32_t write_cmd(struct pinedio_inst *inst, uint8_t cmd, uint32_t pin, uint32_t value)
{
    if (inst->in_error || fd < 0)
        return -1;
    uint8_t p[2] = {(uint8_t)pin, (uint8_t)(value ? 1 : 0)};
    pthread_mutex_lock(&io_mutex);
    int r = send_frame(cmd, p, 2);
    pthread_mutex_unlock(&io_mutex);
    return r ? fail(inst, "pin write", r) : 0;
}

int32_t pinedio_set_pin_mode(struct pinedio_inst *inst, uint32_t pin, uint32_t mode)
{
    return write_cmd(inst, WSB_CMD_MODE, pin, mode);
}

int32_t pinedio_digital_write(struct pinedio_inst *inst, uint32_t pin, bool active)
{
    return write_cmd(inst, WSB_CMD_WRITE, pin, active);
}

int32_t pinedio_set_cs(struct pinedio_inst *inst, bool active)
{
    return pinedio_digital_write(inst, WSB_PIN_CS, !active);
}

int32_t pinedio_digital_read(struct pinedio_inst *inst, uint32_t pin)
{
    if (inst->in_error || fd < 0)
        return -1;
    uint8_t p = (uint8_t)pin, v = 0;
    pthread_mutex_lock(&io_mutex);
    int r = send_frame(WSB_CMD_READ, &p, 1);
    if (!r)
        r = read_reply(WSB_REPLY(WSB_CMD_READ), &v, 1, NULL);
    pthread_mutex_unlock(&io_mutex);
    return r ? fail(inst, "pin read", r) : v;
}

int32_t pinedio_get_irq_state(struct pinedio_inst *inst, uint32_t pin)
{
    return pinedio_digital_read(inst, pin);
}

int32_t pinedio_transceive(struct pinedio_inst *inst, uint8_t *write_buf, uint8_t *read_buf, uint32_t count)
{
    return xfer(inst, write_buf, read_buf, count, false);
}

int32_t pinedio_transceive_select(struct pinedio_inst *inst, uint8_t *write_buf, uint8_t *read_buf, uint32_t count)
{
    return xfer(inst, write_buf, read_buf, count, true);
}

int32_t pinedio_write_read(struct pinedio_inst *inst, uint8_t *writearr, uint32_t writecnt, uint8_t *readarr, uint32_t readcnt)
{
    uint32_t total = writecnt + readcnt;
    uint8_t *tmp = calloc(total ? total : 1, 1);
    if (!tmp)
        return -1;
    if (writecnt)
        memcpy(tmp, writearr, writecnt);
    int32_t r = xfer(inst, tmp, tmp, total, inst->options[PINEDIO_OPTION_AUTO_CS] != 0);
    if (r >= 0 && readcnt)
        memcpy(readarr, tmp + writecnt, readcnt);
    free(tmp);
    return r < 0 ? r : 0;
}

int32_t pinedio_attach_interrupt(struct pinedio_inst *inst, enum pinedio_int_pin int_pin, enum pinedio_int_mode int_mode,
                                 void (*callback)(void))
{
    // The firmware only reports DIO1's rising edge, which is all RadioLib asks for
    if (int_pin != WSB_PIN_IRQ || !(int_mode & PINEDIO_INT_MODE_RISING)) {
        fprintf(stderr, "wsbridge: only rising-edge interrupts on pin %d are supported\n", WSB_PIN_IRQ);
        return -1;
    }
    pthread_mutex_lock(&io_mutex);
    inst->interrupts[int_pin].mode = int_mode;
    inst->interrupts[int_pin].callback = callback;
    pthread_mutex_unlock(&io_mutex);
    atomic_store(&irq_pending, false); // edges from before the attach are not this callback's
    return 0;
}

int32_t pinedio_deattach_interrupt(struct pinedio_inst *inst, enum pinedio_int_pin int_pin)
{
    if (int_pin >= PINEDIO_INT_PIN_MAX)
        return -1;
    pthread_mutex_lock(&io_mutex);
    inst->interrupts[int_pin].callback = NULL;
    pthread_mutex_unlock(&io_mutex);
    return 0;
}

#endif
