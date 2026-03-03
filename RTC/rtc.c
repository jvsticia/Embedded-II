/*
 * DS3231 RTC over I2C using bcm2835 on Raspberry Pi 4
 *
 * Commands:
 *   sudo ./rtc set    -> set RTC from Pi system time (12-hour format)
 *   sudo ./rtc read   -> read RTC and print
 *
 * Wiring:
 *   DS3231 VCC -> Pi 3V3 (Pin 1)
 *   DS3231 GND -> Pi GND (Pin 6)
 *   DS3231 SDA -> Pi SDA1 GPIO2 (Pin 3)
 *   DS3231 SCL -> Pi SCL1 GPIO3 (Pin 5)
 *
 * Compile:
 *   gcc -O2 -Wall -o rtc rtc.c -lbcm2835
 */

#include <bcm2835.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define DS3231_ADDR   0x68

// Time/date registers (spec says 0x00–0x06)
#define REG_SEC       0x00
#define REG_MIN       0x01
#define REG_HOUR      0x02
#define REG_DOW       0x03
#define REG_DATE      0x04
#define REG_MONTH     0x05
#define REG_YEAR      0x06

// Status register (for OSF flag)
#define REG_STATUS    0x0F

static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }
static uint8_t bcd2dec(uint8_t b) { return (uint8_t)(((b >> 4) * 10) + (b & 0x0F)); }

/*
 * Write len bytes starting at start_reg.
 * I2C on bus: START -> 0x68(W) -> start_reg -> data... -> STOP
 */
static int rtc_write_regs(uint8_t start_reg, const uint8_t *data, uint8_t len)
{
    uint8_t buf[32];
    if ((uint8_t)(len + 1) > sizeof(buf)) return -1;

    buf[0] = start_reg;
    for (uint8_t i = 0; i < len; i++) buf[i + 1] = data[i];

    return bcm2835_i2c_write((char *)buf, (uint32_t)(len + 1)); // 0 = success
}

/*
 * Read len bytes starting at start_reg.
 * I2C on bus: START -> 0x68(W) -> start_reg -> RESTART -> 0x68(R) -> data... -> STOP
 */
static int rtc_read_regs(uint8_t start_reg, uint8_t *data, uint8_t len)
{
    uint8_t buf[1];
    buf[0] = start_reg;

    return bcm2835_i2c_write_read_rs((char*)buf,1, (char*)data, len);
}

// Convert tm_wday (0=Sun..6=Sat) -> DS3231 DOW (1=Mon..7=Sun)
static uint8_t tm_to_dow_1_mon(const struct tm *t)
{
    if (t->tm_wday == 0) return 7;         // Sunday -> 7
    return (uint8_t)t->tm_wday;            // Monday=1..Saturday=6
}

// Build Hours register for 12-hour mode
static uint8_t build_hour_12h(uint8_t hour24)
{
    uint8_t pm = (hour24 >= 12) ? 1 : 0;
    uint8_t hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;

    // bit6=1 -> 12h mode
    // bit5=PM
    // bits4..0 = BCD hour (01..12)
    return (uint8_t)((1u << 6) | (pm ? (1u << 5) : 0) | (dec2bcd(hour12) & 0x1F));
}

static void print_time_regs(uint8_t regs[7])
{
    uint8_t sec = bcd2dec(regs[0] & 0x7F);
    uint8_t min = bcd2dec(regs[1] & 0x7F);

    uint8_t hr_reg = regs[2];
    uint8_t is12 = (hr_reg >> 6) & 0x01;

    uint8_t hour12 = 0;
    uint8_t pm = 0;

    if (is12) {
        pm = (hr_reg >> 5) & 0x01;
        hour12 = bcd2dec(hr_reg & 0x1F);   // 1..12
    } else {
        // fallback in case RTC is in 24-hour mode
        uint8_t hour24 = bcd2dec(hr_reg & 0x3F);
        pm = (hour24 >= 12);
        hour12 = hour24 % 12;
        if (hour12 == 0) hour12 = 12;
    }

    uint8_t dow  = bcd2dec(regs[3] & 0x07);
    uint8_t date = bcd2dec(regs[4] & 0x3F);
    uint8_t mon  = bcd2dec(regs[5] & 0x1F);
    uint8_t yr   = bcd2dec(regs[6]);

    static const char *dow_names[] = {"?", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const char *dow_str = (dow <= 7) ? dow_names[dow] : "?";

    printf("%s %02u/%02u/20%02u  %02u:%02u:%02u %s\n",
           dow_str, mon, date, yr, hour12, min, sec, pm ? "PM" : "AM");
}

static int rtc_set_from_system_time(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return -1;

    uint8_t regs[7];
    regs[0] = dec2bcd((uint8_t)t->tm_sec);
    regs[1] = dec2bcd((uint8_t)t->tm_min);
    regs[2] = build_hour_12h((uint8_t)t->tm_hour);
    regs[3] = dec2bcd(tm_to_dow_1_mon(t));
    regs[4] = dec2bcd((uint8_t)t->tm_mday);
    regs[5] = dec2bcd((uint8_t)(t->tm_mon + 1));
    regs[6] = dec2bcd((uint8_t)(t->tm_year % 100));

    int rc = rtc_write_regs(REG_SEC, regs, 7);
    if (rc != 0) return rc;

    // Clear OSF (Oscillator Stop Flag) bit7 in STATUS register
    uint8_t status = 0;
    rc = rtc_read_regs(REG_STATUS, &status, 1);
    if (rc == 0) {
        status &= ~(1u << 7);
        rtc_write_regs(REG_STATUS, &status, 1);
    }

    return 0;
}

static int rtc_read_and_print(void)
{
    uint8_t regs[7] = {0};
    int rc = rtc_read_regs(REG_SEC, regs, 7);
    if (rc != 0) return rc;

    print_time_regs(regs);
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  sudo %s set   - set RTC from system time (12-hour mode)\n", prog);
    printf("  sudo %s read  - read RTC and print\n", prog);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    if (!bcm2835_init()) {
        fprintf(stderr, "ERROR: bcm2835_init failed\n");
        return 1;
    }

    if (!bcm2835_i2c_begin()) {
        fprintf(stderr, "ERROR: bcm2835_i2c_begin failed\n");
        bcm2835_close();
        return 1;
    }

    bcm2835_i2c_setSlaveAddress(DS3231_ADDR);
    bcm2835_i2c_set_baudrate(100000); // 100 kHz for clean Saleae decoding

    int rc = 0;

    if (strcmp(argv[1], "set") == 0) {
        rc = rtc_set_from_system_time();
        if (rc == 0) {
            printf("RTC set from Pi system time.\n");
            // helpful: read immediately so you can confirm
            rtc_read_and_print();
        }
    } else if (strcmp(argv[1], "read") == 0) {
        rc = rtc_read_and_print();
    } else {
        usage(argv[0]);
        rc = 1;
    }

    if (rc != 0) {
        fprintf(stderr, "ERROR: operation failed (rc=%d)\n", rc);
    }

    bcm2835_i2c_end();
    bcm2835_close();
    return (rc == 0) ? 0 : 1;
}
