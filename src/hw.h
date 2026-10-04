#ifndef HW_H
#define HW_H

#include <stdint.h>

/*
 * R2T2 FPGA access. Register map taken from the 2017 r2t2srv binary:
 *   0x53000000 + 4*rx        RX phase increment = f * 2^30 / clock
 *   0x50000040 + rx*0x10000  RX input select (0 = ANT1, 1 = ANT2, 2 = third input)
 *   0x50000000 + rx*0x10000  RX control, r2t2srv writes 2
 * Registers are write-only: reading invalid addresses raises a bus error.
 */

int hw_init(double clock);
void hw_set_freq(int rx, double hz);
void hw_set_input(int rx, int input);
void hw_set_gain(int adc, int db);
void hw_set_att(int adc, int db);
int hw_open_stream(const char *ifname);
unsigned hw_stream_read(void (*cb)(const uint8_t *frame, int len));
unsigned hw_stream_drops(int fd);
int hw_stream_clear_error(int fd);

#endif
