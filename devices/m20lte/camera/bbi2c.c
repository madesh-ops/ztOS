/*
 * bbi2c: I2C by bit-banging two GPIOs (libgpiod 2), for the Galaxy M20's camera
 * buses before the kernel has drivers for them. Open drain by hand: a line is
 * driven low as an output, released as an input (the board pulls it up).
 *
 *   bbi2c CHIP SDA SCL ADDR r REG [COUNT]      read COUNT registers from REG
 *   bbi2c CHIP SDA SCL ADDR w REG VAL [VAL...] write
 *   bbi2c CHIP SDA SCL ADDR r16 REG16 [COUNT]  16-bit register address (sensors)
 *   bbi2c CHIP SDA SCL scan                    which addresses ACK
 *
 * ADDR is 7-bit. About 50 kHz; no clock stretching beyond a short wait.
 */
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct gpiod_line_request *rq_sda, *rq_scl;
static unsigned int o_sda, o_scl;

static void delay(void)
{
	struct timespec t = { 0, 8000 };
	nanosleep(&t, NULL);
}

static struct gpiod_line_request *req(struct gpiod_chip *c, unsigned int off)
{
	struct gpiod_line_settings *s = gpiod_line_settings_new();
	struct gpiod_line_config *lc = gpiod_line_config_new();
	struct gpiod_request_config *rc = gpiod_request_config_new();
	gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_INPUT);
	gpiod_line_config_add_line_settings(lc, &off, 1, s);
	gpiod_request_config_set_consumer(rc, "bbi2c");
	struct gpiod_line_request *r = gpiod_chip_request_lines(c, rc, lc);
	gpiod_line_settings_free(s);
	gpiod_line_config_free(lc);
	gpiod_request_config_free(rc);
	return r;
}

/* low: drive 0; high: release (input) */
static void set(struct gpiod_line_request *r, unsigned int off, int high)
{
	struct gpiod_line_settings *s = gpiod_line_settings_new();
	struct gpiod_line_config *lc = gpiod_line_config_new();
	if (high) {
		gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_INPUT);
	} else {
		gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_OUTPUT);
		gpiod_line_settings_set_output_value(s, GPIOD_LINE_VALUE_INACTIVE);
	}
	gpiod_line_config_add_line_settings(lc, &off, 1, s);
	gpiod_line_request_reconfigure_lines(r, lc);
	gpiod_line_settings_free(s);
	gpiod_line_config_free(lc);
}

static int get(struct gpiod_line_request *r, unsigned int off)
{
	return gpiod_line_request_get_value(r, off) == GPIOD_LINE_VALUE_ACTIVE;
}

static void sda(int v) { set(rq_sda, o_sda, v); }
static void scl(int v)
{
	set(rq_scl, o_scl, v);
	if (v)
		for (int i = 0; i < 100 && !get(rq_scl, o_scl); i++)
			delay();
}

static void start(void) { sda(1); scl(1); delay(); sda(0); delay(); scl(0); delay(); }
static void stop(void) { sda(0); delay(); scl(1); delay(); sda(1); delay(); }

static int wbyte(int b)
{
	for (int i = 7; i >= 0; i--) {
		sda((b >> i) & 1); delay();
		scl(1); delay(); scl(0);
	}
	sda(1); delay();
	scl(1); delay();
	int nack = get(rq_sda, o_sda);
	scl(0); delay();
	return !nack;
}

static int rbyte(int ack)
{
	int b = 0;
	sda(1);
	for (int i = 0; i < 8; i++) {
		delay(); scl(1); delay();
		b = (b << 1) | get(rq_sda, o_sda);
		scl(0);
	}
	sda(!ack); delay(); scl(1); delay(); scl(0); sda(1); delay();
	return b;
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: bbi2c CHIP SDA SCL ADDR r REG [N] | w REG VAL.. | r16 REG16 [N] | scan\n");
		return 2;
	}
	struct gpiod_chip *c = gpiod_chip_open(argv[1]);
	if (!c) { perror(argv[1]); return 1; }
	o_sda = strtoul(argv[2], 0, 0);
	o_scl = strtoul(argv[3], 0, 0);
	rq_sda = req(c, o_sda);
	rq_scl = req(c, o_scl);
	if (!rq_sda || !rq_scl) { perror("request"); return 1; }
	/* bus recovery: a device left mid-byte holds SDA low; clock it out */
	for (int i = 0; i < 9 && get(rq_scl, o_scl) && !get(rq_sda, o_sda); i++) {
		scl(0); delay(); scl(1); delay();
	}
	if (get(rq_sda, o_sda) && get(rq_scl, o_scl))
		stop();
	if (!get(rq_sda, o_sda) || !get(rq_scl, o_scl)) {
		fprintf(stderr, "bus not idle (SDA %d SCL %d): no pull-up or a device holds it\n",
			get(rq_sda, o_sda), get(rq_scl, o_scl));
		return 1;
	}
	if (!strcmp(argv[4], "scan")) {
		for (int a = 0x08; a < 0x78; a++) {
			start();
			int ok = wbyte(a << 1);
			stop();
			if (ok)
				printf("0x%02x ACK\n", a);
		}
		return 0;
	}
	int addr = strtol(argv[4], 0, 0);
	if (argc < 7) return 2;
	char *op = argv[5];
	int reg = strtol(argv[6], 0, 0), rc = 0;
	if (!strcmp(op, "r") || !strcmp(op, "r16")) {
		int n = argc > 7 ? atoi(argv[7]) : 1;
		start();
		if (!wbyte(addr << 1)) { stop(); fprintf(stderr, "0x%02x: no ACK\n", addr); return 1; }
		if (!strcmp(op, "r16"))
			wbyte(reg >> 8);
		wbyte(reg & 0xff);
		start();
		wbyte((addr << 1) | 1);
		for (int i = 0; i < n; i++)
			printf("0x%0*x: 0x%02x\n", strcmp(op, "r16") ? 2 : 4, reg + i, rbyte(i < n - 1));
		stop();
	} else if (!strcmp(op, "w")) {
		start();
		if (!wbyte(addr << 1)) { stop(); fprintf(stderr, "0x%02x: no ACK\n", addr); return 1; }
		wbyte(reg);
		for (int i = 7; i < argc; i++)
			if (!wbyte(strtol(argv[i], 0, 0))) { rc = 1; fprintf(stderr, "NACK on byte %d\n", i - 7); break; }
		stop();
	} else {
		return 2;
	}
	return rc;
}
