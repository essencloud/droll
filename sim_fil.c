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

// NAND flash controller �ϵ����� FIL �߿�� ������ �߻��� �ùķ��̼� ��

typedef enum	// die state
{
	DS_IDLE = 0,

	DS_R1_STALL,	// ä�� �Ҵ��� ��ٸ�
	DS_R2_IO,		// read ��ɰ� �ּҸ� NAND���� ������ ����
	DS_R3_CELL,		// �̸� ������ �ð����� NAND�� ������ �������� (���̴� �߿�� tR ���� �����Ͽ� �̸� ����)
	DS_R4_STALL,	// ä�� �Ҵ��� ��ٸ�
	DS_R5_IO,		// status check ����� ������ NAND internal read�� �������� Ȯ��
	DS_R6_IO,		// �����Ͱ� NAND���� ��Ʈ�ѷ��� ���۵�

	DS_W1_STALL,	// ä�� �Ҵ��� ��ٸ�
	DS_W2_IO,		// write ���, �ּ� �� �����͸� NAND���� ������ ����
	DS_W3_CELL,		// �̸� ������ �ð����� NAND�� ������ �������� (���̴� �߿�� tPROG ���� �����Ͽ� �̸� ����)
	DS_W4_STALL,	// ä�� �Ҵ��� ��ٸ�
	DS_W5_IO,		// status check ����� ������ ready/busy Ȯ��, ready �Ǿ����� pass/fail ���α��� Ȯ��

	DS_E1_STALL,	// ä�� �Ҵ��� ��ٸ�
	DS_E2_IO,		// erase ��ɰ� �ּҸ� NAND���� ������ ����
	DS_E3_CELL,		// �̸� ������ �ð����� NAND�� ������ �������� (���̴� �߿�� tBERS ���� �����Ͽ� �̸� ����)
	DS_E4_STALL,	// ä�� �Ҵ��� ��ٸ�
	DS_E5_IO,		// status check ����� ������ ready/busy Ȯ��, ready �Ǿ����� pass/fail ���α��� Ȯ��

} ds_t;

typedef struct
{
	ds_t			die_state[NUM_DIES];				// flash controller �ϵ��� �ִ� state machine (die ���� ���� state machine �ϳ��� ����)
	flash_cmd_t*	current_cmd[NUM_DIES];				// ���� �������� ���
	UINT64			cmd_begin_time[NUM_DIES];			// ��� ������ ������ �ð�
	UINT32			ch_owner[NUM_CHANNELS];				// ���� ä���� ������� die
	UINT64			ch_release_time[NUM_CHANNELS];		// ä���� �ݳ��� �ð�
	UINT32			num_ch_requests[NUM_CHANNELS];		// STALL ���¿� �ִ� die�� ���� (ä�� ����)
	UINT64			stall_begin_time[NUM_DIES];			// STALL ���°� ���۵� �ð�
	UINT32			host_open_blk[NUM_DIES];			// FOP_OPEN_HOST_BLK ���� FTL�� �˷��� ����
	UINT32			gc_open_blk[NUM_DIES];				// FOP_OPEN_GC_BLK ���� FTL�� �˷��� ����
	UINT32			host_write_wl[NUM_DIES];			// FOP_WRITE_HOST �� ������ ����
	UINT32			gc_write_wl[NUM_DIES];				// FOP_WRITE_GC �� ������ ����
	UINT64			time[NUM_DIES];
	UINT32			num_busy_dies;
	UINT32			unused;

	nand_packet_t	nand_packet[NUM_DIES];

} fil_context_t;

flash_interface_t g_flash_interface;	// FTL�� FIL ������ interface
static fil_context_t g_fil_context;		// FIL ���� ���� ����


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
	// die state machine�� �����̰� ����� �̺�Ʈ

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
		// FTL���� ��� ����

		ASSERT(fi->rq_rear - V32(fi->rq_front) < RESULT_QUEUE_SIZE);	// Ư���� ��ġ�� �ʿ� ������ RESULT_QUEUE_SIZE �� �˳��ϰ� �����

		fi->result_queue[fi->rq_rear % RESULT_QUEUE_SIZE] = (UINT8) cmd_id;

		V8(command->status) = CS_DONE;

		V32(fi->rq_rear) = fi->rq_rear + 1;
	}
	else
	{
		// FIL�� ���� ��ó���ϰ� Flash Command Table ���� ����

		switch (command->fop_code)
		{
			case FOP_READ_HOST:
			{
				read_userdata_t* cmd = (read_userdata_t*) command;

				if (cmd->num_starters != 0)
				{
					UINT32 ncq_tag = cmd->ncq_tag;
					UINT32 count_down = g_flash_interface.count_down[ncq_tag];	// host_read()�� ���ؼ� �ʱ�ȭ�� ��
					ASSERT(count_down >= cmd->num_starters);
					count_down -= cmd->num_starters;
					g_flash_interface.count_down[ncq_tag] = (UINT8) count_down;

					if (count_down == 0)
					{
						hil_notify_read_ready(ncq_tag);							// read ����� ù��° DATA FIS�� ȣ��Ʈ���� ���� �� �ְ� �Ǿ���
					}
				}

				sim_wake_up(SIM_ENTITY_HIL);

				break;
			}
			case FOP_WRITE_HOST:
			{
				write_userdata_t* cmd = (write_userdata_t*)command;
				bm_release_write_buf(cmd->buf_slot_id, cmd->num_slices * SECTORS_PER_SLICE);	// write buffer �ݳ�
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
	// ���� FOP_WRITE_HOST, FOP_WRITE_GC �� ���� ����ϰ� �� ���� ���
	// FOP_WRITE_HOST, FOP_WRITE_GC ���� nand_addr �ʵ尡 ����.

	fil_context_t* fc = &g_fil_context;

	if (cmd->fop_code == FOP_OPEN_HOST)
	{
		ASSERT(fc->host_open_blk[die] == NULL || fc->host_write_wl[die] == WLS_PER_BLK);	// open ���� ����� ���� ���ų� ���� ���� �� ä��

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

	if (fc->ch_owner[ch] != FF32)			// ä�� �����
	{
		return;
	}
	else if (fc->num_ch_requests[ch] == 0)	// ä�� ���䰡 ����
	{
		return;
	}

	// ä�� �Ҵ��� �켱 ����
	// 1. write/erase�� status check (DS_W4_STALL, DS_E4_STALL)
	// 2. read/erase�� ��� �� �ּ� (DS_R1_STALL, DS_E1_STALL)
	// 3. read�� status check �� ������ (DS_R4_STALL)
	// 4. write�� ���, �ּ�, ������ (DS_W1_STALL)
	// ������ �켱 ������ �����ϸ� cmd_begin_time�� �ռ��� die�� ���

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
	UINT32 lba = (cmd->psa & ~BIT(31)) * SECTORS_PER_SLICE; 	// BIT(31)�� 1�̸� PSA�� �ƴϰ� LSA�� �ǹ��Ѵ�.
	UINT32 buf_slot_id = cmd->buf_slot_id;

	for (UINT32 sector_offset = 0; sector_offset < SECTORS_PER_SLICE; sector_offset++)
	{
		buf_slot_t* buf_ptr = bm_read_buf_ptr(buf_slot_id);

		buf_ptr->lba = lba++;
		buf_ptr->ncq_tag = cmd->ncq_tag;
		buf_ptr->valid = TRUE;
		buf_ptr->trimmed = TRUE;						// trimmed == TRUE �̸� HIL�� zero pattern�� ����

		buf_slot_id++;
	}
}

static BOOL8 start_flash_operation(UINT32 die)
{
	fil_context_t* fc = &g_fil_context;
	flash_cmd_queue_t* fcq = g_flash_interface.flash_cmd_q + die;

	BOOL8 did_something = FALSE;

fetch_another_cmd:

	if (fcq->front == V32(fcq->rear))		// �� ���� ����
	{
		return did_something;
	}

	fc->num_busy_dies++;

	did_something = TRUE;

	UINT32 cmd_id = fcq->queue[fcq->front++ % FLASH_QUEUE_SIZE]; 	// FTL�κ��� ��� ����
	flash_cmd_t* cmd = g_flash_interface.table + cmd_id;			// FTL�� ����� �� ������ ���̺� ���� ����.

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
		// ��� ����� �ϴ� STALL ���·� �����Ѵ�. ���� �ð��� ä���� ��ħ ��� �ְ� �켱������ �� ���� die�� ���ٸ�
		// �ð��� ����ϱ� ���� ��� STALL ���¿��� �������� ä�� ����� �����Ѵ�.

		if (cmd->fop_code < FOP_CLASS_READ)
		{
			if (cmd->fop_code == FOP_READ_HOST && (cmd->read_userdata.psa & BIT(31)))	// BIT(31) �̸� trimmed slice �̴�.
			{
				// trimmed slice�� ���ؼ��� �ϴ� FIL���� FOP_READ_HOST ����� �����´�.
				// FTL�� ó���Ϸ��� g_flash_interface.count_down �� ���ؼ� mutex�� �ʿ��ϱ� �����̴�.
				// �밳�� ��� g_flash_interface.count_down �� NAND ������ ���� �ڿ� FIL�� finish_command() ���� �����Ѵ�.
				// trimmed slice�� ���ؼ��� ���ܸ� ������ ���� �����ϰ� ó�������ν� mutex ����� ���ϱ�� �Ѵ�.

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

	channel_arbitration(die % NUM_CHANNELS);	// ä�� �Ҵ� �õ�

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
			ASSERT(slice_offset % SLICES_PER_BIG_PAGE == 0);					// cmd->psa �� �������� big page�� ù��° slice�� �ּ��̾�� �Ѵ�.

			UINT8 lcm = (UINT8) (slice_offset / SLICES_PER_BIG_PAGE);			// 0 = LSB, 1 = CSB, 2 = MSB
			UINT32 slice_bmp = cmd->slice_bmp;
			UINT32 prev_plane = FF32;
			UINT32 buf_slot_id = cmd->buf_slot_id;

			while (1)															// �ѹ��� �� ������ slice �ϳ��� ����
			{
				UINT32 slice = find_one(slice_bmp);								// big page �������� slice ��ȣ

				if (slice == 32)
				{
					break;
				}

				slice_bmp &= ~BIT(slice);
				num_slices++;

				UINT32 plane = slice / SLICES_PER_SMALL_PAGE;
				slice = slice % SLICES_PER_SMALL_PAGE;							// small page �������� slice ��ȣ

				if (plane != prev_plane)
				{
					num_planes++;
					prev_plane = plane;
				}

				SANITY_CHECK(SECTORS_PER_SLICE == 8);

				// �ùķ��̼��� ���Ǹ� ���� main data�� �� ���ʹ� �� ����Ʈ��, �� �����̽��� 8 �����̹Ƿ� UINT64�� ��

				UINT16 small_blk_index = (UINT16) (big_blk_index * SMALL_BLKS_PER_BIG_BLK + plane);

				UINT32 extra_data;		// extra data�� �� �����̽��� 4 ����Ʈ
				UINT64 main_data = sim_nand_read_userdata((UINT8) die, small_blk_index, (UINT16) wl_index, lcm, (UINT8) slice, &extra_data);

				for (UINT32 sector_offset = 0; sector_offset < SECTORS_PER_SLICE; sector_offset++)
				{
					buf_slot_t* buf_ptr = bm_read_buf_ptr(buf_slot_id);

					buf_ptr->body[0] = (UINT8) main_data;
					buf_ptr->lba = extra_data + sector_offset;			// NAND�� ����� ������ 4KB�� �ϳ��� (ù��° ���͸�) ����ϰ�, NAND read �� ���� ��� ���͸��� �ٴ´�. (Marvell FCT�� �ٿ���)
					buf_ptr->ncq_tag = cmd->ncq_tag;					// HIL�� ���� ����
					buf_ptr->valid = TRUE;								// HIL�� ���� ����
					buf_ptr->trimmed = FALSE;							// HIL�� ���� ����

					main_data = main_data >> 8;
					buf_slot_id++;
				}
			}

			break;
		}
		case FOP_READ_INTERNAL:
		case FOP_READ_GC:
		{
			// �Ʒ��� �ڵ忡�� cmd->psa �� cmd->slice_bmp �� ���ְ� buf_ptr->psa �� ����Ͽ� �� ������ �ڵ带 �ۼ��ϴ� ���� �����ϴ�.
			// �׷��� CPU�� buf_ptr->psa �� ������ DRAM read�� ���� stall cycle�� ���� �߻��ϹǷ�, �Ʒ��� �ڵ带 ��ġ�� �ʱ�� �Ѵ�.

			read_userdata_t* cmd = (read_userdata_t*) command;

			UINT32 slice_bmp = cmd->slice_bmp;
			temp_buf_t* buf_ptr = bm_buf_ptr(cmd->buf_slot_id);				// gc_read()�� ���ؼ� ������� circular linked list�� ������ ���
			UINT32 prev_plane = FF32;

			UINT32 _die, big_blk_index, wl_index, slice_offset;
			psa_decode(cmd->psa, &_die, &big_blk_index, &wl_index, &slice_offset);
			ASSERT(_die == die && slice_offset % SLICES_PER_BIG_PAGE == 0);			// cmd->psa �� big page�� ù��° slice�� PSA �̾�� �Ѵ�.

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
			UINT32 slice = slice_offset % SLICES_PER_BIG_PAGE;					// big page �������� slice ��ȣ

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

	UINT64 time = (UINT64) (SIM_NAND_NANOSEC_PER_BYTE * BYTES_PER_SLICE_EX * num_slices);	// data �� random pattern ���ۿ� �ɸ��� �ð�
	time += 1000 * num_planes;																// ���� overhead

	return time;
}

static UINT64 estimate_io_time(flash_cmd_t* command)
{
	// write ��� ó���� �ʿ��� ä�� ���� �ð��� ���

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

	time += NAND_T_TRAN * (PAGES_PER_WL - 1);										// L->C, C->M ��ȯ�� �ɸ��� �ð�
	time += (UINT64) (SIM_NAND_NANOSEC_PER_BYTE * BYTES_PER_SLICE_EX * num_slices);	// data �� random pattern ���ۿ� �ɸ��� �ð�
	time += 1000 + NAND_T_DBSY * num_planes;										// ���, �ּ� ���� �� ��Ÿ overhead

	return time;
}

static void do_write(UINT32 die)
{
	// �� �Լ��� DS_W2_IO ���¿��� DS_W3_CELL ���·� �Ѿ ���� ȣ��ȴ�.

	fil_context_t* fc = &g_fil_context;
	flash_cmd_t* command = fc->current_cmd[die];
	nand_packet_t* nand_pkt = fc->nand_packet + die;	// ��Ʈ�ѷ����� NAND�� ������ data packet

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

			temp_buf_t* buf_ptr = bm_buf_ptr(cmd->buf_slot_id);			// gc_write()�� ���ؼ� ������� circular linked list�� ������ ���

			for (UINT32 slice = 0; slice < cmd->num_slices; slice++)
			{
				buf_ptr = buf_ptr->next_slot;

				*extra_data++ = buf_ptr->sector[0].lba;					// slice�� ù��° ������ LBA ���� �ش� slice�� extra data field�� ���

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

			// cmd->num_slices�� ����Ͽ� ������ �κ��� FIL�� random pattern���� ä���� �ϳ�, �ùķ��̼ǿ����� ����

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
	// �̰��� NAND ������ state machine�� �ƴϰ� flash controller�� state machine�̴�.

	fil_context_t* fc = &g_fil_context;
	UINT64 current_time = g_sim_context.current_time;

	switch (fc->die_state[die])
	{
		case DS_R1_STALL:
		{
			// stall ���¿��ٰ� ä���� �Ҵ� �޾� ���� ���·� �Ѿ

			fc->die_state[die] = DS_R2_IO;
			schedule_next_event(die, 3000);						// ��� �� �ּ� ���ۿ� 3us �ҿ�
			NAND_STAT(g_nand_stat.io_time[die] += 3000);
			break;
		}
		case DS_R2_IO:
		{
			fc->time[die] = do_read(die);						// NAND �б� - do_read()�� ���ϰ��� DS_R6_IO ������ ����

			release_channel(die % NUM_CHANNELS);
			fc->die_state[die] = DS_R3_CELL;
			schedule_next_event(die, NAND_T_R * 19 / 20);		// ����Ǵ� tR�� 95%�� ������� ������ status check ����
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
				schedule_next_event(die, STATUS_CHECK_INTERVAL);		// ���� �ȳ������Ƿ� ��а� �������ξ��ٰ� �ٽ� Ȯ��
			}
			else
			{
				fc->die_state[die] = DS_R6_IO;
				schedule_next_event(die, fc->time[die]);				// fc->time[die]�� do_read()�� ���� ������ �� (������ ũ�⿡ ���� IO �ҿ� �ð�)
				NAND_STAT(g_nand_stat.io_time[die] += STATUS_CHECK_TIME + fc->time[die]);		// NAND ���� ������ ���� ���� ���¿��� status check �� ���� io_time�� �������� ���� (cell_time �̹Ƿ�)
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
			start_flash_operation(die);									// Ȥ�� Flash Command Queue�� ���� ����� ������ ��� ����
			break;
		}
		case DS_W1_STALL:
		{
			fc->die_state[die] = DS_W2_IO;								// NAND���� ���, �ּ�, ������ ������

			UINT64 transfer_time = estimate_io_time(fc->current_cmd[die]);
			schedule_next_event(die, transfer_time);
			NAND_STAT(g_nand_stat.io_time[die] += transfer_time);
			break;
		}
		case DS_W2_IO:
		{
			do_write(die);												// NAND ����

			release_channel(die % NUM_CHANNELS);
			fc->die_state[die] = DS_W3_CELL;
			schedule_next_event(die, NAND_T_PROGO * 19 / 20);			// ����Ǵ� tPROGO�� 95%�� ������� ������ status check ����
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
				schedule_next_event(die, STATUS_CHECK_INTERVAL);		// ���� �ȳ������Ƿ� ��а� �������ξ��ٰ� �ٽ� Ȯ��
			}
			else
			{
				fc->die_state[die] = DS_IDLE;
				NAND_STAT(g_nand_stat.io_time[die] += STATUS_CHECK_TIME);
				NAND_STAT(g_nand_stat.write_time_sum += current_time - fc->cmd_begin_time[die]);
				NAND_STAT(g_nand_stat.num_write_commands++);
				finish_command(fc->current_cmd[die]);
				start_flash_operation(die);								// Ȥ�� Flash Command Queue�� ���� ����� ������ ��� ����
			}

			break;
		}
		case DS_E1_STALL:
		{
			fc->die_state[die] = DS_E2_IO;								// NAND���� ���, �ּ� ������
			schedule_next_event(die, 2000);								// 2us �ҿ� (���� overhead ����)
			NAND_STAT(g_nand_stat.io_time[die] += 2000);
			break;
		}
		case DS_E2_IO:
		{
			do_erase(die);												// NAND �����

			release_channel(die % NUM_CHANNELS);
			fc->die_state[die] = DS_E3_CELL;
			schedule_next_event(die, NAND_T_BERS * 49 / 50);			// ����Ǵ� tBERS�� 98%�� ������� ������ status check ���� (����Ⱑ ������ ���� �������� NAND ���� ����)
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
				schedule_next_event(die, STATUS_CHECK_INTERVAL);		// ���� �ȳ������Ƿ� ��а� �������ξ��ٰ� �ٽ� Ȯ��
			}
			else
			{
				fc->die_state[die] = DS_IDLE;
				NAND_STAT(g_nand_stat.io_time[die] += STATUS_CHECK_TIME);
				NAND_STAT(g_nand_stat.erase_time_sum += current_time - fc->cmd_begin_time[die]);
				NAND_STAT(g_nand_stat.num_erase_commands++);
				finish_command(fc->current_cmd[die]);
				start_flash_operation(die);								// Ȥ�� Flash Command Queue�� ���� ����� ������ ��� ����
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

			UINT32 d = next_die++ % NUM_DIES;	// for loop�� �Ź� die #0���� �����ϴ� ���� ���� ���� �뵵

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
				fil_init(); 								// FIL �ʱ�ȭ
				sim_send_message(msg, SIM_ENTITY_FTL);		// SIM_EVENT_POWER_ON �޽����� FTL���� ����

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
