#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "hw.h"

#define ETH_P_R2T2   0x7232
#define REG_CTRL     0x50000000u
#define REG_RX_STEP  0x00010000u
#define REG_INPUT    0x40u
#define REG_FREQ     0x53000000u

static volatile uint32_t *ctrl_page[NRX];
static volatile uint32_t *freq_page;
static double hw_clock;

static volatile uint32_t *map_page(int fd, uint32_t addr)
{
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, addr & ~0xfffu);
    return p == MAP_FAILED ? NULL : (volatile uint32_t *)p;
}

int hw_init(double clock)
{
    hw_clock = clock;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return -1;
    }
    for (int rx = 0; rx < NRX; rx++) {
        ctrl_page[rx] = map_page(fd, REG_CTRL + rx * REG_RX_STEP);
        if (!ctrl_page[rx]) {
            perror("mmap rx control");
            return -1;
        }
    }
    freq_page = map_page(fd, REG_FREQ);
    if (!freq_page) {
        perror("mmap rx frequency");
        return -1;
    }
    close(fd);
    return 0;
}

void hw_set_freq(int rx, double hz)
{
    if (rx < 0 || rx >= NRX)
        return;
    if (hz < 0)
        hz = 0;
    if (hz > hw_clock / 2)
        hz = hw_clock / 2;
    freq_page[rx] = (uint32_t)(hz * 1073741824.0 / hw_clock + 0.5);
}

void hw_set_input(int rx, int input)
{
    if (rx < 0 || rx >= NRX)
        return;
    uint32_t v = (input >= 1 && input <= 3) ? (uint32_t)(input - 1) : 0x80000000u;
    ctrl_page[rx][REG_INPUT / 4] = v;
    ctrl_page[rx][0] = 2;
}

/* Gain and attenuator sit behind bit-banged GPIO; reuse the vendor tool. */
static void run_tool(const char *opt, int val)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "/usr/bin/r2t2 %s %d >/dev/null 2>&1", opt, val);
    if (system(cmd) != 0)
        fprintf(stderr, "hw: '%s' failed\n", cmd);
}

void hw_set_gain(int adc, int db)
{
    if (db < -9) db = -9;
    if (db > 32) db = 32;
    run_tool(adc ? "-k" : "-g", db);
}

void hw_set_att(int adc, int db)
{
    if (db < 0) db = 0;
    if (db > 31) db = 31;
    run_tool(adc ? "-j" : "-i", db);
}

/*
 * The FPGA delivers ~3500 frames/s. A TPACKET_V3 ring hands them over in
 * blocks, so the process wakes ~60 times per second instead of per frame
 * and no copy is needed.
 */
#define RING_BLOCK_SIZE (256 << 10)
#define RING_BLOCK_NR   32
#define RING_FRAME_SIZE (16 << 10)
#define RING_TIMEOUT_MS 10

static uint8_t *ring;
static unsigned ring_cur;

int hw_open_stream(const char *ifname)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_R2T2));
    if (fd < 0) {
        perror("socket AF_PACKET");
        return -1;
    }

    int ver = TPACKET_V3;
    struct tpacket_req3 req;
    memset(&req, 0, sizeof(req));
    req.tp_block_size = RING_BLOCK_SIZE;
    req.tp_block_nr = RING_BLOCK_NR;
    req.tp_frame_size = RING_FRAME_SIZE;
    req.tp_frame_nr = RING_BLOCK_SIZE / RING_FRAME_SIZE * RING_BLOCK_NR;
    req.tp_retire_blk_tov = RING_TIMEOUT_MS;
    if (setsockopt(fd, SOL_PACKET, PACKET_VERSION, &ver, sizeof(ver)) < 0 ||
        setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0) {
        perror("packet ring");
        close(fd);
        return -1;
    }
    ring = mmap(NULL, (size_t)RING_BLOCK_SIZE * RING_BLOCK_NR, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        perror("mmap packet ring");
        close(fd);
        return -1;
    }
    ring_cur = 0;

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_R2T2);
    sll.sll_ifindex = if_nametoindex(ifname);
    if (!sll.sll_ifindex || bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind stream interface");
        close(fd);
        return -1;
    }
    return fd;
}

/* Hand every frame of all completed blocks to cb (frame starts at the Ethernet header). */
unsigned hw_stream_read(void (*cb)(const uint8_t *frame, int len))
{
    unsigned frames = 0;
    for (;;) {
        struct tpacket_block_desc *bd = (struct tpacket_block_desc *)(ring + (size_t)ring_cur * RING_BLOCK_SIZE);
        if (!(bd->hdr.bh1.block_status & TP_STATUS_USER))
            break;
        __sync_synchronize();
        unsigned n = bd->hdr.bh1.num_pkts;
        struct tpacket3_hdr *ph = (struct tpacket3_hdr *)((uint8_t *)bd + bd->hdr.bh1.offset_to_first_pkt);
        for (unsigned i = 0; i < n; i++) {
            cb((const uint8_t *)ph + ph->tp_mac, (int)ph->tp_snaplen);
            ph = (struct tpacket3_hdr *)((uint8_t *)ph + ph->tp_next_offset);
        }
        frames += n;
        __sync_synchronize();
        bd->hdr.bh1.block_status = TP_STATUS_KERNEL;
        ring_cur = (ring_cur + 1) % RING_BLOCK_NR;
    }
    return frames;
}

/* Frames dropped by the kernel since the last call. */
unsigned hw_stream_drops(int fd)
{
    struct tpacket_stats_v3 st;
    socklen_t len = sizeof(st);
    if (getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &st, &len) < 0)
        return 0;
    return st.tp_drops;
}

/*
 * A packet socket keeps reporting POLLERR until its pending error is read
 * (e.g. ENETDOWN after rad0 went down and up once). Unread, poll() returns
 * at once and the main loop spins at 100 % CPU. Returns the error, 0 if none.
 */
int hw_stream_clear_error(int fd)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        return -1;
    if (!err) {
        /* error queue entries also raise POLLERR; drain them */
        char buf[256];
        while (recv(fd, buf, sizeof(buf), MSG_ERRQUEUE | MSG_DONTWAIT) >= 0)
            err = -2;
    }
    return err;
}
