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


#include "droll.h"

// NAND flash controller 하드웨어와 FIL 펌웨어를 통합한 추상적 시뮬레이션 모델

typedef enum	// die state
{
	DS_IDLE = 0,

	DS_R1_STALL,	// 채널 할당을 기다림
	DS_R2_IO,		// read 명령과 주소를 NAND에게 보내고 있음
	DS_R3_CELL,		// 미리 지정된 시간동안 NAND를 가만히 내버려둠 (길이는 펌웨어가 tR 값을 참조하여 미리 설정)
	DS_R4_STALL,	// 채널 할당을 기다림
	DS_R5_IO,		// status check 명령을 보내서 NAND internal read가 끝났는지 확인
	DS_R6_IO,		// 데이터가 NAND에서 컨트롤러로 전송됨

	DS_W1_STALL,	// 채널 할당을 기다림
	DS_W2_IO,		// write 명령, 주소 및 데이터를 NAND에게 보내고 있음
	DS_W3_CELL,		// 미리 지정된 시간동안 NAND를 가만히 내버려둠 (길이는 펌웨어가 tPROG 값을 참조하여 미리 설정)
	DS_W4_STALL,	// 채널 할당을 기다림
	DS_W5_IO,		// status check 명령을 보내서 ready/busy 확인, ready 되었으면 pass/fail 여부까지 확인

	DS_E1_STALL,	// 채널 할당을 기다림
	DS_E2_IO,		// erase 명령과 주소를 NAND에게 보내고 있음
	DS_E3_CELL,		// 미리 지정된 시간동안 NAND를 가만히 내버려둠 (길이는 펌웨어가 tBERS 값을 참조하여 미리 설정)
	DS_E4_STALL,	// 채널 할당을 기다림
	DS_E5_IO,		// status check 명령을 보내서 ready/busy 확인, ready 되었으면 pass/fail 여부까지 확인

} ds_t;

typedef struct
{
	ds_t			die_state[NUM_DIES];				// flash controller 하드웨어에 있는 state machine (die 마다 각각 state machine 하나씩 존재)
	flash_cmd_t*	current_cmd[NUM_DIES];				// 현재 수행중인 명령
	UINT64			cmd_begin_time[NUM_DIES];			// 명령 수행을 시작한 시각
	UINT32			ch_owner[NUM_CHANNELS];				// 현재 채널을 사용중인 die
	UINT64			ch_release_time[NUM_CHANNELS];		// 채널을 반납한 시각
	UINT32			num_ch_requests[NUM_CHANNELS];		// STALL 상태에 있는 die의 개수 (채널 수요)
	UINT64			stall_begin_time[NUM_DIES];			// STALL 상태가 시작된 시각
	UINT32			host_open_blk[NUM_DIES];			// FOP_OPEN_HOST_BLK 통해 FTL이 알려준 정보
	UINT32			gc_open_blk[NUM_DIES];				// FOP_OPEN_GC_BLK 통해 FTL이 알려준 정보
	UINT32			host_write_wl[NUM_DIES];			// FOP_WRITE_HOST 할 때마다 증가
	UINT32			gc_write_wl[NUM_DIES];				// FOP_WRITE_GC 할 때마다 증가
	UINT64			time[NUM_DIES];
	UINT32			num_busy_dies;
	UINT32			unused;

	nand_packet_t	nand_packet[NUM_DIES];

} fil_context_t;

flash_interface_t g_flash_interface;	// FTL과 FIL 사이의 interface
static fil_context_t g_fil_context;		// FIL 내부 상태 정보


static void die_state_machine(UINT32 die);

static void fil_init(void)
{
	STOSD(&g_flash_interface, 0, sizeof(g_flash_interface));
	STOSQ(&g_fil_context, 0, sizeof(g_fil_context));

	for (UINT32 i = 0; i < FLASH_CMD_TABLE_SIZE; i++)
	{
		g_flash_interface.table[i].next_slot = i + 1;	// linked list of free slots
	}

	UINT64 now = g_sim_context.current_time;

	for (UINT32 ch = 0; ch < NUM_CHANNELS; ch++)
	{
		g_fil_context.ch_owner[ch] = FF32;
		g_fil_context.ch_release_time[ch] = now;
	}

	bm_init();
}

static void schedule_next_event(UINT32 die, UINT64 time)
{
	// die state machine을 움직이게 만드는 이벤트

	UINT64 current_time = g_sim_context.current_time;

	sim_message_t* msg = sim_new_message();
	msg->code = SIM_EVENT_DELAY;
	msg->when = current_time + time;
	msg->arg_32 = die;
	sim_send_message(msg, SIM_ENTITY_FIL);
}

static void finish_command(flash_cmd_t* command)
{
	flash_interface_t* fi = &g_flash_interface;

	g_fil_context.num_busy_dies--;

	UINT32 cmd_id = (UINT32) (command - fi->table);
	ASSERT(cmd_id < FLASH_CMD_TABLE_SIZE);

	if (command->flag & NF_NOTIFY)
	{
		// FTL에게 결과 보냄

		ASSERT(fi->rq_rear - V32(fi->rq_front) < RESULT_QUEUE_SIZE);	// 특별한 조치가 필요 없도록 RESULT_QUEUE_SIZE 를 넉넉하게 잡았음

		fi->result_queue[fi->rq_rear % RESULT_QUEUE_SIZE] = (UINT8) cmd_id;

		V8(command->status) = CS_DONE;

		V32(fi->rq_rear) = fi->rq_rear + 1;
	}
	else
	{
		// FIL이 직접 뒤처리하고 Flash Command Table 에서 제거

		switch (command->fop_code)
		{
			case FOP_READ_HOST:
			{
				read_userdata_t* cmd = (read_userdata_t*) command;

				if (cmd->num_starters != 0)
				{
					UINT32 ncq_tag = cmd->ncq_tag;
					UINT32 count_down = g_flash_interface.count_down[ncq_tag];	// host_read()에 의해서 초기화된 값
					ASSERT(count_down >= cmd->num_starters);
					count_down -= cmd->num_starters;
					g_flash_interface.count_down[ncq_tag] = (UINT8) count_down;

					if (count_down == 0)
					{
						hil_notify_read_ready(ncq_tag);							// read 명령의 첫번째 DATA FIS를 호스트에게 보낼 수 있게 되었음
					}
				}

				sim_wake_up(SIM_ENTITY_HIL);

				break;
			}
			case FOP_WRITE_HOST:
			{
				write_userdata_t* cmd = (write_userdata_t*)command;
				bm_release_write_buf(cmd->buf_slot_id, cmd->num_slices * SECTORS_PER_SLICE);	// write buffer 반납
				break;
			}
			default:
			{
				// do nothing
			}
		}

		command->status = CS_FREE;

		MUTEX_LOCK(&g_cs_flash_cmd_table);

		V32(command->next_slot) = fi->free_slot_index;
		V32(fi->free_slot_index) = cmd_id;

		ASSERT(fi->num_commands != 0);
		V32(fi->num_commands) = fi->num_commands - 1;

		MUTEX_UNLOCK(&g_cs_flash_cmd_table);
	}

	sim_wake_up(SIM_ENTITY_FTL);
}

static void register_open_blk(UINT32 die, flash_control_t* cmd)
{
	// 향후 FOP_WRITE_HOST, FOP_WRITE_GC 를 위해 사용하게 될 블럭을 등록
	// FOP_WRITE_HOST, FOP_WRITE_GC 에는 nand_addr 필드가 없다.

	fil_context_t* fc = &g_fil_context;

	if (cmd->fop_code == FOP_OPEN_HOST)
	{
		ASSERT(fc->host_open_blk[die] == NULL || fc->host_write_wl[die] == WLS_PER_BLK);	// open 블럭을 등록한 적이 없거나 기존 블럭을 다 채움

		fc->host_open_blk[die] = cmd->arg_32_1;
		fc->host_write_wl[die] = 0;
	}
	else
	{
		ASSERT(cmd->fop_code == FOP_OPEN_GC);
		ASSERT(fc->gc_open_blk[die] == NULL || fc->gc_write_wl[die] == WLS_PER_BLK);

		fc->gc_open_blk[die] = cmd->arg_32_1;
		fc->gc_write_wl[die] = 0;
	}
}

static void channel_arbitration(UINT32 ch)
{
	fil_context_t* fc = &g_fil_context;

	if (fc->ch_owner[ch] != FF32)			// 채널 사용중
	{
		return;
	}
	else if (fc->num_ch_requests[ch] == 0)	// 채널 수요가 없음
	{
		return;
	}

	// 채널 할당의 우선 순위
	// 1. write/erase의 status check (DS_W4_STALL, DS_E4_STALL)
	// 2. read/erase의 명령 및 주소 (DS_R1_STALL, DS_E1_STALL)
	// 3. read의 status check 및 데이터 (DS_R4_STALL)
	// 4. write의 명령, 주소, 데이터 (DS_W1_STALL)
	// 동일한 우선 순위가 존재하면 cmd_begin_time이 앞서는 die를 우대

	UINT32 winner = FF32, high_score = 0;

	for (UINT32 die = ch; die < NUM_DIES; die += NUM_CHANNELS)
	{
		UINT32 my_score;

		switch (fc->die_state[die])
		{
			case DS_W4_STALL:	my_score = 4;	break;
			case DS_E4_STALL:	my_score = 4;	break;
			case DS_R1_STALL:	my_score = 3;	break;
			case DS_E1_STALL:	my_score = 3;	break;
			case DS_R4_STALL:	my_score = 2;	break;
			case DS_W1_STALL:	my_score = 1;	break;
			default:			continue;
		}

		if (my_score > high_score)
		{
			high_score = my_score;
			winner = die;
		}
		else if (my_score == high_score && fc->cmd_begin_time[die] < fc->cmd_begin_time[winner])
		{
			high_score = my_score;
			winner = die;
		}
	}

	#if VERBOSE_NAND_STATISTICS
	{
		UINT64 now = g_sim_context.current_time;

		switch (fc->die_state[winner])
		{
			case DS_W4_STALL:	g_nand_stat.write_stall_sum += now - fc->stall_begin_time[winner];	break;
			case DS_E4_STALL:	g_nand_stat.erase_stall_sum += now - fc->stall_begin_time[winner];	break;
			case DS_R1_STALL:	g_nand_stat.read_stall_sum += now - fc->stall_begin_time[winner];	break;
			case DS_E1_STALL:	g_nand_stat.erase_stall_sum += now - fc->stall_begin_time[winner];	break;
			case DS_R4_STALL:	g_nand_stat.read_stall_sum += now - fc->stall_begin_time[winner];	break;
			case DS_W1_STALL:	g_nand_stat.write_stall_sum += now - fc->stall_begin_time[winner];	break;
		}

		g_nand_stat.idle_time[ch] += now - fc->ch_release_time[ch];

	}
	#endif

	fc->ch_owner[ch] = winner;

	fc->num_ch_requests[ch]--;

	die_state_machine(winner);
}

static void read_trimmed_slice(read_userdata_t* cmd)
{
	UINT32 lba = (cmd->psa & ~BIT(31)) * SECTORS_PER_SLICE; 	// BIT(31)이 1이면 PSA가 아니고 LSA를 의미한다.
	UINT32 buf_slot_id = cmd->buf_slot_id;

	for (UINT32 sector_offset = 0; sector_offset < SECTORS_PER_SLICE; sector_offset++)
	{
		buf_slot_t* buf_ptr = bm_read_buf_ptr(buf_slot_id);

		buf_ptr->lba = lba++;
		buf_ptr->ncq_tag = cmd->ncq_tag;
		buf_ptr->valid = TRUE;
		buf_ptr->trimmed = TRUE;						// trimmed == TRUE 이면 HIL이 zero pattern을 전송

		buf_slot_id++;
	}
}

static BOOL8 start_flash_operation(UINT32 die)
{
	fil_context_t* fc = &g_fil_context;
	flash_cmd_queue_t* fcq = g_flash_interface.flash_cmd_q + die;

	BOOL8 did_something = FALSE;

fetch_another_cmd:

	if (fcq->front == V32(fcq->rear))		// 할 일이 없음
	{
		return did_something;
	}

	fc->num_busy_dies++;

	did_something = TRUE;

	UINT32 cmd_id = fcq->queue[fcq->front++ % FLASH_QUEUE_SIZE]; 	// FTL로부터 명령 접수
	flash_cmd_t* cmd = g_flash_interface.table + cmd_id;			// FTL이 명령의 상세 정보를 테이블에 적어 놨다.

	sim_wake_up(SIM_ENTITY_FTL);

	cmd->status = CS_SUBMITTED;

	if (cmd->flag & NF_CTRL)
	{
		register_open_blk(die, (flash_control_t*) cmd);

		finish_command(cmd);

		goto fetch_another_cmd;
	}
	else
	{
		// 모든 명령은 일단 STALL 상태로 시작한다. 현재 시각에 채널이 마침 놀고 있고 우선순위가 더 높은 die가 없다면
		// 시간이 경과하기 전에 즉시 STALL 상태에서 빠져나와 채널 사용을 시작한다.

		if (cmd->fop_code < FOP_CLASS_READ)
		{
			if (cmd->fop_code == FOP_READ_HOST && (cmd->read_userdata.psa & BIT(31)))	// BIT(31) 이면 trimmed slice 이다.
			{
				// trimmed slice에 대해서도 일단 FIL에게 FOP_READ_HOST 명령이 내려온다.
				// FTL이 처리하려면 g_flash_interface.count_down 에 대해서 mutex가 필요하기 때문이다.
				// 대개의 경우 g_flash_interface.count_down 은 NAND 동작이 끝난 뒤에 FIL의 finish_command() 에서 감소한다.
				// trimmed slice에 대해서도 예외를 만들지 말고 동일하게 처리함으로써 mutex 사용을 피하기로 한다.

				read_trimmed_slice((read_userdata_t*) cmd);

				finish_command(cmd);

				goto fetch_another_cmd;
			}

			fc->die_state[die] = DS_R1_STALL;
		}
		else if (cmd->fop_code < FOP_CLASS_WRITE)
		{
			fc->die_state[die] = DS_W1_STALL;
		}
		else if (cmd->fop_code == FOP_ERASE)
		{
			fc->die_state[die] = DS_E1_STALL;
		}
		else
		{
			CHECK(FAIL);
		}

		UINT64 current_time = g_sim_context.current_time;

		fc->current_cmd[die] = cmd;
		fc->cmd_begin_time[die] = current_time;
		fc->stall_begin_time[die] = current_time;
		fc->num_ch_requests[die % NUM_CHANNELS]++;
	}

	channel_arbitration(die % NUM_CHANNELS);	// 채널 할당 시도

	return TRUE;
}

static void release_channel(UINT32 ch)
{
	g_fil_context.ch_owner[ch] = FF32;
	g_fil_context.ch_release_time[ch] = g_sim_context.current_time;
}

static UINT64 do_read(UINT32 die)
{
	fil_context_t* fc = &g_fil_context;
	flash_cmd_t* command = fc->current_cmd[die];
	UINT32 num_slices = 0;
	UINT32 num_planes = 0;

 	switch (command->fop_code)
	{
		case FOP_READ_HOST:
		{
			read_userdata_t* cmd = (read_userdata_t*) command;

			UINT32 big_blk_index, wl_index, slice_offset;
			psa_decode(cmd->psa, &die, &big_blk_index, &wl_index, &slice_offset);
			ASSERT(slice_offset % SLICES_PER_BIG_PAGE == 0);					// cmd->psa 는 읽으려는 big page의 첫번째 slice의 주소이어야 한다.

			UINT8 lcm = (UINT8) (slice_offset / SLICES_PER_BIG_PAGE);			// 0 = LSB, 1 = CSB, 2 = MSB
			UINT32 slice_bmp = cmd->slice_bmp;
			UINT32 prev_plane = FF32;
			UINT32 buf_slot_id = cmd->buf_slot_id;

			while (1)															// 한바퀴 돌 때마다 slice 하나씩 읽음
			{
				UINT32 slice = find_one(slice_bmp);								// big page 내에서의 slice 번호

				if (slice == 32)
				{
					break;
				}

				slice_bmp &= ~BIT(slice);
				num_slices++;

				UINT32 plane = slice / SLICES_PER_SMALL_PAGE;
				slice = slice % SLICES_PER_SMALL_PAGE;							// small page 내에서의 slice 번호

				if (plane != prev_plane)
				{
					num_planes++;
					prev_plane = plane;
				}

				SANITY_CHECK(SECTORS_PER_SLICE == 8);

				// 시뮬레이션의 편의를 위해 main data는 한 섹터당 한 바이트만, 한 슬라이스가 8 섹터이므로 UINT64에 들어감

				UINT16 small_blk_index = (UINT16) (big_blk_index * SMALL_BLKS_PER_BIG_BLK + plane);

				UINT32 extra_data;		// extra data는 한 슬라이스당 4 바이트
				UINT64 main_data = sim_nand_read_userdata((UINT8) die, small_blk_index, (UINT16) wl_index, lcm, (UINT8) slice, &extra_data);

				for (UINT32 sector_offset = 0; sector_offset < SECTORS_PER_SLICE; sector_offset++)
				{
					buf_slot_t* buf_ptr = bm_read_buf_ptr(buf_slot_id);

					buf_ptr->body[0] = (UINT8) main_data;
					buf_ptr->lba = extra_data + sector_offset;			// NAND에 기록할 때에는 4KB당 하나만 (첫번째 섹터만) 기록하고, NAND read 할 때에 모든 섹터마다 붙는다. (Marvell FCT가 붙여줌)
					buf_ptr->ncq_tag = cmd->ncq_tag;					// HIL을 위한 정보
					buf_ptr->valid = TRUE;								// HIL을 위한 정보
					buf_ptr->trimmed = FALSE;							// HIL을 위한 정보

					main_data = main_data >> 8;
					buf_slot_id++;
				}
			}

			break;
		}
		case FOP_READ_INTERNAL:
		case FOP_READ_GC:
		{
			// 아래의 코드에서 cmd->psa 와 cmd->slice_bmp 를 없애고 buf_ptr->psa 를 사용하여 더 간결한 코드를 작성하는 것이 가능하다.
			// 그러나 CPU가 buf_ptr->psa 를 읽으면 DRAM read로 인한 stall cycle이 많이 발생하므로, 아래의 코드를 고치지 않기로 한다.

			read_userdata_t* cmd = (read_userdata_t*) command;

			UINT32 slice_bmp = cmd->slice_bmp;
			temp_buf_t* buf_ptr = bm_buf_ptr(cmd->buf_slot_id);				// gc_read()에 의해서 만들어진 circular linked list의 마지막 노드
			UINT32 prev_plane = FF32;

			UINT32 _die, big_blk_index, wl_index, slice_offset;
			psa_decode(cmd->psa, &_die, &big_blk_index, &wl_index, &slice_offset);
			ASSERT(_die == die && slice_offset % SLICES_PER_BIG_PAGE == 0);			// cmd->psa 는 big page의 첫번째 slice의 PSA 이어야 한다.

			UINT8 lcm = (UINT8) (slice_offset / SLICES_PER_BIG_PAGE);

			do
			{
				buf_ptr = buf_ptr->next_slot;

				UINT32 slice = find_one(slice_bmp);
				ASSERT(slice < SLICES_PER_BIG_PAGE);

				slice_bmp &= ~BIT(slice);

				UINT32 plane = slice / SLICES_PER_SMALL_PAGE;
				slice = slice % SLICES_PER_SMALL_PAGE;

				if (plane != prev_plane)
				{
					num_planes++;
					prev_plane = plane;
				}

				SANITY_CHECK(SECTORS_PER_SLICE == 8);

				UINT16 small_blk_index = (UINT16) (big_blk_index * SMALL_BLKS_PER_BIG_BLK + plane);

				UINT32 extra_data;
				UINT64 main_data = sim_nand_read_userdata((UINT8) die, small_blk_index, (UINT16) wl_index, lcm, (UINT8) slice, &extra_data);

				for (UINT32 sector_offset = 0; sector_offset < SECTORS_PER_SLICE; sector_offset++)
				{
					buf_sector_t* sector = buf_ptr->sector + sector_offset;

					sector->body[0] = (UINT8) (main_data & 0xFF);
					sector->lba = extra_data + sector_offset;

					main_data = main_data >> 8;
				}

			} while (++num_slices != cmd->num_slices);

			break;
		}
		case FOP_READ_METADATA:
		{
			read_metadata_t* cmd = (read_metadata_t*) command;

			UINT32 prev_plane = FF32;

			UINT32 _die, big_blk_index, wl_index, slice_offset;
			psa_decode(cmd->psa, &_die, &big_blk_index, &wl_index, &slice_offset);
			ASSERT(_die == die);

			UINT8 lcm = (UINT8) (slice_offset / SLICES_PER_BIG_PAGE);
			UINT32 slice = slice_offset % SLICES_PER_BIG_PAGE;					// big page 내에서의 slice 번호

			ASSERT(cmd->dram_addr % sizeof(UINT64) == 0 && ((UINT64) cmd->dram_addr) + cmd->num_slices * BYTES_PER_SLICE < DRAM_SIZE);
			UINT8* dram_addr = ((UINT8*) (&g_dram)) + cmd->dram_addr;

			num_slices = cmd->num_slices;

			for (UINT32 i = 0; i < num_slices; i++)
			{
				UINT32 plane = slice / SLICES_PER_SMALL_PAGE;

				if (plane != prev_plane)
				{
					num_planes++;
					prev_plane = plane;
				}

				UINT16 small_blk_index = (UINT16) (big_blk_index * SMALL_BLKS_PER_BIG_BLK + plane);

				sim_nand_read_metadata((UINT8) die, small_blk_index, (UINT16) wl_index, lcm, (UINT8) (slice % SLICES_PER_SMALL_PAGE), dram_addr);

				dram_addr += BYTES_PER_SLICE;
				slice++;
			}

			break;
		}
		default:
			CHECK(FAIL);
	}

	UINT64 time = (UINT64) (SIM_NAND_NANOSEC_PER_BYTE * BYTES_PER_SLICE_EX * num_slices);	// data 및 random pattern 전송에 걸리는 시간
	time += 1000 * num_planes;																// 각종 overhead

	return time;
}

static UINT64 estimate_io_time(flash_cmd_t* command)
{
	// write 명령 처리에 필요한 채널 점유 시간을 계산

	UINT32 num_slices, num_planes;
	UINT64 time;

	if (command->flag & NF_SMALL)
	{
		num_planes = 1;
		num_slices = SLICES_PER_SMALL_WL;
		time = 0;
	}
	else
	{
		num_planes = PLANES_PER_DIE;
		num_slices = SLICES_PER_BIG_WL;
		time = NAND_T_DBSY * (PLANES_PER_DIE - 1);
	}

	time += NAND_T_TRAN * (PAGES_PER_WL - 1);										// L->C, C->M 전환에 걸리는 시간
	time += (UINT64) (SIM_NAND_NANOSEC_PER_BYTE * BYTES_PER_SLICE_EX * num_slices);	// data 및 random pattern 전송에 걸리는 시간
	time += 1000 + NAND_T_DBSY * num_planes;										// 명령, 주소 전송 및 기타 overhead

	return time;
}

static void do_write(UINT32 die)
{
	// 이 함수는 DS_W2_IO 상태에서 DS_W3_CELL 상태로 넘어갈 때에 호출된다.

	fil_context_t* fc = &g_fil_context;
	flash_cmd_t* command = fc->current_cmd[die];
	nand_packet_t* nand_pkt = fc->nand_packet + die;	// 컨트롤러에서 NAND로 보내는 data packet

	switch (command->fop_code)
	{
		case FOP_WRITE_HOST:
		{
			write_userdata_t* cmd = (write_userdata_t*) command;
			UINT32 big_blk_index = fc->host_open_blk[die];
			UINT32 wl_index = fc->host_write_wl[die]++;
			UINT32 buf_slot_id = cmd->buf_slot_id;

			ASSERT(wl_index < WLS_PER_BLK);
			ASSERT(cmd->num_slices <= SLICES_PER_BIG_WL);

			UINT8* main_data = (UINT8*) nand_pkt->main_data;
			UINT32* extra_data = (UINT32*) nand_pkt->extra_data;

			for (UINT32 slice = 0; slice < cmd->num_slices; slice++)
			{
				buf_slot_t* buf_ptr = bm_write_buf_ptr(buf_slot_id);

				*extra_data++ = buf_ptr->lba;

				for (UINT32 sct = 0; sct < SECTORS_PER_SLICE; sct++)
				{
					*main_data++ = buf_ptr->body[0];

					buf_ptr = bm_write_buf_ptr(++buf_slot_id);
				}
			}

			UINT16 small_blk_index[PLANES_PER_DIE];

			for (UINT32 i = 0; i < PLANES_PER_DIE; i++)
			{
				small_blk_index[i] = (UINT16) (big_blk_index * SMALL_BLKS_PER_BIG_BLK + i);
			}

			sim_nand_write_userdata(die, small_blk_index, wl_index, nand_pkt);

			break;
		}
		case FOP_WRITE_GC:
		{
			write_userdata_t* cmd = (write_userdata_t*) command;
			UINT32 big_blk_index = fc->gc_open_blk[die];
			UINT32 wl_index = fc->gc_write_wl[die]++;

			ASSERT(wl_index < WLS_PER_BLK);
			ASSERT(cmd->num_slices <= SLICES_PER_BIG_WL);

			UINT8* main_data = (UINT8*) nand_pkt->main_data;
			UINT32* extra_data = (UINT32*) nand_pkt->extra_data;

			temp_buf_t* buf_ptr = bm_buf_ptr(cmd->buf_slot_id);			// gc_write()에 의해서 만들어진 circular linked list의 마지막 노드

			for (UINT32 slice = 0; slice < cmd->num_slices; slice++)
			{
				buf_ptr = buf_ptr->next_slot;

				*extra_data++ = buf_ptr->sector[0].lba;					// slice의 첫번째 섹터의 LBA 값을 해당 slice의 extra data field에 기록

				for (UINT32 sct = 0; sct < SECTORS_PER_SLICE; sct++)
				{
					*main_data++ = buf_ptr->sector[sct].body[0];
				}
			}

			UINT16 small_blk_index[PLANES_PER_DIE];

			for (UINT32 i = 0; i < PLANES_PER_DIE; i++)
			{
				small_blk_index[i] = (UINT16) (big_blk_index * SMALL_BLKS_PER_BIG_BLK + i);
			}

			sim_nand_write_userdata(die, small_blk_index, wl_index, nand_pkt);

			break;
		}
		case FOP_WRITE_METADATA:
		{
			write_metadata_t* cmd = (write_metadata_t*) command;

			UINT32 wl_index = cmd->wl_index;
			ASSERT(wl_index < WLS_PER_BLK);

			ASSERT(cmd->dram_addr % sizeof(UINT64) == 0 && ((UINT64) cmd->dram_addr) + cmd->num_slices * BYTES_PER_SLICE < DRAM_SIZE);
			UINT8* dram_addr = ((UINT8*) (&g_dram)) + cmd->dram_addr;

			// cmd->num_slices를 고려하여 부족한 부분은 FIL이 random pattern으로 채워야 하나, 시뮬레이션에서는 생략

			if (cmd->flag & NF_SMALL)
			{
				// single plane program

				ASSERT(cmd->num_slices <= SLICES_PER_SMALL_WL);

				UINT16 small_blk_index = cmd->blk_index;
				ASSERT(small_blk_index != NULL && small_blk_index < SMALL_BLKS_PER_DIE && wl_index < WLS_PER_BLK);

				sim_nand_write_metadata_sp(die, small_blk_index, wl_index, dram_addr);
			}
			else
			{
				// multiplane (all-plane) program

				ASSERT(cmd->num_slices <= SLICES_PER_BIG_WL);

				UINT32 big_blk_index = cmd->blk_index;

				UINT16 small_blk_index[PLANES_PER_DIE];

				for (UINT32 i = 0; i < PLANES_PER_DIE; i++)
				{
					small_blk_index[i] = (UINT16) (big_blk_index * SMALL_BLKS_PER_BIG_BLK + i);
				}

				sim_nand_write_metadata_mp(die, small_blk_index, wl_index, dram_addr);
			}

			break;
		}
		default:
		{
			CHECK(FAIL);
		}
	}
}

static void do_erase(UINT32 die)
{
	fil_context_t* fc = &g_fil_context;
	erase_t* cmd = (erase_t*) fc->current_cmd[die];

	ASSERT(cmd->fop_code == FOP_ERASE);

	if (cmd->flag & NF_SMALL)
	{
		ASSERT(cmd->blk_index != NULL && cmd->blk_index < SMALL_BLKS_PER_DIE);

		sim_nand_erase_sp(die, (UINT16) cmd->blk_index);
	}
	else
	{
		ASSERT(cmd->blk_index != NULL && cmd->blk_index < BIG_BLKS_PER_DIE);

		UINT32 big_blk_index = cmd->blk_index;
		UINT16 small_blk_index[PLANES_PER_DIE];

		for (UINT32 i = 0; i < PLANES_PER_DIE; i++)
		{
			small_blk_index[i] = (UINT16) (big_blk_index * SMALL_BLKS_PER_BIG_BLK + i);
		}

		sim_nand_erase_mp(die, small_blk_index);
	}
}

static void die_state_machine(UINT32 die)
{
	// 이것은 NAND 내부의 state machine이 아니고 flash controller의 state machine이다.

	fil_context_t* fc = &g_fil_context;
	UINT64 current_time = g_sim_context.current_time;

	switch (fc->die_state[die])
	{
		case DS_R1_STALL:
		{
			// stall 상태였다가 채널을 할당 받아 다음 상태로 넘어감

			fc->die_state[die] = DS_R2_IO;
			schedule_next_event(die, 3000);						// 명령 및 주소 전송에 3us 소요
			NAND_STAT(g_nand_stat.io_time[die] += 3000);
			break;
		}
		case DS_R2_IO:
		{
			fc->time[die] = do_read(die);						// NAND 읽기 - do_read()의 리턴값은 DS_R6_IO 상태의 길이

			release_channel(die % NUM_CHANNELS);
			fc->die_state[die] = DS_R3_CELL;
			schedule_next_event(die, NAND_T_R * 19 / 20);		// 예상되는 tR의 95%가 경과했을 때부터 status check 시작
			break;
		}
		case DS_R3_CELL:
		{
			fc->die_state[die] = DS_R4_STALL;
			fc->stall_begin_time[die] = current_time;
			fc->num_ch_requests[die % NUM_CHANNELS]++;
			break;
		}
		case DS_R4_STALL:
		{
			fc->die_state[die] = DS_R5_IO;
			schedule_next_event(die, STATUS_CHECK_TIME);
			break;
		}
		case DS_R5_IO:
		{
			if (sim_nand_busy(die) == TRUE)
			{
				release_channel(die % NUM_CHANNELS);
				fc->die_state[die] = DS_R3_CELL;
				schedule_next_event(die, STATUS_CHECK_INTERVAL);		// 아직 안끝났으므로 당분간 내버려두었다가 다시 확인
			}
			else
			{
				fc->die_state[die] = DS_R6_IO;
				schedule_next_event(die, fc->time[die]);				// fc->time[die]은 do_read()에 의해 결정된 값 (데이터 크기에 따른 IO 소요 시간)
				NAND_STAT(g_nand_stat.io_time[die] += STATUS_CHECK_TIME + fc->time[die]);		// NAND 내부 동작이 진행 중인 상태에서 status check 한 것은 io_time에 산입하지 않음 (cell_time 이므로)
			}

			break;
		}
		case DS_R6_IO:
		{
			release_channel(die % NUM_CHANNELS);
			fc->die_state[die] = DS_IDLE;
			NAND_STAT(g_nand_stat.read_time_sum += current_time - fc->cmd_begin_time[die]);
			NAND_STAT(g_nand_stat.num_read_commands++);
			finish_command(fc->current_cmd[die]);
			start_flash_operation(die);									// 혹시 Flash Command Queue에 다음 명령이 있으면 즉시 시작
			break;
		}
		case DS_W1_STALL:
		{
			fc->die_state[die] = DS_W2_IO;								// NAND에게 명령, 주소, 데이터 보내기

			UINT64 transfer_time = estimate_io_time(fc->current_cmd[die]);
			schedule_next_event(die, transfer_time);
			NAND_STAT(g_nand_stat.io_time[die] += transfer_time);
			break;
		}
		case DS_W2_IO:
		{
			do_write(die);												// NAND 쓰기

			release_channel(die % NUM_CHANNELS);
			fc->die_state[die] = DS_W3_CELL;
			schedule_next_event(die, NAND_T_PROGO * 19 / 20);			// 예상되는 tPROGO의 95%가 경과했을 때부터 status check 시작
			break;
		}
		case DS_W3_CELL:
		{
			fc->die_state[die] = DS_W4_STALL;
			fc->stall_begin_time[die] = current_time;
			fc->num_ch_requests[die % NUM_CHANNELS]++;
			break;
		}
		case DS_W4_STALL:
		{
			fc->die_state[die] = DS_W5_IO;
			schedule_next_event(die, STATUS_CHECK_TIME);
			break;
		}
		case DS_W5_IO:
		{
			release_channel(die % NUM_CHANNELS);

			if (sim_nand_busy(die) == TRUE)
			{
				fc->die_state[die] = DS_W3_CELL;
				schedule_next_event(die, STATUS_CHECK_INTERVAL);		// 아직 안끝났으므로 당분간 내버려두었다가 다시 확인
			}
			else
			{
				fc->die_state[die] = DS_IDLE;
				NAND_STAT(g_nand_stat.io_time[die] += STATUS_CHECK_TIME);
				NAND_STAT(g_nand_stat.write_time_sum += current_time - fc->cmd_begin_time[die]);
				NAND_STAT(g_nand_stat.num_write_commands++);
				finish_command(fc->current_cmd[die]);
				start_flash_operation(die);								// 혹시 Flash Command Queue에 다음 명령이 있으면 즉시 시작
			}

			break;
		}
		case DS_E1_STALL:
		{
			fc->die_state[die] = DS_E2_IO;								// NAND에게 명령, 주소 보내기
			schedule_next_event(die, 2000);								// 2us 소요 (각종 overhead 포함)
			NAND_STAT(g_nand_stat.io_time[die] += 2000);
			break;
		}
		case DS_E2_IO:
		{
			do_erase(die);												// NAND 지우기

			release_channel(die % NUM_CHANNELS);
			fc->die_state[die] = DS_E3_CELL;
			schedule_next_event(die, NAND_T_BERS * 49 / 50);			// 예상되는 tBERS의 98%가 경과했을 때부터 status check 시작 (지우기가 실제로 언제 끝날지는 NAND 모델이 결정)
			break;
		}
		case DS_E3_CELL:
		{
			fc->die_state[die] = DS_E4_STALL;
			fc->stall_begin_time[die] = current_time;
			fc->num_ch_requests[die % NUM_CHANNELS]++;
			break;
		}
		case DS_E4_STALL:
		{
			fc->die_state[die] = DS_E5_IO;
			schedule_next_event(die, STATUS_CHECK_TIME);
			break;
		}
		case DS_E5_IO:
		{
			release_channel(die % NUM_CHANNELS);

			if (sim_nand_busy(die) == TRUE)
			{
				fc->die_state[die] = DS_E3_CELL;
				schedule_next_event(die, STATUS_CHECK_INTERVAL);		// 아직 안끝났으므로 당분간 내버려두었다가 다시 확인
			}
			else
			{
				fc->die_state[die] = DS_IDLE;
				NAND_STAT(g_nand_stat.io_time[die] += STATUS_CHECK_TIME);
				NAND_STAT(g_nand_stat.erase_time_sum += current_time - fc->cmd_begin_time[die]);
				NAND_STAT(g_nand_stat.num_erase_commands++);
				finish_command(fc->current_cmd[die]);
				start_flash_operation(die);								// 혹시 Flash Command Queue에 다음 명령이 있으면 즉시 시작
			}

			break;
		}
		default:
		{
			CHECK(FAIL);
		}
	}
}

static void fil_main(void)
{
	fil_context_t* fc = &g_fil_context;
	UINT32 next_die = 0;

	while (1)
	{
		BOOL8 did_something = FALSE;

		for (UINT32 i = 0; i < NUM_DIES; i++)
		{
			if (fc->num_busy_dies == NUM_DIES)
			{
				break;
			}

			UINT32 d = next_die++ % NUM_DIES;	// for loop가 매번 die #0에서 시작하는 것을 막기 위한 용도

			if (fc->die_state[d] == DS_IDLE)
			{
				did_something |= start_flash_operation(d);
			}
		}

		if (did_something == FALSE)
		{
			sim_message_t* msg = sim_receive_message(SIM_ENTITY_FIL);

			if (msg == NULL)
				continue;

			if (msg->code == SIM_EVENT_PRINT_STAT)
			{
				MemoryBarrier();
				sim_send_message(msg, SIM_ENTITY_NAND);
			}
			else
			{
				ASSERT(msg->code == SIM_EVENT_DELAY);
				UINT32 die = msg->arg_32;
				sim_release_message_slot(msg);

				die_state_machine(die);

				channel_arbitration(die % NUM_CHANNELS);
			}
		}
	}
}

void sim_fil_thread(void* arg_list)
{
	UNREFERENCED_PARAMETER(arg_list);

	g_random = new mt19937(g_sim_context.random_seed);

	setjmp(g_jump_buf[SIM_ENTITY_FIL]);

	while (1)
	{
		sim_message_t* msg = sim_receive_message(SIM_ENTITY_FIL);

		if (msg != NULL)
		{
			UINT32 msg_code = msg->code;

			if (msg_code == SIM_EVENT_POWER_ON)
			{
				fil_init(); 								// FIL 초기화
				sim_send_message(msg, SIM_ENTITY_FTL);		// SIM_EVENT_POWER_ON 메시지를 FTL에게 전달

				fil_main();
			}
			else
			{
				sim_release_message_slot(msg);

				if (msg_code == SIM_EVENT_END_SIMULATION)
					break;
			}
		}
	}
}
