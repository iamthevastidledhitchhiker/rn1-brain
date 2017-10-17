/*
A bit of development history:

First we used a Neato XV11 lidar; it seemed a good idea at the time to do some prototyping on. 
We also considered using it on early production models (small batches).

A few things happened:
- The XV11 lidar has a serious lifetime problem with the slip ring communication.
- It also has an availability problem; most units are used, and already starting to fail.
- We decided not to use 2D lidar on long term, since we are actively developing our own 3D vision system.

Scanse Sweep was not available at that time, but now it is, so we migrated to it.

We decided to break all compatibility to the old Neato lidar simply because of its poor availability and reliability;
Scanse, OTOH, is readily available.

Changing the lidars is not the case of only changing the low-level layer, since the two lidars work in different principle:
Neato lidar gave readings on fixed angular intervals of 1.0 degrees, whereas the Scance gives variable number of samples with
an angular resolution of 1/16th degree (5760th full circle). Sampling time, and hence the angular sample interval, is _not_ constant.

Since the code designed for Neato only serves historical purposes, the compatibility will be broken on purpose. You can always
look at the old code in the version control.

*/

/*
Scance Sweep

Sweep	Sample	Min	Max	Samples/rev
rate	mode	samp.f	samp.f	min	max

1Hz	01	500Hz	600Hz	500	600
1Hz	02	750Hz	800Hz	750	800
1Hz	03	1000Hz	1075Hz	1000	1075

2Hz	01	500Hz	600Hz	250	300
2Hz	02	750Hz	800Hz	375	400
2Hz	03	1000Hz	1075Hz	500	538

3Hz	01	500Hz	600Hz	166	200
3Hz	02	750Hz	800Hz	250	267
3Hz	03	1000Hz	1075Hz	333	359

4Hz	01	500Hz	600Hz	125	150
4Hz	02	750Hz	800Hz	187	200
4Hz	03	1000Hz	1075Hz	250	269

We probably don't want to ever run at over 4Hz, since the angular resolution would be compromised too much. (Maybe we'll do it 
one day as a separate mode, when the environment is well mapped, with lots of easy visual clues, proven IMU reliability, so that
the localization dependence on good laser data is not crucial. Then we can run at higher speeds and avoid obstacles.

For mapping large spaces, it's crucial to get enough samples from far-away (say, 10 meters) walls, when seen through
gaps created by nearby obstacles. The 360 degree resolution of the Neato lidar seemed bare minimum; even with its low 5 meter
range!

At the same time, we want to gather enough temporal data to ignore moving objects and fill in the wall data while the robot
moves and sees the world from different angles. To add to this compromise, we don't want to sacrifice measurement accuracy.

To begin, we'll be using 2Hz sweep with 750-800Hz sample rate, giving us 375 to 400 samples per 360 degrees.

Maximum number of readings per sweep is guaranteed 1075. We'll probably never do that.

Let's use an array[720] to hold the samples: we'll never try to write to the same cell twice (if we don't use 1Hz-02 or 1Hz-03 modes)

We fill the array in the interrupt handler receiving the packets: even though the array step is only 0.5 deg, full 1/16th deg resolution
is used to calculate the (x,y) coords, this calculation uses the most recent robot coordinates to map the lidar to the world coords.


*/

#include <stdint.h>
#include "ext_include/stm32f2xx.h"

#include "main.h"
#include "lidar.h"

extern int dbg[10];

extern void delay_us(uint32_t i);
extern void delay_ms(uint32_t i);

void lidar_reset_flags() 
{
	DMA2->LIFCR = 0b111101UL<<16;
}
void lidar_reset_complete_flag() 
{
	DMA2->LIFCR = 0b100000UL<<16;
}
void lidar_reset_half_flag() 
{
	DMA2->LIFCR = 0b010000UL<<16;
}

int lidar_is_complete()
{
	return DMA2->LISR & (1UL<<21);
}

int lidar_is_half()
{
	return DMA2->LISR & (1UL<<20);
}

uint8_t lidar_ignore[360];

const int lidar_ignore_len[32] =
#if defined(RN1P4) || defined(RN1P6) || defined(RN1P5)
{
100,
(100+170)/2,
170,
(170+200)/2,
200,
(200+220)/2,
220,
(220+200)/2,
200,
(200+250)/2,
250,
(250+300)/2,
300,
(300+390)/2,
390,
(390+350)/2,
350,
(350+390)/2,
390,
(390+300)/2,
300,
(300+250)/2,
250,
(250+200)/2,
200,
(200+220)/2,
220,
(220+200)/2,
200,
(200+170)/2,
170,
(170+100)/2
};
#endif

#ifdef PULU1
{
30,
(30+55)/2,
55,
(55+70)/2,
70,
(70+90)/2,
90,
(90+70)/2,
70,
(70+90)/2,
90,
(90+120)/2,
120,
(120+210)/2,
210,
(210+190)/2,
190,
(190+210)/2,
210,
(210+120)/2,
120,
(120+90)/2,
90,
(90+70)/2,
70,
(70+90)/2,
90,
(90+70)/2,
70,
(70+55)/2,
55,
(55+30)/2
};
#endif


extern live_lidar_scan_t* p_livelidar_store;
extern point_t* p_livelid2d_store;
extern int* p_livelidar_num_samples_store; // For counting validness of data for lidar-based correction.
/*
	Call lidar_fsm() at 1 kHz.

	As lidar produces full revolutions at 5Hz, packets are generated at 450Hz.
	Data is prepared and stored with location info (cur_pos from feedbacks.c) on each sample.
*/

int reset;
int cur_lidar_id;

void reset_livelidar_images(int id)
{
	reset = 3; // to skip doing anything with the image being acquired right now.
	if(id > -1 && id < 128)
	{
		cur_lidar_id = id;
	}
}

void lidar_mark_invalid()
{
	p_livelidar_store->status |= LIVELIDAR_INVALID;
}

void lidar_fsm()
{
	static int prev_cur_packet;
	int packets_left = DMA2_Stream2->NDTR/22;
	int cur_packet = 89-packets_left; if(cur_packet == -1) cur_packet = 0;

	if(cur_packet != prev_cur_packet)
	{
		int valid;
		int dist;
		int idx = prev_cur_packet; // We read from previous packet, since writing it is finished.
		int odx = 89-idx; // We write starting from the end to mirror the lidar image.
		int valid_tbl_odx = odx/15; // Validness table includes total counts of valid points divided in six 60 degree segments.
		int ignore_len_tbl_odx = (odx*32+16)/90;
		COPY_POS(p_livelidar_store->pos[odx], cur_pos);

		dist = lidar_full_rev[idx].d[0].flags_distance&0x3fff;
		p_livelidar_store->scan[odx*4+3] = dist;
		valid = !((lidar_ignore[idx*4+0]) || (lidar_full_rev[idx].d[0].flags_distance&(1<<15)) || dist < lidar_ignore_len[ignore_len_tbl_odx]);
		p_livelid2d_store[odx*4+3].valid = valid;
		if(valid) p_livelidar_num_samples_store[valid_tbl_odx]++;

		dist = lidar_full_rev[idx].d[1].flags_distance&0x3fff;
		p_livelidar_store->scan[odx*4+2] = dist;
		valid = !((lidar_ignore[idx*4+1]) || (lidar_full_rev[idx].d[1].flags_distance&(1<<15)) || dist < lidar_ignore_len[ignore_len_tbl_odx]);
		p_livelid2d_store[odx*4+2].valid = valid;
		if(valid) p_livelidar_num_samples_store[valid_tbl_odx]++;

		dist = lidar_full_rev[idx].d[2].flags_distance&0x3fff;
		p_livelidar_store->scan[odx*4+1] = dist;
		valid = !((lidar_ignore[idx*4+2]) || (lidar_full_rev[idx].d[2].flags_distance&(1<<15)) || dist < lidar_ignore_len[ignore_len_tbl_odx]);
		p_livelid2d_store[odx*4+1].valid = valid;
		if(valid) p_livelidar_num_samples_store[valid_tbl_odx]++;

		dist = lidar_full_rev[idx].d[3].flags_distance&0x3fff;
		p_livelidar_store->scan[odx*4+0] = dist;
		valid = !((lidar_ignore[idx*4+3]) || (lidar_full_rev[idx].d[3].flags_distance&(1<<15)) || dist < lidar_ignore_len[ignore_len_tbl_odx]);
		p_livelid2d_store[odx*4+0].valid = valid;
		if(valid) p_livelidar_num_samples_store[valid_tbl_odx]++;


		if(prev_cur_packet == 81)
		{
			// Time is running out: tell lidar_corr that calculation must be finished ASAP, or terminated.
			// It won't be terminated right away; the termination condition is not checked too frequently as
			// it would slow down the process.
			live_lidar_calc_must_be_finished();
		}

		if(prev_cur_packet == 89)
		{	
			// We just got the full round.

			if(reset == 0)
			{

				int skip = livelidar_skip();


				// Now processing the two previous lidar images is (must be) finished, and the correction
				// has already been applied to the latter one. We still need to apply the same correction
				// to this new image we just finished storing:

				if(!skip)
					apply_corr_to_livelidar(p_livelidar_store);

				p_livelidar_store->id = cur_lidar_id;

				// Now, correction has been applied to both images: the previous one, and the one just finished.
				// We can swap the buffers to start gathering the new image, and start processing the latest scan:
				livelidar_storage_finished(cur_lidar_id);

				// One more thing, we need to apply the correction to the robot coordinates right here,
				// so that we get the new coords applied to the new lidar scan from the start:
				extern pos_t latest_corr; // from lidar_corr.c
				if(!skip)
					correct_location_without_moving(latest_corr);
			}
			else
			{
				reset--;
				reset_lidar_corr_images();

				if(reset == 0)
					livelidar_storage_finished();
			}
		}
	}
	prev_cur_packet = cur_packet;
}

void generate_lidar_ignore()
{
	int i;
	for(i = 0; i < 360; i++) lidar_ignore[i] = 0;

	for(i = 0; i < 90; i++)
	{
		int o;
		for(o = 0; o < 4; o++)
		{
			if(!(lidar_full_rev[i].d[o].flags_distance&(1<<15)))
			{
				if((int)(lidar_full_rev[i].d[o].flags_distance&0x3fff) < ((i<12||i>=78)?LIDAR_IGNORE_LEN_FRONT:LIDAR_IGNORE_LEN))
				{
					int cur = i*4+o;
					int next = cur+1; if(next > 359) next = 0;
					int prev = cur-1; if(prev < 0) prev = 359;
					lidar_ignore[prev] = 1;
					lidar_ignore[cur] = 1;
					lidar_ignore[next] = 1;
				}
			}
		}
	}
}

typedef enum
{
	S_LIDAR_UNINIT = 0,
	S_LIDAR_OFF,
	S_LIDAR_WAITPOWERED,
	S_LIDAR_PRECONF_WAIT_READY, // Quite stupidly, we need to poll whether the motor has reached its initial (default, or the previous) setpoint, 
	                            // even if we want to just configure it again to whatever we actually want. Then we need to wait again.
	S_LIDAR_CONF1,
	S_LIDAR_CONF2,
	S_LIDAR_WAIT_READY,
	S_LIDAR_WAIT_START_ACK,
	S_LIDAR_RUNNING,
	S_LIDAR_ERROR
} lidar_state_t;

lidar_state_t cur_lidar_state;


uint8_t lidar_rxbuf[2][16];
uint8_t lidar_txbuf[16];

uint8_t lidar_error_flags;


int sweep_idx;

int prev_lidar_scan_idx;

/*
Double buffer of processed lidar scans in the world coordinate frame.
ack_lidar_scan is actively written to all the time.
prev_lidar_scan points to the finished buffer. Take a local copy of the pointer: even if you start reading
it just before the buffer swap happens, you'll likely read it faster than the new data comes in, so it's ok :-).

The buffers are never zeroed out, but written over.
*/

lidar_scan_t lidar_scans[2];
lidar_scan_t *acq_lidar_scan;
lidar_scan_t *prev_lidar_scan;

int lidar_fps = 2;
int lidar_smp = 2;

/*
	Controls the state machine to turn lidar on, stabilize, configure, and start acquiring.
	If called during acquisition, motor speed / sampling is changed on the fly.
*/
void lidar_on(int fps, int smp)
{
	if(fps < 1 || fps > 5 || smp < 1 || smp > 3)
		return;
	lidar_fps = fps;
	lidar_smp = smp;
	LIDAR_ENA();
	if(cur_lidar_state == S_LIDAR_RUNNING)
		cur_lidar_state = S_LIDAR_RECONF;
	else
		cur_lidar_state = S_LIDAR_WAITPOWERED;
}

void lidar_off()
{
	LIDAR_DIS();
	cur_lidar_state = S_LIDAR_OFF;
}

int wait_ready_poll_cnt;

void lidar_fsm()
{
	static int powerwait_cnt;

	switch(cur_lidar_state)
	{
		case S_LIDAR_WAITPOWERED:
		{
			if(++powerwait_cnt > 2000)
			{
				powerwait_cnt = 0;
				wait_ready_poll_cnt = 1000; // Try to first poll after 1 sec
				cur_lidar_state = S_LIDAR_PRECONF_WAIT_READY;
			}
		}
		break;

		case S_LIDAR_PRECONF_WAIT_READY:
		case S_LIDAR_WAIT_READY:
		{
			// Just do the tx part of the polling here, the state is changed in ISR based on the reply.
			if(--wait_ready_poll_cnt == 0)
			{
				lidar_txbuf[0] = 'M';
				lidar_txbuf[1] = 'Z';
				lidar_txbuf[2] = 10;
				lidar_send_cmd(3, 4);
			}
		}
		break;


		default:
		break;
	}
}

void lidar_rx_done_inthandler()
{
	switch(cur_lidar_state)
	{
		case S_LIDAR_PRECONF_WAIT_READY:
		{
			if(lidar_rxbuf[0] == 'M' && 
			   lidar_rxbuf[1] == 'Z' &&
			   lidar_rxbuf[2] == '0' &&
			   lidar_rxbuf[3] == '0')
			{
				// Motor speed is stabilized to whatever uninteresting default value: now we can start configuring the device.
				lidar_txbuf[0] = 'M';
				lidar_txbuf[1] = 'S';
				lidar_txbuf[2] = '0';
				lidar_txbuf[3] = '0'+lidar_fps;
				lidar_txbuf[4] = 10;
				lidar_send_cmd(5, 9);
				cur_lidar_state = S_LIDAR_CONF1;
			}
			else
			{
				// Motor not stabilized yet. Poll again after 100 ms pause (in lidar_fsm())
				wait_ready_poll_cnt = 100;
			}
		}
		break;

		case S_LIDAR_CONF1:
		{
			if(lidar_rxbuf[0] == 'M' && 
			   lidar_rxbuf[1] == 'S' &&
			   lidar_rxbuf[2] == '0' &&
			   lidar_rxbuf[3] == '0'+lidar_fps &&
			   lidar_rxbuf[4] == 10 &&
			   lidar_rxbuf[5] == '0' &&
			   lidar_rxbuf[6] == '0' &&
			   lidar_rxbuf[7] == 'P' &&
			   lidar_rxbuf[8] == 10)
			{
				// Adjust motor speed command succeeded.
				// Send "Adjust LiDAR Sample Rate" command.
				lidar_txbuf[0] = 'L';
				lidar_txbuf[1] = 'R';
				lidar_txbuf[2] = '0';
				lidar_txbuf[3] = '0'+lidar_smp;
				lidar_txbuf[4] = 10;
				lidar_send_cmd(5, 9);
				cur_lidar_state = S_LIDAR_CONF2;
			}
			else // unexpected reply
			{
				cur_lidar_state = S_LIDAR_ERROR;
			}
		}
		break;

		case S_LIDAR_CONF2:
		{
			if(lidar_rxbuf[0] == 'L' && 
			   lidar_rxbuf[1] == 'R' &&
			   lidar_rxbuf[2] == '0' &&
			   lidar_rxbuf[3] == '0'+lidar_smp &&
			   lidar_rxbuf[4] == 10 &&
			   lidar_rxbuf[5] == '0' &&
			   lidar_rxbuf[6] == '0' &&
			   lidar_rxbuf[7] == 'P' &&
			   lidar_rxbuf[8] == 10)
			{
				// Sample rate successfully set.
				wait_ready_poll_cnt = 1000; // Force the generation of "is speed stabilized?" poll message after 1 second. (Polling done in lidar_fsm())
				cur_lidar_state = S_LIDAR_WAIT_READY;
			}
			else // unexpected reply
			{
				cur_lidar_state = S_LIDAR_ERROR;
			}
		}
		break;

		case S_LIDAR_WAIT_READY:
		{
			if(lidar_rxbuf[0] == 'M' && 
			   lidar_rxbuf[1] == 'Z' &&
			   lidar_rxbuf[2] == '0' &&
			   lidar_rxbuf[3] == '0')
			{
				// Motor speed is stabilized.
				// Send "Start data acquisition" command and start waiting for actual scan data.
				lidar_txbuf[0] = 'D';
				lidar_txbuf[1] = 'S';
				lidar_txbuf[2] = 10;
				lidar_send_cmd(3, 6);
				cur_lidar_state = S_LIDAR_WAIT_START_ACK;
			}
			else
			{
				// Motor not stabilized yet. Poll again after 100 ms pause (in lidar_fsm())
				wait_ready_poll_cnt = 100;
			}
		}
		break;

		case S_LIDAR_WAIT_START_ACK:
		{
			if(lidar_rxbuf[0] == 'D' && 
			   lidar_rxbuf[1] == 'S' &&
			   lidar_rxbuf[2] == '0' &&
			   lidar_rxbuf[3] == '0' &&
			   lidar_rxbuf[4] == 'P') // The correct checksum from "00"
			{
				// Start data acquisition acknowledged OK. Reconfigure the DMA to circular doublebuffer without reconfig, to minimize time
				// spent in this ISR in the RUNNING state.
				lidar_start_acq();
				cur_lidar_state = S_LIDAR_RUNNING;
				chk_err_cnt = 0;
			}
			else
			{
				// This shouldn't happen, as we have polled to confirm that the motor is ready.
				cur_lidar_state = S_LIDAR_ERROR;
			}
		}
		break;

		case S_LIDAR_RUNNING:
		{
			int buf_idx = (DMA2_Stream2->CR&(1UL<<19))?0:1; // We want to read the previous buffer, not the one the DMA is now writing to.
			// Actual data packet is 7 bytes of binary instead of ASCII.
			int chk = (lidar_rxbuf[buf_idx][0]+lidar_rxbuf[buf_idx][1]+lidar_rxbuf[buf_idx][2]+
				  lidar_rxbuf[buf_idx][3]+lidar_rxbuf[buf_idx][4]+lidar_rxbuf[buf_idx][5]) % 255;
			if(chk != lidar_rxbuf[buf_idx][6] /*checksum fail*/ || (lidar_rxbuf[buf_idx][0]&0b11111110) /* any error bit*/)
			{
				chk_err_cnt+=20;

				if(chk_err_cnt > 100)
				{
					// In the long run, 1/20th of the data is allowed to fail the checksum / error flag tests.
					// In the short run, 5 successive samples are allowed to fail.
					cur_lidar_state = S_LIDAR_ERROR;
					lidar_error_flags = lidar_rxbuf[buf_idx][0];
				}
				// Else: just ignore this data.

				break;
			}


			if(chk_err_cnt) chk_err_cnt--;
			lidar_s cur_lidar_scan_idx;
			if(lidar_rxbuf[buf_idx][0]) // non-zero = sync. (error flags have been handled already)
			{
				COPY_POS(acq_lidar_scan->pos_at_end, cur_pos);
				lidar_scan_t* swptmp;
				swptmp = prev_lidar_scan;
				prev_lidar_scan = acq_lidar_scan;
				acq_lidar_scan = swptmp;
				COPY_POS(acq_lidar_scan->pos_at_start, cur_pos);
			}

			// optimization todo: we are little endian like the sensor: align rxbuf properly and directly access as uint16
			int32_t degper16 = (lidar_rxbuf[buf_idx][2]<<8) | lidar_rxbuf[buf_idx][1];
			int32_t len      = (lidar_rxbuf[buf_idx][4]<<8) | lidar_rxbuf[buf_idx][3];
//			int snr      = lidar_rxbuf[buf_idx][5];

			unsigned int degper2 = degper16>>3;
			if(degper2 > 719)
			{
				chk_err_cnt+=20;
				break;
			}

			if(len < 2)
			{
				acq_lidar_scan->scan[degper2].valid = 0;
				break;
			}

			len *= 10; // cm --> mm

			uint32_t ang32 = (uint32_t)cur_pos.ang + degper16*ANG_1PER16_DEG;
			int32_t y_idx = (ang32)>>SIN_LUT_SHIFT;
			int32_t x_idx = (1073741824-ang32)>>SIN_LUT_SHIFT;

			acq_lidar_scan->scan[degper2].valid = 1;
			acq_lidar_scan->scan[degper2].x = cur_pos.x + (((int32_t)sin_lut[x_idx] * (int32_t)len)>>15);
			acq_lidar_scan->scan[degper2].y = cur_pos.y + (((int32_t)sin_lut[y_idx] * (int32_t)len)>>15);
			

				
		}
		break;


		default:
		break;

	}

}

/*
	Sends a command to the lidar using DMA; configures DMA to expect answer, which will give an interrupt after rx_len bytes received.
	To save a little bit of time, buffers are fixed (lidar_txbuf and lidar_rxbuf).
*/
void lidar_send_cmd(int tx_len, int rx_len)
{
	// Configure for RX:
	DMA2_Stream2->CR = 4UL<<25 /*Channel*/ | 0b01UL<<16 /*med prio*/ | 0b00UL<<13 /*8-bit mem*/ | 0b00UL<<11 /*8-bit periph*/ |
	                   1UL<<10 /*mem increment*/ | 1UL<<4 /*transfer complete interrupt*/;
	DMA2_Stream2->NDTR = rx_len;

	// Configure for TX:
	DMA2_Stream7->CR = 4UL<<25 /*Channel*/ | 0b01UL<<16 /*med prio*/ | 0b00UL<<13 /*8-bit mem*/ | 0b00UL<<11 /*8-bit periph*/ |
	                   1UL<<10 /*mem increment*/;
	DMA2_Stream7->NDTR = tx_len;

	USART1->SR = 0;
	DMA2->LIFCR = 0xffffffff; // Clear all flags
	DMA2->HIFCR = 0xffffffff;

	DMA2_Stream2->CR |= 1UL; // Enable RX DMA
	DMA2_Stream7->CR |= 1UL; // Enable TX DMA
}

/*
	Start receiving - configure the RX DMA for circular double buffering - we don't need to reconfigure DMA until we stop.
	Each new full packet causes an interrupt.
*/
void lidar_start_acq()
{
	DMA2_Stream2->CR = 4UL<<25 /*Channel*/ | 1UL<<18 /*Double Buf mode*/ | 0b01UL<<16 /*med prio*/ | 
			   0b00UL<<13 /*8-bit mem*/ | 0b00UL<<11 /*8-bit periph*/ |
	                   1UL<<10 /*mem increment*/ | 1UL<<8 /*circular*/;  // Disable

	USART1->SR = 0;
	DMA2->LIFCR = 0xffffffff; // Clear all flags
	DMA2->HIFCR = 0xffffffff;

	DMA2_Stream2->CR |= 1UL; // Enable RX DMA
}


void init_lidar()
{
	// USART1 (lidar) = APB2 = 60 MHz
	// 16x oversampling
	// 115200bps -> Baudrate register = 32.5625 = 32 9/16
	// USART1 RX: DMA2 Stream2 Ch4
	// USART1 TX: DMA2 Stream7 Ch4

	// Preconfigure what we can on the DMA, don't enable yet.

	DMA2_Stream2->PAR = (uint32_t)&(USART1->DR);
	DMA2_Stream2->M0AR = (uint32_t)(lidar_rxbuf[0]);
	DMA2_Stream2->M1AR = (uint32_t)(lidar_rxbuf[1]);

	DMA2_Stream7->PAR = (uint32_t)&(USART1->DR);
	DMA2_Stream7->M0AR = (uint32_t)(lidar_txbuf);

	USART1->BRR = 32UL<<4 | 9UL;
	USART1->CR1 = 1UL<<13 /*USART enable*/ | 1UL<<3 /*TX ena*/ | 1UL<<2 /*RX ena*/;
	USART1->CR3 = 1UL<<7 /*TX DMA*/ 1UL<<6 /*RX DMA*/;

	cur_lidar_state = S_LIDAR_OFF;
}

