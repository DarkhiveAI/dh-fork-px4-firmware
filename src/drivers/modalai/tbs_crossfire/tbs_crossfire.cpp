/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <string>
#include <stdint.h>
#include <px4_platform_common/tasks.h>
#include <px4_platform_common/getopt.h>
#include <drivers/device/qurt/uart.h>
#include <uORB/uORB.h>
#include <uORB/Publication.hpp>
#include <uORB/PublicationMulti.hpp>
#include <uORB/topics/input_rc.h>
#include "uORB/uORBManager.hpp"
#include <uORB/topics/mavlink_msg.h>
#include <v2.0/standard/mavlink.h>
#include <px4_log.h>

#define ASYNC_UART_READ_WAIT_US 2000
#define RC_INPUT_RSSI_MAX	100

#define TX_BUFFER_LEN 20

extern "C" { __EXPORT int tbs_crossfire_main(int argc, char *argv[]); }

namespace tbs_crossfire
{

static bool _is_running = false;
volatile bool _task_should_exit = false;
static px4_task_t _task_handle = -1;
static px4_task_t _task_handle2 = -1;
int _uart_fd = -1;
bool debug = false;
std::string port = "7";
uint32_t baudrate = 115200;

uORB::Publication<mavlink_msg_s> _mav_rx_pub{ORB_ID(mavlink_rx_msg)};
uORB::PublicationMulti<input_rc_s> _rc_pub{ORB_ID(input_rc)};

int openPort(const char *dev, speed_t speed);
int closePort();

int writeResponse(void *buf, size_t len);

int start(int argc, char *argv[]);
int stop();
int status();

void usage();
void task_main(int argc, char *argv[]);

void handle_message_dsp(mavlink_message_t *msg);
void handle_message_rc_channels_override_dsp(mavlink_message_t *msg);

void handle_message_dsp(mavlink_message_t *msg) {
	if (debug) PX4_INFO("msg ID: %d", msg->msgid);
	switch (msg->msgid) {
	case MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE:
		handle_message_rc_channels_override_dsp(msg);
		break;
	default:
		// Send everything else off to the mavlink module
		mavlink_msg_s tmp_msg;
		tmp_msg.timestamp = hrt_absolute_time();
		tmp_msg.msg_len = mavlink_msg_to_send_buffer(tmp_msg.msg, msg);
		_mav_rx_pub.publish(tmp_msg);
		break;
	}
}

void task_main2(int argc, char *argv[])
{
    int mavlink_tx_msg_fd = orb_subscribe(ORB_ID(mavlink_tx_msg));
	(void) orb_set_queue_size(mavlink_tx_msg_fd, 255);
	struct mavlink_msg_s incoming_msg;
    bool updated = false;
	mavlink_message_t msg;
	mavlink_status_t status{};
	uint32_t copy_counter = 0;

	while ( ! _task_should_exit) {
		// Process any incoming mavlink messages for transmission
		int nwrite = 0;
		updated = false;
		copy_counter = 0;

        (void) orb_check(mavlink_tx_msg_fd, &updated);
		if (! updated) px4_usleep(100);
        while (updated) {
			orb_copy(ORB_ID(mavlink_tx_msg), mavlink_tx_msg_fd, &incoming_msg);
			for (int i = 0; i < incoming_msg.msg_len; i++) {
				if (mavlink_parse_char(MAVLINK_COMM_0, incoming_msg.msg[i], &msg, &status)) {
					if (msg.msgid == MAVLINK_MSG_ID_PARAM_VALUE) {
						mavlink_param_value_t value;
						mavlink_msg_param_value_decode(&msg, &value);
						PX4_INFO("%u Received PARAM_VALUE for index %u at %lu", copy_counter++, value.param_index, hrt_absolute_time());
					}
					break;
				}
			}

			nwrite = qurt_uart_write(_uart_fd, (char*) &incoming_msg.msg[0], incoming_msg.msg_len);
			if (nwrite != incoming_msg.msg_len) {
				PX4_INFO("Write failed. Expected %d, got %d", incoming_msg.msg_len, nwrite);
			}

        	(void) orb_check(mavlink_tx_msg_fd, &updated);
		}
	}
}

void task_main(int argc, char *argv[])
{
	int ch;
	int myoptind = 1;
	const char *myoptarg = nullptr;
	while ((ch = px4_getopt(argc, argv, "dp:b:", &myoptind, &myoptarg)) != EOF) {
		switch (ch) {
		case 'd':
			debug = true;
			PX4_INFO("Setting debug flag on");
			break;
		case 'p':
			port = myoptarg;
			if (debug) PX4_INFO("Setting port to %s", port.c_str());
			break;
		case 'b':
			baudrate = atoi(myoptarg);
			if (debug) PX4_INFO("Setting baudrate to %u", baudrate);
			break;
		default:
			break;
		}
	}

	if (openPort(port.c_str(), (speed_t) baudrate) == -1) {
		PX4_ERR("Failed to open UART");
		return;
	}

	_task_handle2 = px4_task_spawn_cmd("tbs_crossfire_2",
					  SCHED_DEFAULT,
					  SCHED_PRIORITY_DEFAULT,
					  2000,
					  (px4_main_t)&task_main2,
					  (char *const *)argv);

	if (_task_handle2 < 0) {
		PX4_ERR("task 2 start failed");
		return;
	}

	_is_running = true;

	uint8_t rx_buf[1024];
	mavlink_message_t msg;
	mavlink_status_t status{};

	while ( ! _task_should_exit) {

		// Check for incoming messages from the TBS Crossfire receiver
		int nread = qurt_uart_read(_uart_fd, (char*) rx_buf, sizeof(rx_buf), ASYNC_UART_READ_WAIT_US);
		if (nread) {
			// if (debug) PX4_INFO("Read %d bytes", nread);
			if (debug) PX4_INFO("%u %u", rx_buf[0], rx_buf[1]);
			//Take buffer and convert it into mavlink msg
			for (int i = 0; i <= nread; i++){
				if (mavlink_parse_char(MAVLINK_COMM_0, rx_buf[i], &msg, &status)) {
					handle_message_dsp(&msg);
				}
			}
		}

		px4_usleep(5000);
	}
}

void handle_message_rc_channels_override_dsp(mavlink_message_t *msg) {
	mavlink_rc_channels_override_t man;
	mavlink_msg_rc_channels_override_decode(msg, &man);

	if (debug) PX4_INFO("RC channels override msg received");

	// Check target
	if (man.target_system != 0) {
		PX4_ERR("Message has incorrect target system %u", man.target_system);
		return;
	}

	// fill uORB message
	input_rc_s rc{};

	// metadata
	rc.timestamp = hrt_absolute_time();
	rc.timestamp_last_signal = rc.timestamp;
	rc.rssi = RC_INPUT_RSSI_MAX;
	rc.rc_failsafe = false;
	rc.rc_lost = false;
	rc.rc_lost_frame_count = 0;
	rc.rc_total_frame_count = 1;
	rc.rc_ppm_frame_length = 0;
	rc.input_source = input_rc_s::RC_INPUT_SOURCE_MAVLINK;

	// channels
	rc.values[0] = man.chan1_raw;
	rc.values[1] = man.chan2_raw;
	rc.values[2] = man.chan3_raw;
	rc.values[3] = man.chan4_raw;
	rc.values[4] = man.chan5_raw;
	rc.values[5] = man.chan6_raw;
	rc.values[6] = man.chan7_raw;
	rc.values[7] = man.chan8_raw;
	rc.values[8] = man.chan9_raw;
	rc.values[9] = man.chan10_raw;
	rc.values[10] = man.chan11_raw;
	rc.values[11] = man.chan12_raw;
	rc.values[12] = man.chan13_raw;
	rc.values[13] = man.chan14_raw;
	rc.values[14] = man.chan15_raw;
	rc.values[15] = man.chan16_raw;
	rc.values[16] = man.chan17_raw;
	rc.values[17] = man.chan18_raw;

	// check how many channels are valid
	for (int i = 17; i >= 0; i--) {
		const bool ignore_max = rc.values[i] == UINT16_MAX; // ignore any channel with value UINT16_MAX
		const bool ignore_zero = (i > 7) && (rc.values[i] == 0); // ignore channel 8-18 if value is 0

		if (ignore_max || ignore_zero) {
			// set all ignored values to zero
			rc.values[i] = 0;
		} else {
			// first channel to not ignore -> set count considering zero-based index
			rc.channel_count = i + 1;
			break;
		}
	}

	// publish uORB message
	_rc_pub.publish(rc);
}

int openPort(const char *dev, speed_t speed)
{
	if (_uart_fd >= 0) {
		PX4_ERR("Port already in use: %s", dev);
		return -1;
	}

	_uart_fd = qurt_uart_open(dev, speed);

	if (_uart_fd < 0) {
		PX4_ERR("Error opening port: %s (%i)", dev, errno);
		return -1;
	} else if (debug) PX4_INFO("qurt uart opened successfully");

	return 0;
}

int closePort()
{
	_uart_fd = -1;

	return 0;
}

int writeResponse(void *buf, size_t len)
{
	if (_uart_fd < 0 || buf == NULL) {
		PX4_ERR("invalid state for writing or buffer");
		return -1;
	}

    return qurt_uart_write(_uart_fd, (const char*) buf, len);
}

int start(int argc, char *argv[])
{
	if (_is_running) {
		PX4_WARN("already running");
		return -1;
	}

	_task_should_exit = false;

	_task_handle = px4_task_spawn_cmd("tbs_crossfire_main",
					  SCHED_DEFAULT,
					  SCHED_PRIORITY_DEFAULT,
					  2000,
					  (px4_main_t)&task_main,
					  (char *const *)argv);

	if (_task_handle < 0) {
		PX4_ERR("task start failed");
		return -1;
	}

	return 0;
}

int stop()
{
	if (!_is_running) {
		PX4_WARN("not running");
		return -1;
	}

	_task_should_exit = true;

	while (_is_running) {
		usleep(200000);
		PX4_INFO(".");
	}

	_task_handle = -1;
	return 0;
}

int status()
{
	PX4_INFO("running: %s", _is_running ? "yes" : "no");

	return 0;
}

void
usage()
{
	PX4_INFO("Usage: tbs_crossfire {start|info|stop}");
}

}

int tbs_crossfire_main(int argc, char *argv[])
{
	int myoptind = 1;

	if (argc <= 1) {
		tbs_crossfire::usage();
		return -1;
	}

	const char *verb = argv[myoptind];

	if (!strcmp(verb, "start")) {
		return tbs_crossfire::start(argc - 1, argv + 1);
	} else if (!strcmp(verb, "stop")) {
		return tbs_crossfire::stop();
	} else if (!strcmp(verb, "status")) {
		return tbs_crossfire::status();
	} else {
		tbs_crossfire::usage();
		return -1;
	}
}