/*
  ===========================================================================

  limesdr_linrad - Streams RX data from a LimeSDR to Linrad and accepts
  TX data from a FIFO.

  Copyright (C) 2019 Daniel Estevez <daniel@destevez.net>
  
  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <https://www.gnu.org/licenses/>.

  This program is loosely based on LIMESDR_TOOLBOX by Emvivre. 

  ===========================================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <time.h>

#include <unistd.h>

#include <lime/LimeSuite.h>

#define LINRAD_NET_MULTICAST_PAYLOAD 1392
#define LINRAD_SAMPLES_PER_PACKET (LINRAD_NET_MULTICAST_PAYLOAD/(sizeof(int16_t) * 2))

#define LINRAD_BUFSIZE 4096
#define LINRAD_BASE_PORT 50100

struct linrad_udp_packet {
	double passband_center;
	int32_t time;
	float userx_freq;
	uint32_t ptr;
	uint16_t block_no;
	int8_t userx_no;
	int8_t passband_direction;
	char buffer[LINRAD_NET_MULTICAST_PAYLOAD];
};

void init_linrad_header(struct linrad_udp_packet *p, double passband_center) {
	memset(p, 0, sizeof(*p));
	p->passband_center = passband_center;
	p->userx_no = -1;
	p->passband_direction = 1;
	p->ptr = LINRAD_NET_MULTICAST_PAYLOAD;
}

int linrad_header_fill_time(struct linrad_udp_packet *p) {
	struct timespec t;

	if (clock_gettime(CLOCK_REALTIME, &t) == -1) return -1;
	p->time = t.tv_sec * 1000 + t.tv_nsec / 1000000;

	return 0;
}

void next_linrad_header(struct linrad_udp_packet *p) {
	p->ptr = (p->ptr + LINRAD_NET_MULTICAST_PAYLOAD) % LINRAD_BUFSIZE;
	p->block_no++;
}

int open_linrad_udp_socket(int *sock, struct sockaddr_in *sockaddr, const char *ip) {
	*sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (*sock < 0) return -1;
	
	memset(sockaddr, 0, sizeof(*sockaddr));
	sockaddr->sin_family = AF_INET;
	sockaddr->sin_port = htons(LINRAD_BASE_PORT);
	sockaddr->sin_addr.s_addr = inet_addr(ip);

	return 0;
}

int limesdr_open(unsigned int device_i, lms_device_t **device) {
	int device_count = LMS_GetDeviceList(NULL);
	if (device_count < 0) {
		fprintf(stderr, "LMS_GetDeviceList() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}
	lms_info_str_t device_list[device_count];
	if (LMS_GetDeviceList(device_list) < 0) {
		fprintf(stderr, "LMS_GetDeviceList() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}

	if (LMS_Open(device, device_list[device_i], NULL) < 0) {
		fprintf(stderr, "LMS_Open() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}

	const lms_dev_info_t *device_info;

	device_info = LMS_GetDeviceInfo(*device);
	if (device_info != NULL) {
		fprintf(stderr, "%s Library %s Firmware %s Gateware %s ", device_info->deviceName, LMS_GetLibraryVersion(), device_info->firmwareVersion, device_info->gatewareVersion);
		float_type Temp;
		LMS_GetChipTemperature(*device, 0, &Temp);
		fprintf(stderr, "Temperature %.2f\n", Temp);
	}
	else {
		fprintf(stderr, "LMS_GetDeviceInfo() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}

	if (LMS_Reset(*device) < 0) {
		fprintf(stderr, "LMS_Reset() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}
	if (LMS_Init(*device) < 0) {
		fprintf(stderr, "LMS_Init() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}

	return 0;
}

int limesdr_enable_channels(lms_device_t *device,
			    unsigned int in_channel, unsigned int out_channel) {
	if (LMS_EnableChannel(device, LMS_CH_TX, out_channel, true) < 0) {
		fprintf(stderr, "LMS_EnableChannel() (TX) : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}

	if (LMS_EnableChannel(device, LMS_CH_RX, in_channel, true) < 0) {
		fprintf(stderr, "LMS_EnableChannel() (RX) : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}

	return 0;
}

int limesdr_set_sample_rate(lms_device_t *device, double sample_rate, double *host_sample_rate) {
	int oversample = 0;
	
	if (LMS_SetSampleRate(device, sample_rate, oversample) < 0) {
		fprintf(stderr, "LMS_SetSampleRate() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}
		
	if (LMS_GetSampleRate(device, LMS_CH_TX, 0, host_sample_rate, NULL) < 0) {
		fprintf(stderr, "Warning : LMS_GetSampleRate() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}

	return 0;
}

int limesdr_set_frequency(lms_device_t *device, int is_tx, unsigned int channel,
			  double freq, double if_freq, double lpf_bw) {
	if (LMS_SetLOFrequency(device, is_tx, channel, freq - if_freq) < 0) {
		fprintf(stderr, "LMS_SetLOFrequency() : %s\n", LMS_GetLastErrorMessage());
		return -1;
	}
	if (if_freq) {
		float_type nco_freqs[16] = {if_freq, 0};
		if (LMS_SetNCOFrequency(device, is_tx, channel, nco_freqs, 0.0) < 0) {
			fprintf(stderr, "LMS_SetNCOFrequency() : %s\n", LMS_GetLastErrorMessage());
			return -1;
		}
		int downconvert = !is_tx;
		if (LMS_SetNCOIndex(device, is_tx, channel, 0, downconvert) < 0) {
			fprintf(stderr, "LMS_SetNCOIndex() : %s\n", LMS_GetLastErrorMessage());
			return -1;
		}
	}
	if (lpf_bw) {
		if (LMS_SetLPFBW(device, is_tx, channel, lpf_bw) < 0) {
			fprintf(stderr, "LMS_SetLPFBW() : %s\n", LMS_GetLastErrorMessage());
			return -1;
		}
	}

	return 0;
}

int main(int argc, char** argv)
{
	if ( argc < 2 ) {
		printf("Usage: %s <OPTIONS>\n", argv[0]);
		printf("  -if <INPUT_FREQUENCY>\n"
		       "  -il <INPUT_LO_FREQUENCY> (default: 0Hz)\n"
		       "  -ib <INPUT_LPF_BW> (default: none)\n"
		       "  -of <OUTPUT_FREQUENCY>\n"
		       "  -oi <OUTPUT_IF_FREQUENCY> (default: 0Hz)\n"
		       "  -ol <OUTPUT_LO_FREQUENCY> (default: 0Hz)\n"
		       "  -ob <OUTPUT_LPF_BW> (default: none)\n"
		       "  -b <BANDWIDTH_CALIBRATING> (default: 8e6)\n"
		       "  -ig <INPUT_GAIN_NORMALIZED> (default: 1)\n"
		       "  -og <OUTPUT_GAIN_NORMALIZED> (default: 1)\n"
		       "  -d <DEVICE_INDEX> (default: 0)\n"
		       "  -ic <CHANNEL_INDEX> (default: 0)\n"
		       "  -oc <CHANNEL_INDEX> (default: 0)\n"
		       "  -r <REFERENCE_CLOCK> (default: do not change)\n"
		       "  -ip <IP TO SEND UDP>\n"
		       "  -c <0|1> (calibration mode: listen on TX freq, default: 0)\n");
		return 1;
	}
	int i;
	double in_freq = 0, out_freq = 0;
	double in_if_freq = 0, out_if_freq = 0;
	double in_lo_freq = 0, out_lo_freq = 0;
	double in_lpf_bw = 0, out_lpf_bw = 0;
	double bandwidth_calibrating = 8e6;
	double sample_rate = 1.5e6;
	double qo100_lo = 10489.5e6 - 2400e6;
	double in_gain = 1, out_gain = 1;
	unsigned int device_i = 0;
	unsigned int in_channel = 0, out_channel = 0;
	double reference_clock = 0;
	int calibration_mode = 0;
	char *ip = NULL;	
	for ( i = 1; i < argc-1; i += 2 ) {
		if      (strcmp(argv[i], "-if") == 0) { in_freq = atof( argv[i+1] ); }
		else if (strcmp(argv[i], "-il") == 0) { in_lo_freq = atof(argv[i+1]); }
		else if (strcmp(argv[i], "-ib") == 0) { in_lpf_bw = atof(argv[i+1]); }
		else if (strcmp(argv[i], "-of") == 0) { out_freq = atof( argv[i+1] ); }
		else if (strcmp(argv[i], "-oi") == 0) { out_if_freq = atof(argv[i+1]); }
		else if (strcmp(argv[i], "-ol") == 0) { out_lo_freq = atof(argv[i+1]); }
		else if (strcmp(argv[i], "-bo") == 0) { out_lpf_bw = atof(argv[i+1]); }
		else if (strcmp(argv[i], "-b") == 0) { bandwidth_calibrating = atof( argv[i+1] ); }
		else if (strcmp(argv[i], "-ig") == 0) { in_gain = atof( argv[i+1] ); }
		else if (strcmp(argv[i], "-og") == 0) { out_gain = atof( argv[i+1] ); }
		else if (strcmp(argv[i], "-d") == 0) { device_i = atoi( argv[i+1] ); }
		else if (strcmp(argv[i], "-ic") == 0) { in_channel = atoi( argv[i+1] ); }
		else if (strcmp(argv[i], "-oc") == 0) { out_channel = atoi( argv[i+1] ); }
		else if (strcmp(argv[i], "-r") == 0) { reference_clock = atof(argv[i+1]); }
		else if (strcmp(argv[i], "-ip") == 0) { ip = argv[i+1]; }
		else if (strcmp(argv[i], "-c") == 0) { calibration_mode = atoi(argv[i+1]); }
	}
	in_freq = out_freq + qo100_lo;
	if (in_freq == 0) {
		fprintf(stderr, "ERROR: invalid RX frequency\n");
		exit(1);
	}
	if (out_freq == 0) {
		fprintf(stderr, "ERROR: invalid TX frequency\n");
		exit(1);
	}
	if (!ip) {
		fprintf(stderr, "Need to specify send IP\n");
		exit(1);
	}
	if ((calibration_mode != 0) && (calibration_mode != 1)) {
		fprintf(stderr, "Invalid calibration mode\n");
		exit(1);
	}

	int linrad_udp_socket;
	struct sockaddr_in linrad_udp_sockaddr;
	static struct linrad_udp_packet udp_packet;

	if (open_linrad_udp_socket(&linrad_udp_socket, &linrad_udp_sockaddr, ip) < 0) {
		perror("Could not open Linrad UDP socket");
		exit(1);
	}
	init_linrad_header(&udp_packet, 1e-6*out_freq);

	const int tx_data_size = 2400000;
	const int tx_data_samples = tx_data_size / (2 * sizeof(int16_t));
	int tx_data_idx = 0;
	uint8_t *tx_data = malloc(2 * tx_data_size); // we have two copies of the data in the buffer
	if (!tx_data) {
		perror("Could not allocate tx_data buffer");
		exit(1);
	}
	FILE *f = fopen("tx_signal.int16", "rb");
	if (!f) {
		perror("Could not open tx_signal.int16");
		exit(1);
	}
	if (fread(tx_data, tx_data_size, 1, f) != 1) {
		perror("Could not read tx_signal.int16");
		exit(1);
	}
	fclose(f);
	memcpy(tx_data + tx_data_size, tx_data, tx_data_size);
	
	lms_device_t* device = NULL;
	double host_sample_rate;

	if (limesdr_open(device_i, &device) < 0) {
		exit(1);
	}

	if (reference_clock > 0) {
		if (LMS_SetClockFreq(device, LMS_CLOCK_REF, reference_clock) < 0) {
			fprintf(stderr, "LMS_SetClockFreq() : %s\n", LMS_GetLastErrorMessage());
			exit(1);
		}
	}

	if (limesdr_enable_channels(device, in_channel, out_channel) < 0) {
		exit(1);
	}
	if (limesdr_set_sample_rate(device, sample_rate, &host_sample_rate) < 0) {
		exit(1);
	}
	fprintf(stderr, "sample_rate: %f\n", host_sample_rate);
	
	fprintf(stderr, "Setting RX frequency\n");
	if (calibration_mode) {
		if (limesdr_set_frequency(device, LMS_CH_RX, in_channel,
					  out_freq - out_lo_freq, in_if_freq, in_lpf_bw) < 0) {
			exit(1);
		}
	}
	else {
		if (limesdr_set_frequency(device, LMS_CH_RX, in_channel,
					  in_freq - in_lo_freq, in_if_freq, in_lpf_bw) < 0) {
			exit(1);
		}
	}
	fprintf(stderr, "Setting TX frequency\n");
	if (limesdr_set_frequency(device, LMS_CH_TX, out_channel,
				  out_freq - out_lo_freq, out_if_freq, out_lpf_bw) < 0) {
		exit(1);
	}

	if (LMS_SetNormalizedGain(device, LMS_CH_RX, in_channel, in_gain) < 0) {
		fprintf(stderr, "LMS_SetNormalizedGain() (RX) : %s\n", LMS_GetLastErrorMessage());
		exit(1);
	}
	if (LMS_SetNormalizedGain(device, LMS_CH_TX, out_channel, out_gain) < 0) {
		fprintf(stderr, "LMS_SetNormalizedGain() (TX) : %s\n", LMS_GetLastErrorMessage());
		exit(1);
	}

	if (LMS_Calibrate(device, LMS_CH_RX, in_channel, bandwidth_calibrating, 0) < 0) {
		fprintf(stderr, "LMS_Calibrate() (RX) : %s\n", LMS_GetLastErrorMessage());
		exit(1);
	}

	if (LMS_Calibrate(device, LMS_CH_TX, out_channel, bandwidth_calibrating, 0) < 0) {
		fprintf(stderr, "LMS_Calibrate() (TX) : %s\n", LMS_GetLastErrorMessage());
		exit(1);
	}
	
	lms_stream_t rx_stream = {
		.channel = in_channel,
		.fifoSize = LINRAD_SAMPLES_PER_PACKET*1000,
		.throughputVsLatency = 1.0,
		.isTx = LMS_CH_RX,
		.dataFmt = LMS_FMT_I16
	};

	lms_stream_t tx_stream = {
		.channel = out_channel,
		.fifoSize = LINRAD_SAMPLES_PER_PACKET*100,
		.throughputVsLatency = 1.0,
		.isTx = LMS_CH_TX,
		.dataFmt = LMS_FMT_I16
	};
	
	if ( LMS_SetupStream(device, &rx_stream) < 0 ) {
		fprintf(stderr, "LMS_SetupStream() : %s\n", LMS_GetLastErrorMessage());
		return 1;
	}
	if ( LMS_SetupStream(device, &tx_stream) < 0 ) {
	 	fprintf(stderr, "LMS_SetupStream() : %s\n", LMS_GetLastErrorMessage());
	 	return 1;
	}

	if (LMS_StartStream(&rx_stream) < 0) {
		fprintf(stderr, "LMS_StartStream() (RX) : %s\n", LMS_GetLastErrorMessage());
	}
	if (LMS_StartStream(&tx_stream) < 0) {
	 	fprintf(stderr, "LMS_StartStream() (TX) : %s\n", LMS_GetLastErrorMessage());
	}
	int keep_reading = 1;
	int just_read;

	/* start reading and throw away some samples */
	/* this forces RX dropped packets to occur now, before we are streaming RX
           data by the network, or starting TX data */
	for (int j = 0; j < 100; j++) {
	 	for (int read = 0; read < LINRAD_SAMPLES_PER_PACKET; read += just_read) {
	 		int timeout_ms =  1000;
			just_read = LMS_RecvStream(&rx_stream,
						   udp_packet.buffer + read * 2 * sizeof(int16_t),
						   LINRAD_SAMPLES_PER_PACKET - read,
						   NULL, timeout_ms);
			if (just_read < 0) {
				fprintf(stderr, "LMS_RecvStream() : %s\n", LMS_GetLastErrorMessage());
				exit(1);
			}
		}
	}
	
	unsigned int laps = 0;
	int tx_underrun = 0, tx_overrun = 0, tx_dropped = 0;
	int rx_underrun = 0, rx_overrun = 0, rx_dropped = 0;
	uint64_t total_samples_read = 0;
	const int delay = 1024*128;

	tx_data_idx = delay * 2 * sizeof(int16_t);
	
	lms_stream_meta_t rx_meta, tx_meta;
	int synchronized = 0;

	memset(&rx_meta, 0, sizeof(rx_meta));
	memset(&tx_meta, 0, sizeof(tx_meta));
	tx_meta.waitForTimestamp = true;

	while (keep_reading) {
		if (laps++ % 0x512 == 0) {
			// Print FIFOs status
			lms_stream_status_t tx_status, rx_status;
			if (LMS_GetStreamStatus(&tx_stream, &tx_status) < 0) {
			 	fprintf(stderr, "LMS_GetStreamStatus() : %s\n", LMS_GetLastErrorMessage());
			 	break;
			}
			if (LMS_GetStreamStatus(&rx_stream, &rx_status) < 0) {
				fprintf(stderr, "LMS_GetStreamStatus() : %s\n", LMS_GetLastErrorMessage());
				break;
			}
			tx_underrun += tx_status.underrun;
			tx_overrun += tx_status.overrun;
			tx_dropped += tx_status.droppedPackets;
			rx_underrun += rx_status.underrun;
			rx_overrun += rx_status.overrun;
			rx_dropped += rx_status.droppedPackets;
			fprintf(stderr,
				"STREAM STATUS\n"
				"-------------\n"
				"TX: %d / %d, under = %d, over = %d, dropped = %d, timestamp = %llu\n"
				"RX: %d / %d, under = %d, over = %d, dropped = %d, timestamp = %llu\n",
				tx_status.fifoFilledCount, tx_status.fifoSize,
				tx_underrun, tx_overrun, tx_dropped, tx_status.timestamp,
				rx_status.fifoFilledCount, rx_status.fifoSize,
				rx_underrun, rx_overrun, rx_dropped, rx_status.timestamp);
		}
		
		lms_stream_meta_t *meta;
		for (int read = 0; read < LINRAD_SAMPLES_PER_PACKET; read += just_read) {
			int timeout_ms =  1000;
			meta = synchronized == 0 ? &rx_meta : NULL;
			just_read = LMS_RecvStream(&rx_stream,
						   udp_packet.buffer + read * 2 * sizeof(int16_t),
						   LINRAD_SAMPLES_PER_PACKET - read,
						   meta, timeout_ms);
			if (just_read < 0) {
				fprintf(stderr, "LMS_RecvStream() : %s\n", LMS_GetLastErrorMessage());
				keep_reading = 0;
				break;
			}
			if (synchronized == 0) synchronized = 1;
		}

		int ret;

		lms_stream_status_t tx_status;
		if (LMS_GetStreamStatus(&tx_stream, &tx_status) < 0) {
			fprintf(stderr, "LMS_GetStreamStatus() : %s\n", LMS_GetLastErrorMessage());
			break;
		}
		tx_underrun += tx_status.underrun;
		tx_overrun += tx_status.overrun;
		tx_dropped += tx_status.droppedPackets;
		int send_samples = tx_status.fifoSize - tx_status.fifoFilledCount;
		if (send_samples > tx_data_samples) send_samples = tx_data_samples;
		if (send_samples) {
			if (synchronized == 1) {
				meta = &tx_meta;
				tx_meta.timestamp = rx_meta.timestamp + delay;
				synchronized = 2;
			}
			else {
				meta = NULL;
			}
			if ((ret = LMS_SendStream(&tx_stream, tx_data + tx_data_idx, send_samples, meta, 1000)) < 0) {
				fprintf(stderr, "LMS_SendStream() : %s\n", LMS_GetLastErrorMessage());
				break;
			}
			if (ret != send_samples) {
				fprintf(stderr, "Didn't write to TX FIFO all we expected\n");
				break;
			}
			tx_data_idx += send_samples * 2 * sizeof(int16_t);
			if (tx_data_idx > tx_data_size) tx_data_idx -= tx_data_size;
		}

		// Adjust DC bias
		for (int i = 0; i < 2 * LINRAD_SAMPLES_PER_PACKET; i++) {
			((int16_t *) udp_packet.buffer)[i] |= 8; // 3 LSBs are guaranteed to be zero
		}

		if (linrad_header_fill_time(&udp_packet) < 0) {
			perror("Could not get system time");
			break;
		}

		if (sendto(linrad_udp_socket, &udp_packet, sizeof(udp_packet), 0,
			   (struct sockaddr *) &linrad_udp_sockaddr,
			   sizeof(linrad_udp_sockaddr)) < 0) {
			perror("Could not send UDP packet");
			break;
		}

		next_linrad_header(&udp_packet);
		total_samples_read += LINRAD_SAMPLES_PER_PACKET;
	}

	LMS_StopStream(&rx_stream);
	LMS_DestroyStream(device, &rx_stream);
	LMS_Close(device);
	return 0;
}
