#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "driver/openvfd_drv.h"

#define UNUSED(x)	(void*)(x)
#define DRV_NAME	"/dev/" DEV_NAME
#define PIPE_PATH	"/tmp/" DEV_NAME "_service"

void select_display_type(void);
bool set_display_type(int new_display_type);
bool is_verbose(int argc, char *argv[]);
bool is_demo_mode(int argc, char *argv[]);
bool is_test_mode(int argc, char *argv[]);
bool is_12h_mode(int argc, char *argv[]);
int get_cmd_display_type(int argc, char *argv[]);
int get_cmd_chars_order(int argc, char *argv[], u_int8 chars[], const int sz);
bool print_usage(int argc, char *argv[]);
void update_secondary_clock(const struct tm *timenow);

struct sync_data {
	bool isActive;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct timespec abs_time;
	bool useBuffer;
	union {
		struct vfd_display_data display_data;
		char buffer[sizeof(struct vfd_display_data)];
	};
};

struct display_setup {
	bool is_demo;
	bool is_12h;
};

typedef struct _DotLedBitMap {
	uint8_t on;
	uint8_t bitmap;
}DotLedBitMap;

#define LED_MASK_VOID	0x00
static DotLedBitMap dotLeds[LED_DOT_MAX] = {
	{0, 0x01},
	{0, 0x02},
	{0, 0x04},
	{0, 0x08},
	{0, 0x10},
	{0, 0x20},
	{0, 0x40},
};

#define LEDCODES_LEN	(sizeof(LED_decode_tab1)/sizeof(LED_decode_tab1[0]))
static const led_bitmap *ledCodes = LED_decode_tab1;
static struct vfd_display display_type;

int openvfd_fd;
bool verbose = false;
static int secondary_green_on = 0;

uint8_t char_to_mask(uint8_t ch)
{
	unsigned int index = 0;
	if (display_type.controller > CONTROLLER_7S_MAX)
		return ch;
	else {
		for (index = 0; index < LEDCODES_LEN; index++) {
			if (ledCodes[index].character == ch) {
				return ledCodes[index].bitmap;
			}
		}
	}

	return LED_MASK_VOID;
}

void mdelay(int n)
{
	unsigned long msec=(n);
	while (msec--)
		usleep(1000);
}

struct sync_data sync_data;

void update_secondary_clock(const struct tm *timenow)
{
	static int last_minute = -1;
	static int last_colon = -1;
	static int last_green = -1;
	int colon_on;
	int packed;

	if (!timenow)
		return;

	/* Keep the secondary FD650 as a 24-hour local clock, independent of
	 * whatever is shown on the primary CIG20. */
	/* One-second phase shared with the primary CIG20 clock. */
	colon_on = ((timenow->tm_sec & 1) == 0);
	if (last_minute == timenow->tm_min && last_colon == colon_on &&
	    last_green == secondary_green_on)
		return;

	packed = (timenow->tm_min & 0xff) |
		 ((timenow->tm_hour & 0xff) << 8) |
		 ((colon_on & 0x01) << 16) |
		 ((secondary_green_on & 0x01) << 17);

	if (ioctl(openvfd_fd, VFD_IOC_SECONDARY_CLOCK, &packed) == 0) {
		last_minute = timenow->tm_min;
		last_colon = colon_on;
		last_green = secondary_green_on;
	} else if (verbose && errno != ENODEV && errno != ENOTTY) {
		perror("VFD_IOC_SECONDARY_CLOCK");
	}
}

void led_display_loop(const struct display_setup *setup)
{
	static struct vfd_display_data data;
	int ret = -1;

	time_t now;
	struct tm *timenow;

	memset(&data, 0, sizeof(data));
	while(sync_data.isActive) {
		if (!pthread_mutex_lock(&sync_data.mutex)) {
			ret = pthread_cond_timedwait(&sync_data.cond, &sync_data.mutex, &sync_data.abs_time);
			if (!ret || ret == ETIMEDOUT) {
				clock_gettime(CLOCK_REALTIME, &sync_data.abs_time);
				sync_data.abs_time.tv_nsec += (long)5E8;
				if (sync_data.abs_time.tv_nsec >= (long)1E9) {
					sync_data.abs_time.tv_nsec -= (long)1E9;
					sync_data.abs_time.tv_sec++;
				}

				select_display_type();

				/* Secondary FD650 is always a clock, even while the primary
				 * CIG20 is showing channel names or signal bars. */
				time(&now);
				timenow = localtime(&now);
				update_secondary_clock(timenow);

				if (sync_data.useBuffer && (sync_data.display_data.mode == DISPLAY_MODE_CLOCK ||
						sync_data.display_data.mode == DISPLAY_MODE_DATE)) {
					sync_data.useBuffer = false;
					sync_data.display_data.colon_on = data.colon_on;
					data = sync_data.display_data;
				}

				if (sync_data.useBuffer) {
					data = sync_data.display_data;
				} else {
					// Current local time was sampled above for both displays.
					if (setup->is_demo) {
						data.mode = 1 + timenow->tm_sec / 12;
						data.temperature = timenow->tm_hour + timenow->tm_min + timenow->tm_sec;
						data.channel_data.channel = (u_int16)10*(timenow->tm_hour + timenow->tm_min + timenow->tm_sec);
						data.channel_data.channel_count = (u_int16)86400;
						data.time_date.hours = ((timenow->tm_sec >= 24) && (timenow->tm_sec < 30)) ? 0 : (timenow->tm_hour == 0) ? 24 : timenow->tm_hour;
						data.time_date.minutes = timenow->tm_min;
						data.time_date.seconds = timenow->tm_sec;
						data.time_date.day_of_week = timenow->tm_wday;
						data.time_date.day = timenow->tm_mday;
						data.time_date.month = timenow->tm_mon;
						data.time_date.year = timenow->tm_year + 1900;
						data.time_secondary.hours = timenow->tm_hour;
						data.time_secondary.minutes = timenow->tm_min;
						data.time_secondary.seconds = timenow->tm_sec;
						data.colon_on = ((timenow->tm_sec & 1) == 0);
						// Really long movie title.
						snprintf(data.string_main, sizeof(data.string_secondary), "The Saga of the Viking Women and their Voyage to the Waters of the Great Sea Serpent");
						snprintf(data.string_secondary, sizeof(data.string_secondary), "Now playing:");
					} else {
						if (data.mode != DISPLAY_MODE_DATE)
							data.mode = DISPLAY_MODE_CLOCK;
						if (setup->is_12h) {
							if (timenow->tm_hour == 0)
								data.time_date.hours = 12;
							else if (timenow->tm_hour > 12)
								data.time_date.hours = timenow->tm_hour - 12;
							else
								data.time_date.hours = timenow->tm_hour;
						} else {
							data.time_date.hours = timenow->tm_hour;
						}
						data.time_date.minutes = timenow->tm_min;
						data.time_date.seconds = timenow->tm_sec;
						data.time_date.day_of_week = timenow->tm_wday;
						data.time_date.day = timenow->tm_mday;
						data.time_date.month = timenow->tm_mon;
						data.time_date.year = timenow->tm_year + 1900;
						data.colon_on = ((timenow->tm_sec & 1) == 0);
					}
				}
				ret = write(openvfd_fd,&data,sizeof(data));
			}
			pthread_mutex_unlock(&sync_data.mutex);
		} else {
			mdelay(500);
		}
	}
}

void led_test_codes()
{
	unsigned short write_buffer[7];
	unsigned char val = ':';
	unsigned int i = 0;

	// Test chart, sequence of numbers.
	for(i = 0; i < LEDCODES_LEN; i++) {
		val = ~val;
		write_buffer[1] = char_to_mask(ledCodes[i].character);
		write_buffer[2] = char_to_mask(ledCodes[i].character);
		write_buffer[3] = char_to_mask(ledCodes[i].character);
		write_buffer[4] = char_to_mask(ledCodes[i].character);
		write_buffer[0] = val;

		write(openvfd_fd,write_buffer,sizeof(write_buffer[0])*5);
		mdelay(500);
	}

	// Test bit sequence.
	for(i = 0; i < 10; i++) {
		val = ~val;
		write_buffer[1] = char_to_mask(1);
		write_buffer[2] = char_to_mask(2);
		write_buffer[3] = char_to_mask(3);
		write_buffer[4] = char_to_mask(4);
		write_buffer[0] = val;

		write(openvfd_fd,write_buffer,sizeof(write_buffer[0])*5);
		mdelay(500);
	}

	// Test sequence 2
	write_buffer[0] = 0;
	for(i = 0; i < LED_DOT_MAX; i++){
		val = ~val;
		write_buffer[1] = char_to_mask(5);
		write_buffer[2] = char_to_mask(6);
		write_buffer[3] = char_to_mask(7);
		write_buffer[4] = char_to_mask(8);
		write_buffer[0] &= dotLeds[i%LED_DOT_MAX].bitmap;

		write(openvfd_fd,write_buffer,sizeof(write_buffer[0])*5);
		mdelay(500);
	}

	// Test sequence 3
	write_buffer[0] = 0;
	for(i = 0; i < LED_DOT_MAX; i++){
		val = ~val;
		write_buffer[1] = char_to_mask(6);
		write_buffer[2] = char_to_mask(7);
		write_buffer[3] = char_to_mask(8);
		write_buffer[4] = char_to_mask(9);
		write_buffer[0] |= dotLeds[i%LED_DOT_MAX].bitmap;

		write(openvfd_fd,write_buffer,sizeof(write_buffer[0])*5);
		mdelay(500);
	}
}

void led_test_loop(bool cycle_display_types)
{
	int current_type = DISPLAY_TYPE_5D_7S_NORMAL;
	int transposed = 0;
	const pid_t pid = getpid();
	printf("Initializing...\n");
	if (!cycle_display_types)
		printf("Process ID = %d\n", pid);
	while (1) {
		int i;
		const int len = 7;
		unsigned short wb[7];
		const size_t sz = sizeof(wb[0])*len;

		if (cycle_display_types) {
			printf("Process ID = %d\n", pid);
			++current_type;
			current_type %= DISPLAY_TYPE_MAX;
			if (!current_type)
				transposed = (~transposed & DISPLAY_FLAG_TRANSPOSED_INT);
			printf("Set display type to 0x%08X\n", current_type | transposed);
			set_display_type(current_type | transposed);
			select_display_type();
		}

		// Light up all sections and cycle
		// through display brightness levels.
		memset(wb, 0xFF, sz);
		write(openvfd_fd,wb,sz);
		for (i = FD628_Brightness_1; i <= FD628_Brightness_8; i++) {
			ioctl(openvfd_fd, VFD_IOC_SBRIGHT, &i);
			mdelay(1000);
		}

		// Clear display for a second.
		memset(wb, 0x00, sz);
		write(openvfd_fd,wb,sz);
		mdelay(1000);

		// Run original test codes.
		led_test_codes();

		// Cycle through fully lit characters.
		for (i = 0; i < len; i++) {
			memset(wb, 0x00, sz);
			wb[i] = 0xFF;
			write(openvfd_fd,wb,sz);
			mdelay(1000);
		}

		// Cycle through bits in each character.
		for (i = 0; i < len; i++) {
			memset(wb, (1 << i), sz);
			write(openvfd_fd,wb,sz);
			mdelay(1000);
		}
	}
}

void *display_thread_handler(void *arg)
{
	struct display_setup *setup = (struct display_setup*)arg;
	led_display_loop(setup);
	pthread_exit(NULL);
}

void *display_test_thread_handler(void *arg)
{
	bool cycle_display_types = *(bool*)arg;
	led_test_loop(cycle_display_types);
	pthread_exit(NULL);
}

void *named_pipe_thread_handler(void *arg)
{
	int file;
	char buf[1024];
	int ret = 0, i;
	unsigned char skipSignal;

	unlink(PIPE_PATH);
	if ((mkfifo(PIPE_PATH, 0666)) != 0) {
		printf("Unable to create a fifo; errno=%d\n",errno);
		pthread_exit(NULL);                    /* Print error message and return */
	}

	while (sync_data.isActive) {
		file = open(PIPE_PATH, O_RDONLY);
		ret = read(file, buf, sizeof(buf) - 1);
		close(file);
		if (ret < 0)
			ret = 0;
		buf[ret] = '\0';
		if (verbose) {
			printf("ret = %d, %s\n", ret, buf);
			for (i = 0; i < ret; i++)
				printf("0x%02X, ", buf[i]);
			printf("\n");
		}
		if (ret > 0 && !pthread_mutex_lock(&sync_data.mutex)) {
			skipSignal = 0;

			/*
			 * Text commands are intentionally simple so Enigma2 plugins and
			 * shell scripts do not need to reproduce struct vfd_display_data.
			 *
			 *   channel=123   -> DISPLAY_MODE_CHANNEL
			 *   channel 123   -> DISPLAY_MODE_CHANNEL
			 *   title=BBC NEWS -> DISPLAY_MODE_TITLE (CIG20 scrolls long names)
			 *   green=0/1     -> secondary FD650 green signal indicator
			 *   clock         -> return to the normal clock display
			 */
			if (!strncmp(buf, "channel=", 8) || !strncmp(buf, "channel ", 8)) {
				char *end = NULL;
				long channel = strtol(buf + 8, &end, 10);

				if (end != (buf + 8) && channel > 0 && channel <= 65535) {
					sync_data.display_data.mode = DISPLAY_MODE_CHANNEL;
					sync_data.display_data.channel_data.channel = (u_int16)channel;
					sync_data.display_data.channel_data.channel_count = 0;
					sync_data.display_data.colon_on = 0;
					sync_data.useBuffer = true;
					if (verbose)
						printf("Display channel %ld\n", channel);
				} else {
					if (verbose)
						printf("Invalid channel command: %s\n", buf);
					skipSignal = 1;
				}
			} else if (!strncmp(buf, "title=", 6) || !strncmp(buf, "title ", 6)) {
				char *title = buf + 6;
				size_t len = strcspn(title, "\r\n");

				title[len] = '\0';
				if (*title) {
					sync_data.display_data.mode = DISPLAY_MODE_TITLE;
					snprintf(sync_data.display_data.string_main,
						 sizeof(sync_data.display_data.string_main), "%s", title);
					sync_data.display_data.colon_on = 0;
					sync_data.useBuffer = true;
					if (verbose)
						printf("Display title: %s\n", title);
				} else {
					skipSignal = 1;
				}
			} else if (!strncmp(buf, "snr=", 4) || !strncmp(buf, "snr ", 4) ||
			           !strncmp(buf, "signal=", 7) || !strncmp(buf, "signal ", 7)) {
				char *value = (!strncmp(buf, "snr", 3)) ? buf + 4 : buf + 7;
				char *end = NULL;
				long percent = strtol(value, &end, 10);

				if (end != value) {
					int level;
					time_t green_now;
					if (percent < 0) percent = 0;
					if (percent > 100) percent = 100;
					level = (int)percent;
					if (ioctl(openvfd_fd, VFD_IOC_SIGNAL_LEVEL, &level) && verbose)
						perror("VFD_IOC_SIGNAL_LEVEL");
					else if (verbose)
						printf("CIG20 signal bars %d%%\n", level);

					/*
					 * Tiger Dual: make the secondary FD650 green indicator
					 * follow the exact same live signal value that drives the
					 * CIG20 bar graph.  This removes the separate green-command
					 * timing/state dependency: signal > 0 => ON, signal == 0 => OFF.
					 */
					secondary_green_on = level > 0 ? 1 : 0;
					green_now = time(NULL);
					update_secondary_clock(localtime(&green_now));
					if (verbose)
						printf("Secondary green follows signal: %s (%d%%)\n",
						       secondary_green_on ? "on" : "off", level);
				} else if (verbose) {
					printf("Invalid signal command: %s\n", buf);
				}
				skipSignal = 1;
			} else if (!strncmp(buf, "green=", 6) || !strncmp(buf, "green ", 6)) {
				char *end = NULL;
				long state = strtol(buf + 6, &end, 10);
				if (end != (buf + 6)) {
					secondary_green_on = state ? 1 : 0;
					/* Force the next secondary refresh without changing CIG20 mode. */
					update_secondary_clock(localtime(&(time_t){time(NULL)}));
					if (verbose) printf("Secondary green signal %s\n", secondary_green_on ? "on" : "off");
				} else {
					skipSignal = 1;
				}
				skipSignal = 1;
			} else if (!strncmp(buf, "clock", 5)) {
				sync_data.display_data.mode = DISPLAY_MODE_CLOCK;
				sync_data.useBuffer = false;
				if (verbose)
					printf("Return display to clock mode\n");
			} else if (ret == sizeof(sync_data.display_data)) {
				if (verbose)
					printf("Write display data\n");
				memcpy(&sync_data.display_data, buf, sizeof(sync_data.display_data));
				sync_data.useBuffer = true;
			} else {
				if (verbose)
					printf("Write unknown data\n");
				switch ((unsigned char)buf[0]) {
				case 0:
				default:
					if (verbose)
						printf("case 0, default\n");
					sync_data.useBuffer = true;
					sync_data.display_data.mode = DISPLAY_MODE_CLOCK;
					break;
				case 1:
					// Refresh display. Will signal the led_loop to update display.
					break;
				case 2:
					if (ret >= 3 && buf[1] == DISPLAY_MODE_DATE)
					{
						if (sync_data.display_data.mode == DISPLAY_MODE_DATE)
							skipSignal = 1;
						else
							sync_data.display_data.mode = DISPLAY_MODE_DATE;
						sync_data.display_data.time_secondary._reserved = buf[2];
						sync_data.useBuffer = true;
					}
					break;
				}
			}
			if (!skipSignal)
				pthread_cond_signal(&sync_data.cond);
			pthread_mutex_unlock(&sync_data.mutex);
		}
	}

	unlink(PIPE_PATH);
	pthread_exit(NULL);
}

void select_display_type()
{
	if (!ioctl(openvfd_fd, VFD_IOC_GDISPLAY_TYPE, &display_type)) {
		switch(display_type.type) {
			case DISPLAY_TYPE_5D_7S_T95:
				ledCodes = LED_decode_tab1;
				break;
			case DISPLAY_TYPE_5D_7S_G9SX:
				ledCodes = LED_decode_tab3;
				break;
			case DISPLAY_TYPE_4D_7S_FREESATGTC:
				ledCodes = LED_decode_tab4;
				break;
			case DISPLAY_TYPE_5D_7S_TAP1:
				ledCodes = LED_decode_tab5;
				break;
			default:
				ledCodes = LED_decode_tab2;
				break;
		}
	} else {
		memset(&display_type, 0, sizeof(display_type));
		perror("Failed to read display type, using default.");
	}
}

bool set_display_type(int new_display_type)
{
	long ret = ioctl(openvfd_fd, VFD_IOC_SDISPLAY_TYPE, &new_display_type);
	if (ret) {
		printf("Failed setting a new display type.\n");
		if (ret == ERANGE)
			printf("Unsupported display type. (out of range)\n");
	}

	return ret == 0;
}

void handle_signal(int signal)
{
	int file;
	sync_data.isActive = false;
	file = open(PIPE_PATH, O_WRONLY);
	write(file, "\1", 1);
	close(file);
}

int main(int argc, char *argv[])
{
	u_int8 char_indexes[7];
	int ret, type, char_order_count;
	bool test_mode = false;
	bool cycle_display_types = true;
	pthread_t disp_id, npipe_id = 0;

	if (print_usage(argc, argv))
		return 0;
	openvfd_fd = open(DRV_NAME, O_RDWR);
	if (openvfd_fd < 0) {
		perror("Open device failed.\n");
		exit(1);
	}

	verbose = is_verbose(argc, argv);
	char_order_count = get_cmd_chars_order(argc, argv, char_indexes, (int)sizeof(char_indexes));
	if (char_order_count)
		if (ioctl(openvfd_fd, VFD_IOC_SCHARS_ORDER, char_indexes))
			printf("Error setting new character order.\n");

	type = get_cmd_display_type(argc, argv);
	if (type >= 0)
		cycle_display_types = !set_display_type(type);
	select_display_type();

	test_mode = is_test_mode(argc, argv);
	if (test_mode)
		ret = pthread_create(&disp_id, NULL, display_test_thread_handler, &cycle_display_types);
	else {
		struct display_setup setup = { 0 };
		struct sigaction sig_handler = {.sa_handler=handle_signal};
		memset(&sync_data, 0, sizeof(struct sync_data));
		sync_data.isActive = true;
		sigaction(SIGTERM, &sig_handler, 0);
		sigaction(SIGINT, &sig_handler, 0);
		setup.is_demo = is_demo_mode(argc, argv);
		setup.is_12h = is_12h_mode(argc, argv);
		ret = pthread_create(&disp_id, NULL, display_thread_handler, &setup);
		if (ret == 0)
			ret = pthread_create(&npipe_id, NULL, named_pipe_thread_handler, NULL);
	}
	if(ret != 0) {
		perror("Create disp_id or npipe_id thread error\n");
		return ret;
	}

	if (npipe_id)
		pthread_join(npipe_id, NULL);
	pthread_join(disp_id, NULL);
	close(openvfd_fd);
	return 0;
}

bool is_cmd_option(int argc, char *argv[], const char *str)
{
	bool ret = false;
	int i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], str)) {
			ret = true;
			break;
		}
	}

	return ret;
}

bool is_verbose(int argc, char *argv[])
{
	return is_cmd_option(argc, argv, "-v");
}

bool is_demo_mode(int argc, char *argv[])
{
	return is_cmd_option(argc, argv, "-dm");
}

bool is_test_mode(int argc, char *argv[])
{
	return is_cmd_option(argc, argv, "-t");
}

bool is_12h_mode(int argc, char *argv[])
{
	return is_cmd_option(argc, argv, "-12h");
}

int get_cmd_display_type(int argc, char *argv[])
{
	int ret = -1, i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-dt")) {
			if (argc >= (i + 2)) {
				long temp = -1;
				char *start, *end;
				start = !strncmp(argv[i+1], "0x", 2) ? argv[i+1] + 2 : argv[i+1];
				temp = strtol(start, &end, 0x10);
				if (end == start || *end != '\0' || errno == ERANGE) {
					printf("Error parsing display type index.\n");
				} else {
					ret = (int)temp;
					printf("Display type 0x%08X\n", ret);
				}
			} else {
				printf("Error parsing display type index, missing argument.\n");
			}
			break;
		}
	}

	return ret;
}

int get_cmd_chars_order(int argc, char *argv[], u_int8 chars[], const int sz)
{
	int ret = 0, i, j;
	for (i = 0; i < sz; i++)
		chars[i] = i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-co")) {
			for (i++, j = 0; i < argc && j < sz; i++) {
				long temp = -1;
				char *end;
				temp = strtol(argv[i], &end, 10);
				if (end == argv[i] || *end != '\0' || errno == ERANGE)
					break;
				else if (temp >= 0 && temp < sz)
					chars[j++] = temp;
			}

			ret = j;
			break;
		}
	}

	if (ret) {
		printf("Got %d char indexes.\n", ret);
		for (i = 0; i < ret; i++) {
			printf("index[%d] = %d\n", i, chars[i]);
		}
	}

	return ret;
}

bool print_usage(int argc, char *argv[])
{
	bool ret = false;
	int i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			ret = true;
			printf("\nUsage: OpenVFDService [-t] [-dt TYPE] [-h]\n\n");
			printf("\t-t\t\tRun OpenVFDService in display test mode.\n");
			printf("\t-dm\t\tRun OpenVFDService in display demo mode.\n");
			printf("\t-dt N\t\tSpecifies which display type to use.\n");
			printf("\t-co N...\t< D HH:MM > Order of display chars.\n\t\t\tValid values are 0 - 6.\n\t\t\t(D=dots, represented by a single char)\n");
			printf("\t-h\t\tThis text.\n\n");
		}
	}

	return ret;
}
