#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

struct pwm_ioctl_t {
	int channel;
	int period;
	int duty;
	int polarity;
};

#define PWM_IOCTL_CONFIG     0x001
#define PWM_IOCTL_CONFIG_DUTY 0x002
#define PWM_IOCTL_ENABLE     0x010
#define PWM_IOCTL_DISABLE    0x100

int main(int argc, char **argv) {
	int on = (argc > 1 && strcmp(argv[1], "on") == 0);
	int fd = open("/dev/pwm", O_RDWR);
	if (fd < 0) { printf("open /dev/pwm failed: %s\n", strerror(errno)); return 1; }
	struct pwm_ioctl_t cfg = { .channel = 0, .duty = 0, .period = 1000000, .polarity = 1 };
	if (argc > 5) cfg.channel = atoi(argv[5]);
	int r;
	if (on) {
		if (argc > 2) cfg.period = atoi(argv[2]);
		if (argc > 3) cfg.duty = atoi(argv[3]);
		if (argc > 4) cfg.polarity = atoi(argv[4]);
		r = ioctl(fd, PWM_IOCTL_CONFIG, (unsigned long)&cfg);
		printf("config(p=%d d=%d pol=%d): %d errno=%d\n", cfg.period, cfg.duty, cfg.polarity, r, errno);
		r = ioctl(fd, PWM_IOCTL_ENABLE, cfg.channel);
		printf("enable: %d errno=%d\n", r, errno);
	} else {
		r = ioctl(fd, PWM_IOCTL_DISABLE, 0);
		printf("disable: %d\n", r);
	}
	close(fd);
	return 0;
}
