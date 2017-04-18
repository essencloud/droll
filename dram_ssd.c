/********************************************************************

                        Droll - SSD simulator

     Copyright (C) 2017 Hyunmo Chung (hyunmo@essencloud.com)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*********************************************************************/


// FTL이 없는 상태에서 SATA 인터페이스와 HCT/HIL을 테스트하기 위한 모듈
// NAND를 전혀 사용하지 않고 DRAM을 저장 공간으로 사용한다.
// SATA 최고 성능을 보려면 아래 change_delay_mode() 함수를 고쳐서 항상 "고정된 delay 1ns" 가 적용되도록 한다.


#include "droll.h"

#if OPTION_DRAM_SSD

#define PENDING_OPERATIONS_MAX		32

enum { NAND_WRITE_DONE, NAND_READ_DONE };

ftl_cmd_queue_t g_ftl_cmd_q;
feedback_queue_t g_feedback_q;

typedef struct
{
	UINT32	delay_min;
	UINT32	delay_max;
	UINT32	num_pending_ops;
	UINT32	mode_change_count_down;

	UINT32	count_down[NCQ_SIZE];

} dram_ssd_t;

static dram_ssd_t g_ds;

void ftl_open(void)
{
	STOSD(&g_ds, 0, sizeof(g_ds));

	#if OPTION_FAST_DRAM_SSD
	g_ds.delay_min = g_ds.delay_max = 1;
	#else
	g_ds.mode_change_count_down = 1;
	#endif
}

static void spend_time(void)
{
	sim_message_t* msg = sim_receive_message(SIM_ENTITY_FTL);

	if (msg == NULL)
		return;

	if (msg->code == SIM_EVENT_DELAY)
	{
		ASSERT(g_ds.num_pending_ops != 0);
		g_ds.num_pending_ops--;

		UINT32 buf_slot_id = (UINT32) (msg->arg_64 >> 32);
		UINT32 num_sectors = (UINT32) (msg->arg_64 & FF32);

		if (msg->arg_16 == NAND_READ_DONE)						// do_read() 에서 과거에 보낸 메시지
		{
			for (UINT32 i = 0; i < num_sectors; i++)
			{
				buf_slot_t* buf_ptr = bm_read_buf_ptr(buf_slot_id++);
				buf_ptr->valid = TRUE;
				buf_ptr->trimmed = FALSE;
			}

			UINT32 num_starters = msg->arg_32;
			UINT32 ncq_tag = msg->arg_32_a;

			if (num_starters != 0)
			{
				UINT32 count_down = g_ds.count_down[ncq_tag];
				ASSERT(count_down >= num_starters);
				count_down -= num_starters;
				g_ds.count_down[ncq_tag] = count_down;

				if (count_down == 0)
				{
					// 해당 read 명령의 첫번째 DATA FIS를 호스트에게 보낼 수 있게 되었음
					hil_notify_read_ready(ncq_tag);
				}
			}

			sim_wake_up(SIM_ENTITY_HIL);
		}
		else
		{
			ASSERT(msg->arg_16 == NAND_WRITE_DONE); 			// do_write() 에서 과거에 보낸 메시지

			bm_release_write_buf(buf_slot_id, num_sectors); 	// write buffer 반납
		}
	}

	sim_release_message_slot(msg);
}

static void do_read(ftl_cmd_t* cmd)
{
	// NAND 읽기의 기본 단위는 sector가 아닌 slice 이다.
	// host read 명령의 시작 위치 또는 끝 위치가 slice 경계선에 align 되지 않은 경우, 호스트에게 전송되지 않고 HIL에 의해서 버려지는 섹터들이 있다.

	UINT32 sector_count = (cmd->sector_count == 0) ? SECTOR_COUNT_MAX : cmd->sector_count;

	ASSERT(cmd->lba + sector_count <= NUM_LSECTORS);

	UINT32 offset = cmd->lba % SECTORS_PER_SLICE;
	UINT32 lba = cmd->lba - offset;
	UINT32 remaining_slices = (offset + sector_count + SECTORS_PER_SLICE - 1) / SECTORS_PER_SLICE;
	UINT32 ncq_tag = cmd->ncq_tag;
	UINT32 count_down = MIN(sector_count, DATA_FIS_SIZE_MAX);

	g_ds.count_down[ncq_tag] = count_down;		// 첫번째 DATA FIS의 크기 (섹터 수)

	while (remaining_slices != 0)
	{
		while (g_ds.num_pending_ops == PENDING_OPERATIONS_MAX)
		{
			spend_time();
		}

		UINT32 r = random(1, 8);
		UINT32 num_slices = MIN(remaining_slices, r);
		UINT32 num_sectors = num_slices * SECTORS_PER_SLICE;
		UINT32 buf_slot_id;

		while (bm_alloc_read_buf(num_sectors, &buf_slot_id) == FAIL)
		{
			spend_time();
		}

		UINT32 temp = buf_slot_id;
		UINT32 num_starters = 0;				// number of starters

		for (UINT32 i = 0; i < num_sectors; i++)
		{
			buf_slot_t* buf_ptr = bm_read_buf_ptr(temp++);

			#if OPTION_VERIFY_DATA
			buf_ptr->body[0] = g_dram.storage[lba];
			#endif

			buf_ptr->lba = lba++;
			buf_ptr->ncq_tag = (UINT8) ncq_tag;

			if (offset != 0)
			{
				offset--;						// HIL에 의해서 버려지는 섹터
			}
			else if (count_down != 0)
			{
				count_down--;					// read 명령의 첫번째 DATA FIS에 속하는 섹터
				num_starters++;
			}
		}

		sim_message_t* msg = sim_new_message();

		msg->code = SIM_EVENT_DELAY;
		msg->when = g_sim_context.current_time + random(g_ds.delay_min, g_ds.delay_max);
		msg->arg_16 = NAND_READ_DONE;
		msg->arg_32 = num_starters;
		msg->arg_32_a = ncq_tag;
		msg->arg_64 = (((UINT64) buf_slot_id) << 32) | num_sectors;

		sim_send_message(msg, SIM_ENTITY_FTL);

		g_ds.num_pending_ops++;

		remaining_slices -= num_slices;
	}
}

static void do_write(ftl_cmd_t* cmd)
{
	UINT32 lba = cmd->lba;
	UINT32 remaining_sectors = (cmd->sector_count == 0) ? SECTOR_COUNT_MAX : cmd->sector_count;
	ASSERT(cmd->lba + remaining_sectors <= NUM_LSECTORS);

	while (remaining_sectors != 0)
	{
		while (g_ds.num_pending_ops == PENDING_OPERATIONS_MAX)
		{
			spend_time();
		}

		UINT32 r = random(1, 8) * SECTORS_PER_SLICE;
		UINT32 num_sectors = MIN(remaining_sectors, r);
		UINT32 buf_slot_id;

		while (bm_consume_write_data(num_sectors, &buf_slot_id) == FAIL)
		{
			spend_time();
		}

		UINT32 buf_slot_id_tmp = buf_slot_id;

		for (UINT32 i = 0; i < num_sectors; i++, lba++)
		{
			buf_slot_t* buf_ptr = bm_write_buf_ptr(buf_slot_id_tmp++);
			ASSERT(buf_ptr->lba == lba);

			#if OPTION_VERIFY_DATA
			g_dram.storage[lba] = buf_ptr->body[0];
			#endif
		}

		sim_message_t* msg = sim_new_message();

		msg->code = SIM_EVENT_DELAY;
		msg->when = g_sim_context.current_time + random(g_ds.delay_min, g_ds.delay_max);
		msg->arg_16 = NAND_WRITE_DONE;
		msg->arg_64 = (((UINT64) buf_slot_id) << 32) | num_sectors;

		sim_send_message(msg, SIM_ENTITY_FTL);

		g_ds.num_pending_ops++;

		remaining_sectors -= num_sectors;
	}
}

static void change_delay_mode(void)
{
	switch (random(0, 4))
	{
		case 0: // 고정된 delay 1ns
			g_ds.delay_min = g_ds.delay_max = 1;
			break;

		case 1: // 고정된 delay 1ns - 10ms
			g_ds.delay_min = g_ds.delay_max = random(1, 10000000);
			break;

		case 2: // 작은 random delay 1ns - 100us
			g_ds.delay_min = 1;
			g_ds.delay_max = 100000;
			break;

		case 3: // 큰 random delay 7ms - 10ms
			g_ds.delay_min = 7000000;
			g_ds.delay_max = 10000000;
			break;

		case 4: // 변동이 심한 random delay 1ns - 10ms
			g_ds.delay_min = 1;
			g_ds.delay_max = 10000000;
			break;

		default:
			__assume(FALSE);
	}
}

static void handle_command(ftl_cmd_t* cmd)
{
	#if OPTION_FAST_DRAM_SSD == FALSE
	{
		if (--g_ds.mode_change_count_down == 0)
		{
			change_delay_mode();
			g_ds.mode_change_count_down = 5000000;
		}
	}
	#endif

	switch (cmd->req_code)
	{
		case REQ_HOST_READ:
		{
			do_read(cmd);
			break;
		}
		case REQ_HOST_WRITE:
		{
			do_write(cmd);

			#if 0
			g_dram.storage[1234] = 5;	// OPTION_VERIFY_DATA 가 bug를 얼마나 빨리 검출하는지 테스트
			#endif

			break;
		}
		case REQ_TRIM:
		{
			ASSERT(cmd->lba + cmd->sector_count <= NUM_LSECTORS);
			memset(g_dram.storage + cmd->lba, 0, (cmd->sector_count == 0) ? SECTOR_COUNT_MAX : cmd->sector_count);
			send_feedback_to_hil(FB_TRIM_DONE, NULL);
			break;
		}
		case REQ_FAST_FLUSH:
		case REQ_SLOW_FLUSH:
		{
			while (g_ds.num_pending_ops != 0)
			{
				spend_time();
			}

			send_feedback_to_hil(FB_FLUSH_DONE, NULL);

			break;
		}
		default:
			ASSERT(FAIL);
	}
}

void ftl_format(void)
{
	#if NUM_LSECTORS % 8 == 0
	STOSQ(g_dram.storage, 0, NUM_LSECTORS);
	#else
	STOSD(g_dram.storage, 0, NUM_LSECTORS);
	#endif
}

void ftl_main(void)
{
	while (1)
	{
		if (g_ftl_cmd_q.front == V32(g_ftl_cmd_q.rear))
		{
			spend_time();
		}
		else
		{
			ftl_cmd_t* new_cmd = g_ftl_cmd_q.queue + g_ftl_cmd_q.front%FTL_CMD_Q_SIZE;

			handle_command(new_cmd);

			V32(g_ftl_cmd_q.front) = g_ftl_cmd_q.front + 1;
		}
	}
}

#endif	// OPTION_DRAM_SSD
