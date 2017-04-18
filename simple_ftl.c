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


// �׽�Ʈ�� ���� ������ FTL
// ���� �Ǵ� ���: read, write, trim, flush, GC

#include "droll.h"

#if OPTION_SIMPLE_FTL == TRUE

ftl_cmd_queue_t g_ftl_cmd_q;	// FTL Command Queue
feedback_queue_t g_feedback_q;


#define FREE_BLKS_LOW_THRESHOLD		5


#define WQ_SIZE				POT(SLICES_PER_BIG_WL)
#define DATA_IN_GC_BUF		0x80000000	// flag bit in map entry
#define DO_NOT_GC			0x8000		// valid_slice_count�� ���� ũ�� ���� choose_victim()�� �������� ���ϵ��� ��ġ


typedef enum { FTL_INIT, FTL_UP, FTL_FLUSH, FTL_DOWN } fs_t;

typedef struct
{
	UINT8	valid_slice_bmp[NUM_DIES][BIG_BLKS_PER_DIE][WLS_PER_BLK][SLICES_PER_BIG_WL / 8];
	UINT16	valid_slice_count[NUM_DIES][BIG_BLKS_PER_DIE];
	UINT32	free_blk_bmp[NUM_DIES][DIV_CEIL(BIG_BLKS_PER_DIE, 32)];
	UINT32	num_free_blks[NUM_DIES];

	UINT32	host_open_blk[NUM_DIES];		// �� die���� host write�� ���� open block�� 0�� �Ǵ� 1�� ����
	UINT32	num_host_open_blks;				// host open block�� ���� die�� ����
	UINT32	host_write_wl[NUM_DIES];		// �� host open block���� ������ write�� wordline ��ġ
	ftl_cmd_t write_queue[WQ_SIZE];			// FTL�� ���ؼ� �����ǰ� ó���� �̷������ �ִ� ��ɵ�
	UINT32	wq_front;
	UINT32	wq_rear;
	UINT32	num_wq_slices;					// write queue�� �׿� �ִ� write data ũ���� ����
	UINT32	num_wq_partial_slices;			// write queue�� �׿� �ִ� partial slice ������ ����

	UINT32	gc_open_blk[NUM_DIES];			// GC open block�� �ּ� (�� die���� GC write�� ���� open block�� 0�� �Ǵ� 1�� ����)
	UINT32	num_gc_open_blks;				// GC open block�� ���� die�� ����
	UINT32	gc_write_wl[NUM_DIES];			// GC open block ������ write�� wordline ��ġ - gc_wrte()���� ó���� ������ ������ ����
	UINT32	victim_blk[NUM_DIES];			// victim block�� �ּ� (�� die���� victim�� 0�� �Ǵ� 1�� ����)
	UINT32	num_victim_blks;				// victim block�� ���� die�� ����
	UINT32	gc_count_target[NUM_DIES];		// victim block���� �о�� �� valid slice�� ����
	UINT32	gc_count[NUM_DIES];				// victim block���� ���� valid slice�� ���� (gc_read()���� ó���� ������ ���ݾ� ����)
	UINT32	gc_scan_wl[NUM_DIES];			// victim block���� ó���� ������ ��ġ - gc_read()���� ó���� ������ ���ݾ� ������ ����
	UINT32	gc_scan_page[NUM_DIES];			// victim block���� ó���� ������ ��ġ - gc_read()���� ó���� ������ ���ݾ� ������ ����
	UINT32	gc_pending_slices;				// NAND read�� �Ϸ�ǰ� ���� gc_write()�� ���� ó������ ���� slice�� ����
	UINT32	gc_read_count;
	UINT32	gc_write_count;

	UINT32	gc_open_die;
	UINT32	host_open_die;
	UINT32	host_write_die;
	UINT32	gc_read_die;
	UINT32	gc_write_die;

	fs_t	ftl_state;
	UINT32	merge_lsa;
	BOOL8	internal_flush;
	UINT8	unused[3];

	temp_buf_t*	gc_buf_list;				// NAND read�� �Ϸ�ǰ� ���� gc_write()�� ���� ó������ ���� slice�� circular linked list

} ftl_context_t;

static ftl_context_t g_ftl_context;

SANITY_CHECK(SLICES_PER_BIG_PAGE == 8);		// valid_slice_bmp
SANITY_CHECK(SLICES_PER_BIG_WL % 8 == 0);	// valid_slice_bmp

void ftl_open(void)
{
	if (g_sim_context.session == 2)
	{
		printf("simple FTL�� power cycling�� �������� ����\n");
		CHECK(FAIL);
	}

	ftl_context_t* fc = &g_ftl_context;

	fc->gc_buf_list = &(g_dram.dummy);
	g_dram.dummy.next_slot = fc->gc_buf_list;

	FTL_STAT(reset_ftl_statistics());
	NAND_STAT(reset_nand_statistics());

	fc->ftl_state = FTL_INIT;
}

static void spend_time(void)
{
	sim_message_t* msg = sim_receive_message(SIM_ENTITY_FTL);

	if (msg != NULL)
	{
		ASSERT(msg->code == SIM_EVENT_PRINT_STAT);

		FTL_STAT(print_ftl_statistics());
		FTL_STAT(reset_ftl_statistics());

		BUF_STAT(print_buf_statistics());
		BUF_STAT(reset_buf_statistics());

		sim_send_message(msg, SIM_ENTITY_FIL);
	}
}

static BOOL8 handle_flash_result(BOOL8 call_from_top)
{
	ftl_context_t* fc = &g_ftl_context;
	flash_interface_t* fi = &g_flash_interface;
	BOOL8 did_something = FALSE;

	while (1)
	{
		if (fi->rq_front == V32(fi->rq_rear))	// Result Queue�� �����
		{
			return did_something;
		}

		UINT32 cmd_id = fi->result_queue[fi->rq_front++ % RESULT_QUEUE_SIZE];
		flash_cmd_t* done_command = fi->table + cmd_id;

		ASSERT(cmd_id < FLASH_CMD_TABLE_SIZE && done_command->status == CS_DONE);

		switch (done_command->fop_code)
		{
			case FOP_READ_GC:
			{
				// gc_read()�� ���ؼ� GC buffer slot���� circular linked list�� �����Ǿ� �ְ�, �ű⿡ NAND read data�� ����ִ�.
				// cmd->buf_slot_id�� �� ����Ʈ�� ������ ��带 ����Ų��.
				// �Ʒ��� �ڵ�� �� ����Ʈ(A)��, ������ �׿� �ִ� GC buffer slot��� �̷���� circular linked list (B)�� �߰��Ѵ�.
				// g_ftl_context.gc_buf_list�� B�� ������ ��� (�ֽ� ���)�� ����Ų��.
				// B�� gc_write()�� ���ؼ� ���ݾ� ó���ǰ� �پ���.

				read_userdata_t* cmd = (read_userdata_t*) done_command;
				temp_buf_t* buf = bm_buf_ptr(cmd->buf_slot_id);			// A�� ������ ���
				fc->gc_buf_list->next_slot = buf->next_slot;			// A�� ù��° ��带 B�� �޺κп� �߰�
				buf->next_slot= &g_dram.dummy;
				fc->gc_buf_list = buf;									// A�� ������ ���

				fc->gc_pending_slices += cmd->num_slices;

				break;
			}
			case FOP_WRITE_GC:
			{
				write_userdata_t* cmd = (write_userdata_t*) done_command;
				temp_buf_t* buf_ptr = bm_buf_ptr(cmd->buf_slot_id); 		// gc_write()�� ���ؼ� ������� circular linked list�� ������ ���
				bm_release_gc_buf(buf_ptr, cmd->num_slices);				// circular linked list�� ������ ��带 �Ѱ��ش�.
				break;
			}
			default:
			{
				CHECK(FAIL);
			}
		}

		done_command->status = CS_FREE;

		MUTEX_LOCK(&g_cs_flash_cmd_table);

		V32(done_command->next_slot) = fi->free_slot_index;
		V32(fi->free_slot_index) = cmd_id;

		ASSERT(fi->num_commands != 0);
		V32(fi->num_commands) = fi->num_commands - 1;

		MUTEX_UNLOCK(&g_cs_flash_cmd_table);

		did_something = TRUE;

		if (call_from_top == FALSE)
		{
			return did_something;	// ���� ���ϰ� �� ���� ������ ���̻� result ó������ ���� ���ư�
		}
	}
}

static flash_cmd_t* new_flash_cmd(UINT32* cmd_id)
{
	while (V32(g_flash_interface.num_commands) == FLASH_CMD_TABLE_SIZE)
	{
		// ���̺� �� ĭ�� �����Ƿ� ���� ��ɵ��� result ��ٷȴٰ� ó���ؾ� ��

		if (handle_flash_result(FALSE) == FALSE)
		{
			spend_time();
		}
	}

	MUTEX_LOCK(&g_cs_flash_cmd_table);

	g_flash_interface.num_commands++;

	FTL_STAT(g_ftl_stat.flash_table_high = MAX(g_ftl_stat.flash_table_high, g_flash_interface.num_commands));

	UINT32 slot_index = g_flash_interface.free_slot_index;
	ASSERT(slot_index < FLASH_CMD_TABLE_SIZE);

	flash_cmd_t* cmd = g_flash_interface.table + slot_index;
	g_flash_interface.free_slot_index = cmd->next_slot;

	MUTEX_UNLOCK(&g_cs_flash_cmd_table);

	*cmd_id = slot_index;

	ASSERT(cmd->status == CS_FREE);
	cmd->status = CS_WAITING;

	return cmd;
}

static void issue_flash_cmd(UINT32 die, UINT32 cmd_id)
{
	flash_cmd_queue_t* queue = g_flash_interface.flash_cmd_q + die;

	while (queue->rear - V32(queue->front) == FLASH_QUEUE_SIZE)
	{
		handle_flash_result(FALSE);	// �� ȣ���ؾ� �ϴ� ���� �ƴ�
		spend_time();
	}

	V8(queue->queue[queue->rear % FLASH_QUEUE_SIZE]) = (UINT8) cmd_id;
	queue->rear++;
	V32(queue->rear) = queue->rear;

	FTL_STAT(g_ftl_stat.flash_q_high = MAX(g_ftl_stat.flash_q_high, queue->rear - queue->front));

	sim_wake_up(SIM_ENTITY_FIL);
}

static void finish_flash_cmd(UINT32 cmd_id)
{
	while (1)
	{
		if (g_flash_interface.table[cmd_id].status == CS_FREE)
		{
			break;
		}

		spend_time();

		handle_flash_result(FALSE);
	}
}

static UINT32 get_free_blk(UINT32 die)
{
	ASSERT(die < NUM_DIES);

	ASSERT(g_ftl_context.num_free_blks[die] != 0);
	g_ftl_context.num_free_blks[die]--;

	UINT32 blk_index = mu_bmp_search(g_ftl_context.free_blk_bmp[die], BIG_BLKS_PER_DIE);
	ASSERT(blk_index < BIG_BLKS_PER_DIE);

	mu_clear_bit((UINT8*) g_ftl_context.free_blk_bmp[die], blk_index);

	UINT32 cmd_id;
	erase_t* cmd = (erase_t*) new_flash_cmd(&cmd_id);

	cmd->fop_code = FOP_ERASE;
	cmd->flag = 0;
	cmd->blk_index = blk_index;

	issue_flash_cmd(die, cmd_id);

	FTL_STAT(g_ftl_stat.num_erased_blocks++);

	return blk_index;
}

static void release_blk(UINT32 die, UINT32 blk_index)
{
	ASSERT(die < NUM_DIES && blk_index < BIG_BLKS_PER_DIE);

	if (blk_index == g_ftl_context.victim_blk[die])
	{
		// �� �Լ��� gc_write()�� ���ؼ� ȣ��� ��, �밳�� ��� g_ftl_context.victim_blk[die] ���� �̹� gc_read()�� ���ؼ� FF32�� �ǰų� ���ο� ������ �ٲ�� �ִ�.
		// ������ ���� ��Ȳ������ ������ ���ɼ��� �ִ�.
		//
		// 1. gc_scan_page ���� #16���� ������ �����̴�.
		// 2. host write �Ǵ� trim�� ���ؼ� #18���� ������ invalidate�ȴ�.
		// 3. gc_read()�� #17�� �д´�. �̰��� �ش� ���� �� �ϳ� ���� valid data�̴�.
		// 4. gc_write()���� #17�� ó���ϰ� ���� valid_slice_count�� 0�� �ǹǷ� release_blk() �Ѵ�.
		// 5. �ش� ���� �ٽ� open �ǰ� host write�� ���ؼ� #0���� #18���� ��������.
		// 6. gc_read()�� #18�� �д´�. (�� ���̿� victim�� �̹� release �ǰ� �ٽ� open �Ǿ ä�����ٴ� ����� �𸥴�.)
		//
		// �Ʒ��� �ڵ�� victim�� release �Ǿ��ٴ� ����� gc_read()���� �˷��ֱ� ���� ���̴�.

		g_ftl_context.victim_blk[die] = FF32;
		g_ftl_context.num_victim_blks--;
	}

	g_ftl_context.valid_slice_count[die][blk_index] = DO_NOT_GC;	// choose_victim()�� free block�� �������� �ʴ´�.

	mu_set_bit((UINT8*) g_ftl_context.free_blk_bmp[die], blk_index);

	ASSERT(g_ftl_context.num_free_blks[die] < BIG_BLKS_PER_DIE);
	g_ftl_context.num_free_blks[die]++;
}

static BOOL8 choose_victim(void)
{
	BOOL8 did_something = FALSE;

	ftl_context_t* fc = &g_ftl_context;

	if (fc->num_victim_blks == NUM_DIES)
	{
		return FALSE;
	}

	for (UINT32 die = 0; die < NUM_DIES; die++)
	{
		if (fc->num_free_blks[die] >= FREE_BLKS_LOW_THRESHOLD || fc->victim_blk[die] != FF32)
		{
			continue;
		}

		UINT32 num_valid_slices;
		UINT32 blk_index = mu_search(fc->valid_slice_count[die], MU_MIN_16, BIG_BLKS_PER_DIE, NULL, &num_valid_slices);

		if (num_valid_slices == SLICES_PER_BIG_BLK)
		{
			continue;
		}

		FTL_STAT(g_ftl_stat.num_victims++);
		FTL_STAT(g_ftl_stat.gc_cost_sum += num_valid_slices);
		FTL_STAT(g_ftl_stat.gc_cost_min = MIN(g_ftl_stat.gc_cost_min, num_valid_slices));
		FTL_STAT(g_ftl_stat.gc_cost_max = MAX(g_ftl_stat.gc_cost_max, num_valid_slices));

		ASSERT(num_valid_slices < SLICES_PER_BIG_BLK);

		if (num_valid_slices == 0)
		{
			release_blk(die, blk_index);
		}
		else
		{
			ASSERT(fc->num_victim_blks < NUM_DIES);
			fc->num_victim_blks++;

			fc->victim_blk[die] = blk_index;
			fc->gc_scan_wl[die] = 0;
			fc->gc_scan_page[die] = 0;
			fc->gc_count_target[die] = num_valid_slices;
			fc->gc_count[die] = 0;
			fc->valid_slice_count[die][blk_index] = (UINT16) (num_valid_slices | DO_NOT_GC);
		}

		did_something = TRUE;
	}

	return did_something;
}

static BOOL8 gc_read(void)
{
	ftl_context_t* fc = &g_ftl_context;

	if (fc->num_victim_blks == 0)
	{
		return FALSE;	// �� ���� ����
	}
	else if (bm_query_gc_buf() < SLICES_PER_BIG_PAGE)
	{
		return FALSE;	// GC ���۰� ������ ���ɼ��� �����Ƿ� ���� �� �� ����
	}
	else if (fc->ftl_state == FTL_FLUSH && (fc->num_wq_slices == 0 || fc->num_host_open_blks != 0))
	{
		return FALSE;	// ������ GC�� �� ���� �ƴ�
	}

 	UINT32 die = fc->gc_read_die;
	UINT32 victim_blk_index;

	while (1)
	{
		die = (die + 1) % NUM_DIES;

		victim_blk_index = fc->victim_blk[die];

		if (victim_blk_index != FF32)
			break;
	}

	fc->gc_read_die = die;

	UINT32 wl_index = fc->gc_scan_wl[die];	// big block ������ big wordline�� ��ȣ
	UINT32 lcm = fc->gc_scan_page[die];		// big WL ������ big page�� ��ȣ (0=L, 1=C, 2=M)
	UINT32 bitmap;							// big page ������ �� slice�� �� bit��

	ASSERT(wl_index < WLS_PER_BLK && lcm < PAGES_PER_WL);

	// ���� big page ���� (valid slice�� �ϳ� �̻� ���Ե� big page)

	while (1)
	{
		bitmap = fc->valid_slice_bmp[die][victim_blk_index][wl_index][lcm];		// �ϳ��� big page ������ slice ���� bitmap

		if (bitmap != 0)
		{
			break;
		}

		if (++lcm == PAGES_PER_WL)
		{
			lcm = 0;

			if (++wl_index == WLS_PER_BLK)
			{
				// gc_count�� gc_count_target�� �������� ���������� �ұ��ϰ� scan �������� ���� ������ �Դ�.
				// victim�� �ִ� �Ϻ� slice�� host write�� ���ؼ� invalidate �Ǹ� �̷� ��Ȳ�� �߻��Ѵ�.

				fc->victim_blk[die] = FF32;
				fc->num_victim_blks--;

				if (fc->gc_count[die] == 0)
				{
					// �� victim block���� ���� slice�� �ϳ��� �����Ƿ�, �� victim�� gc_write()���� release�� �� ����.
					release_blk(die, victim_blk_index);
				}

				return TRUE;

			}
		}
	}

	// GC ���� �Ҵ� �� ���� ���� ���·� ����

	UINT32 num_slices_to_read = 0;
	UINT32 bmp_backup = bitmap;
	UINT32 psa = psa_encode(die, victim_blk_index, wl_index, lcm * SLICES_PER_BIG_PAGE);	// big page�� ù��° slice�� PSA

	temp_buf_t* buf_ptr = g_bm.free_gc_buf;
	temp_buf_t* last_buf_ptr = buf_ptr;
	temp_buf_t* first_buf_ptr = buf_ptr;

	while (1)
	{
		UINT32 slice = find_one(bitmap);						// big page �������� slice ��ȣ

		if (slice == 32)
		{
			break;
		}

		bitmap &= ~BIT(slice);

		num_slices_to_read++;

		buf_ptr->psa = psa + slice;								// gc_write() ���� ����� ����

		last_buf_ptr = buf_ptr;

		buf_ptr = buf_ptr->next_slot;
	}

	g_bm.free_gc_buf = buf_ptr;
	g_bm.num_free_gc_buf_slots -= num_slices_to_read;

	BUF_STAT(g_buf_stat.gc_buf_high = MAX(g_buf_stat.gc_buf_high, NUM_GCBUF_SLOTS - g_bm.num_free_gc_buf_slots));

	last_buf_ptr->next_slot = first_buf_ptr;					// FIL���� �Ѱ��ֱ� ���� circular linked list ���·� ���� (handle_flash_result()���� while loop�� ������ �ʱ� ����)


	// FIL���� �б� ��� ������

	UINT32 cmd_id;
	read_userdata_t* cmd = (read_userdata_t*) new_flash_cmd(&cmd_id);

	cmd->fop_code = FOP_READ_GC;
	cmd->flag = NF_NOTIFY;
	cmd->psa = psa;
	cmd->buf_slot_id = (UINT16) bm_buf_id(last_buf_ptr);			// circular linked list�� ������ ���
	cmd->slice_bmp = (UINT8) bmp_backup;
	cmd->num_slices = (UINT8) num_slices_to_read;

	issue_flash_cmd(die, cmd_id);

	FTL_STAT(g_ftl_stat.num_gc_read_pages++);

	// ������ ȣ���� ���� ����

	fc->gc_read_count += num_slices_to_read;	// gc_read() �Լ��� ���� �뵿��
	fc->gc_count[die] += num_slices_to_read;	// current victim�� ���� gc_read()���� ó���� slice�� ���� (���ο� victim�� �����ϸ� 0���� �ٽ� ����)

	ASSERT(fc->gc_count[die] <= fc->gc_count_target[die]);

	if (fc->gc_count[die] == fc->gc_count_target[die])
	{
		fc->victim_blk[die] = FF32;
		fc->num_victim_blks--;
	}
	else
	{
		if (++lcm == PAGES_PER_WL)
		{
			lcm = 0;

			if (++wl_index == WLS_PER_BLK)
			{
				fc->victim_blk[die] = FF32;
				fc->num_victim_blks--;
			}
		}

		fc->gc_scan_wl[die] = wl_index;			// �������� gc_read()�� ȣ���ϸ� �� ��ġ�������� scan�� ����
		fc->gc_scan_page[die] = lcm;
	}

	return TRUE;
}

static BOOL8 gc_write(void)
{
	// gc_read()�� �ѹ� ȣ�⿡ �ϳ��� big page�� ������, gc_write()�� �ѹ� ȣ�⿡ �ϳ��� big wordline�� ����.

	ftl_context_t* fc = &g_ftl_context;

	if (fc->gc_read_count == fc->gc_write_count)
	{
		return FALSE;	// �� ���� ����
	}
	else if (fc->num_gc_open_blks == 0)
	{
		return FALSE;	// ���� �� �� ����
	}
	else if (fc->gc_pending_slices < SLICES_PER_BIG_WL)
	{
		return FALSE;	// �� ���� �ʹ� �����Ƿ� �������� �̷�
	}

	UINT32 write_blk_index;
	UINT32 die = fc->gc_write_die;

	while (1)
	{
		die = (die + 1) % NUM_DIES;

		write_blk_index = fc->gc_open_blk[die];

		if (write_blk_index != FF32)
			break;
	}

	fc->gc_write_die = die;

	UINT32 num_processed_slices = 0;				// write �� wordline �������� slice offset
	UINT32 num_invalid_slices = 0;

	temp_buf_t* first_buf = g_dram.dummy.next_slot;		// gc_buf_list (circular linked list) �� ù��° (���� ������) ���
	temp_buf_t* buf_ptr = first_buf;
	UINT32 write_wl = fc->gc_write_wl[die];

	while (1)
	{
		// loop�� �ѹ� �� ������ num_processed_slices �� 1�� �����ϴٰ� SLICES_PER_BIG_WL �� �����ϸ� ����

		UINT32 lsa = buf_ptr->sector[0].lba / SECTORS_PER_SLICE;

		UINT32 read_psa = buf_ptr->psa;					// �� �����Ͱ� ����ִ� NAND �ּ�

		if (g_dram.map[lsa] == read_psa)
		{
			UINT32 read_die, read_blk_index, read_wl_index, read_slice_offset;
			psa_decode(read_psa, &read_die, &read_blk_index, &read_wl_index, &read_slice_offset);

			mu_clear_bit(fc->valid_slice_bmp[read_die][read_blk_index][read_wl_index], read_slice_offset);

			UINT16 valid_slice_count = fc->valid_slice_count[read_die][read_blk_index];
			ASSERT(valid_slice_count > DO_NOT_GC);
			valid_slice_count--;
			fc->valid_slice_count[read_die][read_blk_index] = valid_slice_count;

			if (valid_slice_count == DO_NOT_GC)
			{
				release_blk(read_die, read_blk_index);
				FTL_STAT(g_ftl_stat.num_reclaimed_blks++);
			}

			g_dram.map[lsa] = psa_encode(die, write_blk_index, write_wl, num_processed_slices);

			mu_set_bit(fc->valid_slice_bmp[die][write_blk_index][write_wl], num_processed_slices);
		}
		else
		{
			// gc_read()���� �о ���Ŀ� �� �����Ͱ� host write �Ǵ� trim�� ���ؼ� invalidate �Ǿ���.
			num_invalid_slices++;
		}

		if (++num_processed_slices == SLICES_PER_BIG_WL)
		{
			fc->gc_pending_slices -= num_processed_slices;
			break;
		}

		buf_ptr = buf_ptr->next_slot;
	}

	g_dram.dummy.next_slot = buf_ptr->next_slot;							// �������� ó���� node���� gc_buf_list���� ����

	if (fc->gc_buf_list == buf_ptr)										// ������ ��� ����Ʈ�� �����
	{
		ASSERT(buf_ptr->next_slot == &(g_dram.dummy) && fc->gc_pending_slices == 0);
		fc->gc_buf_list = buf_ptr->next_slot;
	}

	fc->gc_write_count += num_processed_slices;							// gc_write() �Լ��� ���� �뵿��

	buf_ptr->next_slot = first_buf; 									// circular linked list ���·� �����.

	if (num_processed_slices != num_invalid_slices)
	{
		UINT16 valid_slice_count = fc->valid_slice_count[die][write_blk_index];
		UINT32 delta = num_processed_slices - num_invalid_slices;
		ASSERT(valid_slice_count + delta <= (SLICES_PER_BIG_BLK | DO_NOT_GC));
		fc->valid_slice_count[die][write_blk_index] = (UINT16) (valid_slice_count + delta);

		UINT32 cmd_id;
		write_userdata_t* cmd = (write_userdata_t*) new_flash_cmd(&cmd_id);

		cmd->fop_code = FOP_WRITE_GC;
		cmd->flag = NF_NOTIFY;
		cmd->buf_slot_id = (UINT16) bm_buf_id(buf_ptr);				// circular linked list�� ������ ��带 �Ѱ��ش�.
		cmd->num_slices = (UINT8) num_processed_slices;

		issue_flash_cmd(die, cmd_id);

		FTL_STAT(g_ftl_stat.num_gc_written_wls++);
		FTL_STAT(g_ftl_stat.num_gc_written_slices += num_processed_slices);

		fc->gc_write_wl[die]++;

		if (fc->gc_write_wl[die] == WLS_PER_BLK)
		{
			fc->gc_open_blk[die] = FF32;
			fc->num_gc_open_blks--;
			fc->valid_slice_count[die][write_blk_index] &= ~DO_NOT_GC;	// choose_victim()�� ���� ���õǴ� ���� ���
		}
	}
	else
	{
		// valid slice�� ���� ������ ����

		bm_release_gc_buf(buf_ptr, num_processed_slices);				// circular linked list�� ������ ��带 �Ѱ��ش�.
	}

	return TRUE;
}

static BOOL8 open_block(void)
{
	BOOL8 did_something = FALSE;

	ftl_context_t* fc = &g_ftl_context;

	UINT32 die = fc->gc_open_die;

	for (UINT32 i = 0; i < NUM_DIES; i++)
	{
		if (fc->num_gc_open_blks == NUM_DIES)
		{
			break;
		}

		die = (die + 1) % NUM_DIES;

		if (fc->gc_open_blk[die] == FF32 && fc->num_free_blks[die] != 0)
		{
			UINT32 blk_index = get_free_blk(die);
			fc->gc_open_blk[die] = blk_index;
			fc->gc_write_wl[die] = 0;
			fc->num_gc_open_blks++;

			ASSERT(fc->valid_slice_count[die][blk_index] == DO_NOT_GC);

			fc->gc_open_die = die;

			UINT32 cmd_id;
			flash_control_t* cmd = (flash_control_t*) new_flash_cmd(&cmd_id);
			cmd->fop_code = FOP_OPEN_GC;
			cmd->flag = NF_CTRL;
			cmd->arg_32_1 = blk_index;
			issue_flash_cmd(die, cmd_id);

			did_something = TRUE;
		}
	}

	die = fc->host_open_die;

	for (UINT32 i = 0; i < NUM_DIES; i++)
	{
		if (fc->num_host_open_blks == NUM_DIES)
			break;

		die = (die + 1) % NUM_DIES;

		if (fc->host_open_blk[die] == FF32 && fc->num_free_blks[die] >= 2)
		{
			UINT32 blk_index = get_free_blk(die);
			fc->host_open_blk[die] = blk_index;
			fc->host_write_wl[die] = 0;
			fc->num_host_open_blks++;

			ASSERT(fc->valid_slice_count[die][blk_index] == DO_NOT_GC);

			fc->host_open_die = die;

			UINT32 cmd_id;
			flash_control_t* cmd = (flash_control_t*) new_flash_cmd(&cmd_id);
			cmd->fop_code = FOP_OPEN_HOST;
			cmd->flag = NF_CTRL;
			cmd->arg_32_1 = blk_index;
			issue_flash_cmd(die, cmd_id);

			did_something = TRUE;
		}
	}

	return did_something;
}

static UINT32 search_preceding_slices(UINT32 current_slice, UINT32 lsa)
{
	if (current_slice == 0)
	{
		return FF32;
	}

	UINT32 hit_index = mu_search(g_dram.buf_map + SLICES_PER_BIG_WL - current_slice, MU_EQU_32, current_slice, lsa, &lsa);

	if (hit_index == current_slice)
	{
		return FF32;
	}

	return current_slice - hit_index - 1;
}

static void read_merge_fore(UINT32 target_buf_slot_id, UINT32 sector_offset, UINT32 lsa)
{
	// ȣ��Ʈ ����� ���� ��ġ�� slice ��輱�� align �Ǿ� ���� ���� ���
	// sector_offset == 3 �̸� NAND�� �����ϴ� ���� ������ 3 sector�� �о
	// write buffer�� �����ؾ� �Ѵ�. write buffer�� ���� �����̰� �� buffer slot�� ũ��� 1 sector �ε�,
	// HIL�� host data�� �����Ͽ� write buffer�� ä�� ���� 3 sector�� �ǳ� �پ���.
	// NAND �б��� �ּ� ������ 4KB �̹Ƿ�, �ϴ��� ������ �ӽ� ����(temp buffer)�� ���ؼ� �о��
	// copy (temp buffer to write buffer) ������ �ؾ� �Ѵ�.
	// merge�� ������ ���� �ش� ȣ��Ʈ ����� Ȯ��ȴ�.
	// ���� ���, �־��� ȣ��Ʈ ����� lba = 3, sector_count = 13 �̾��ٸ� lba = 0, sector_count = 16 ���� Ȯ��ȴ�.
	// Ȯ���ϰ� �� �ڿ��� ��� ũ�⸦ slice ������ �������� ������ ����.


	UINT32 psa = g_dram.map[lsa];

	if (psa != NULL)
	{
		UINT32 read_die, read_blk_index, read_wl_index, read_slice_offset;
		psa_decode(psa, &read_die, &read_blk_index, &read_wl_index, &read_slice_offset);

		UINT32 cmd_id;
		read_userdata_t* flash_cmd = (read_userdata_t*) new_flash_cmd(&cmd_id);

		flash_cmd->fop_code = FOP_READ_INTERNAL;
		flash_cmd->flag = 0;
		flash_cmd->num_slices = 1;
		flash_cmd->psa = psa & ~(SLICES_PER_BIG_PAGE - 1);
		flash_cmd->buf_slot_id = (UINT16) bm_buf_id(g_dram.merge_buf);
		flash_cmd->slice_bmp = BIT(psa % SLICES_PER_BIG_PAGE);		// big page ������ �������� slice �� bitmap
		flash_cmd->num_starters = NULL;

		issue_flash_cmd(read_die, cmd_id);

		finish_flash_cmd(cmd_id);

		for (UINT32 i = 0; i < sector_offset; i++)
		{
			buf_slot_t* write_buf_ptr = bm_write_buf_ptr(target_buf_slot_id++);

			write_buf_ptr->body[0] = g_dram.merge_buf->sector[i].body[0];
			write_buf_ptr->lba = g_dram.merge_buf->sector[i].lba;
		}
	}
	else
	{
		// NAND���� ���� �����Ͱ� �������� �����Ƿ� zero pattern���� ä���.

		for (UINT32 i = 0; i < sector_offset; i++)
		{
			buf_slot_t* buf_ptr = bm_write_buf_ptr(target_buf_slot_id++);

			buf_ptr->body[0] = 0;

			buf_ptr->lba = lsa * SECTORS_PER_SLICE + i;
		}
	}
}

static void read_merge_aft(UINT32 target_buf_slot_id, UINT32 written_sectors, UINT32 lsa, UINT32 psa)
{
	// ȣ��Ʈ ����� �� ��ġ�� slice ��輱�� align �Ǿ� ���� ���� ��� ó��
	// written_sectors == 5 �̸� NAND�� �����ϴ� ���� ������ SECTORS_PER_SLICE - 5 = 3 sector�� �о
	// write buffer�� �����ؾ� �Ѵ�. write buffer�� ���� �����̰� �� buffer slot�� ũ��� 1 sector �ε�,
	// HIL�� host data�� �����Ͽ� write buffer 5 sector�� ä�� �ڿ� 3 sector�� �ǳ� �پ���. (���� ����� ������ �� �ǳ� �ڴ�.)
	// NAND �б��� �ּ� ������ 4KB �̹Ƿ�, �ϴ��� ������ �ӽ� ����(temp buffer)�� ���ؼ� �о��
	// copy (temp buffer to write buffer) ������ �ؾ� �Ѵ�.

	if (psa != NULL)
	{
		if (lsa != g_ftl_context.merge_lsa)
		{
			// lsa == merge_lsa �̸� read_merge_fore() ���� �̹� ���� slice �̹Ƿ� NAND�� �ٽ� ���� �ʿ䰡 ����.
			// (ȣ��Ʈ ����� ũ�Ⱑ 1 slice ���� ���� ���)

			UINT32 die, blk_index, wl_index, slice_offset;
			psa_decode(psa, &die, &blk_index, &wl_index, &slice_offset);

			UINT32 cmd_id;
			read_userdata_t* flash_cmd = (read_userdata_t*) new_flash_cmd(&cmd_id);

			flash_cmd->fop_code = FOP_READ_INTERNAL;
			flash_cmd->flag = 0;
			flash_cmd->num_slices = 1;
			flash_cmd->psa = psa & ~(SLICES_PER_BIG_PAGE - 1);
			flash_cmd->buf_slot_id = (UINT16) bm_buf_id(g_dram.merge_buf);
			flash_cmd->slice_bmp = BIT(psa % SLICES_PER_BIG_PAGE);		// big page ������ �������� slice �� bitmap
			flash_cmd->num_starters = NULL;

			issue_flash_cmd(die, cmd_id);

			finish_flash_cmd(cmd_id);
		}

		for (UINT32 i = written_sectors; i < SECTORS_PER_SLICE; i++)
		{
			buf_slot_t* write_buf_ptr = bm_write_buf_ptr(target_buf_slot_id++);

			write_buf_ptr->body[0] = g_dram.merge_buf->sector[i].body[0];
			write_buf_ptr->lba = g_dram.merge_buf->sector[i].lba;
		}
	}
	else
	{
		for (UINT32 i = written_sectors; i < SECTORS_PER_SLICE; i++)
		{
			buf_slot_t* buf_ptr = bm_write_buf_ptr(target_buf_slot_id++);

			buf_ptr->body[0] = 0;

			buf_ptr->lba = lsa * SECTORS_PER_SLICE + i;
		}
	}
}

static void receive_write_cmd(ftl_cmd_t* cmd)
{
	// write ����� �ϴ� write queue�� �ְ� ó���� �̷��.
	// ���� ������ ũ�Ⱑ big wordline�� ä�� ��ŭ ����� ���̸� ó���Ѵ�.
	// NAND ���� ���� ���ְ� ���� ���ϸ� �������� big wordline ��ü�� �ѹ��� ��� �ϱ� �����̴�.

	g_ftl_context.write_queue[g_ftl_context.wq_rear++ % WQ_SIZE] = *cmd;				// ����ü�� deep copy (8 byte)

	UINT32 sector_count = (cmd->sector_count == 0) ? 0x10000 : cmd->sector_count;
	UINT32 sector_offset = cmd->lba % SECTORS_PER_SLICE;
	UINT32 slice_count = DIV_CEIL(sector_offset + sector_count, SECTORS_PER_SLICE);		// partial slice���� ������ ����

	g_ftl_context.num_wq_slices += slice_count;											// write queue�� ������ ��ɵ��� slice ������ ����
}

static BOOL8 host_write(BOOL32 flush)
{
	ftl_context_t* fc = &g_ftl_context;

	if (fc->num_wq_slices == 0)
	{
		return FALSE;		// �� ���� ���� ���� (write queue�� ��� ����)
	}
	else if (fc->num_wq_slices < SLICES_PER_BIG_WL && flush == FALSE)
	{
		return FALSE;		// �� ���� �ʹ� �۾Ƽ� �ڷ� �̷� (write ����� ����� �������� �ʾ���)
	}
	else if (fc->num_host_open_blks == 0)
	{
		return FALSE;		// ���� �� �� ��� �ڷ� �̷� (open block�� ��� host data�� write�� �� ����)
	}

	UINT32 num_slices_to_write = MIN(fc->num_wq_slices, SLICES_PER_BIG_WL);		// host_write()�� �����ϱ� ���� ó���� slice�� ����
	UINT32 start_buf_slot_id;

	if (bm_consume_write_data(num_slices_to_write * SECTORS_PER_SLICE, &start_buf_slot_id) == FAIL)
	{
		return FALSE;		// data�� ���� ȣ��Ʈ�κ��� �������� �ʾ���
	}

	ASSERT(start_buf_slot_id % SECTORS_PER_SLICE == 0);

	fc->num_wq_slices -= num_slices_to_write;

	UINT32 blk_index;
	UINT32 die = fc->host_write_die;

	while (1)
	{
		die = (die + 1) % NUM_DIES;

		blk_index = fc->host_open_blk[die]; 	// die �������� big block ��ȣ

		if (blk_index != FF32)					// FF32�̸� �ش� die���� open block�� ���ٴ� �ǹ�
			break;
	}

	fc->host_write_die = die;

	UINT32 wl_index = fc->host_write_wl[die];					// �̹��� write�� wordline
	UINT32 current_slice = 0;									// big wordline �������� slice ��ȣ - �Ʒ� while ���� �ѹ� �� ������ 1�� �����ϴٰ� num_slices_to_write �� �����ϸ� ����
	UINT32 new_psa = psa_encode(die, blk_index, wl_index, 0);	// �̹��� write�� wordline�� ù��° slice�� �ּ�
	UINT32 sector_count = 0, sector_offset, lsa;

	// while loop �� ���� slice �ϳ��� ó���ϴٰ� big wordline �ϳ��� ä��� �����Ѵ�.
	// ȣ��Ʈ ����� ũ�Ⱑ ������ �����ϱ� ���� �� �̻��� ����� ó���ϰ� �ǰ�, ū ����� �־����� �� ó������ ���ϰ� �����ϰ� �ȴ�.

	while (1)
	{
		if (sector_count == 0)
		{
			ftl_cmd_t* ftl_cmd = fc->write_queue + fc->wq_front % WQ_SIZE;

			lsa = ftl_cmd->lba / SECTORS_PER_SLICE;
			sector_count = (ftl_cmd->sector_count == 0) ? 0x10000 : ftl_cmd->sector_count;
			sector_offset = ftl_cmd->lba % SECTORS_PER_SLICE;

			if (sector_offset != 0)
			{
				// ȣ��Ʈ ����� ������ slice ��輱�� align ���� ����

				UINT32 hit_slice = search_preceding_slices(current_slice, lsa);
				UINT32 target_buf_slot_id = start_buf_slot_id + current_slice*SECTORS_PER_SLICE;

				if (hit_slice == FF32)
				{
					read_merge_fore(target_buf_slot_id, sector_offset, lsa);
				}
				else
				{
					UINT32 from_buf_id = start_buf_slot_id + hit_slice*SECTORS_PER_SLICE;
					UINT32 to_buf_id = target_buf_slot_id;

					for (UINT32 i = 0; i < sector_offset; i++)
					{
						buf_slot_t* from = bm_write_buf_ptr(from_buf_id++);
						buf_slot_t* to = bm_write_buf_ptr(to_buf_id++);

						to->body[0] = from->body[0];
						to->lba = from->lba;
					}
				}

				fc->merge_lsa = lsa;

				sector_count += sector_offset;	// ��� Ȯ��
			}
			else
			{
				fc->merge_lsa = FF32;
			}
		}

		#if 0	// debugging
		{
			UINT32 s = MIN(sector_count, SECTORS_PER_SLICE);

			for (UINT32 i = 0; i < s; i++)
			{
				buf_slot_t* buf_ptr = bm_write_buf_ptr(start_buf_slot_id + current_slice*SECTORS_PER_SLICE + i);
				ASSERT(buf_ptr->lba == lsa*SECTORS_PER_SLICE + i);
			}
		}
		#endif

		UINT32 old_psa = g_dram.map[lsa];
		UINT32 old_die, old_blk_index, old_wl_index, old_slice_offset;

		if (old_psa != NULL)
		{
			psa_decode(old_psa, &old_die, &old_blk_index, &old_wl_index, &old_slice_offset);	// �ش� LSA�� ���� �ּ�

			mu_clear_bit(fc->valid_slice_bmp[old_die][old_blk_index][old_wl_index], old_slice_offset);

			UINT16 valid_slice_count = fc->valid_slice_count[old_die][old_blk_index];
			ASSERT((valid_slice_count & ~DO_NOT_GC) != 0);
			fc->valid_slice_count[old_die][old_blk_index] = valid_slice_count - 1;
		}

		mu_set_bit(fc->valid_slice_bmp[die][blk_index][wl_index], current_slice);

		UINT16 valid_slice_count = fc->valid_slice_count[die][blk_index];
		ASSERT(valid_slice_count < (SLICES_PER_BIG_BLK | DO_NOT_GC));
		fc->valid_slice_count[die][blk_index] = (UINT16) (valid_slice_count + 1);

		g_dram.map[lsa] = new_psa + current_slice;

		g_dram.buf_map[SLICES_PER_BIG_WL - 1 - current_slice] = lsa;	// mu_search()�� ����Ͽ� ���� �ֱ��� �׸��� ã�� ���� ���������� ���

		if (sector_count > SECTORS_PER_SLICE)
		{
			sector_count -= SECTORS_PER_SLICE;
			lsa++;
		}
		else
		{
			if (sector_count < SECTORS_PER_SLICE)
			{
				// ȣ��Ʈ ����� ���� slice ��輱�� align ���� ����

				UINT32 hit_slice = search_preceding_slices(current_slice, lsa);
				UINT32 target_buf_slot_id = start_buf_slot_id + current_slice*SECTORS_PER_SLICE + sector_count;

				if (hit_slice == FF32)
				{
					read_merge_aft(target_buf_slot_id, sector_count, lsa, old_psa);
				}
				else
				{
					UINT32 from_buf_id = start_buf_slot_id + hit_slice*SECTORS_PER_SLICE + sector_count;
					UINT32 to_buf_id = target_buf_slot_id;

					for (UINT32 i = sector_count; i < SECTORS_PER_SLICE; i++)
					{
						buf_slot_t* from = bm_write_buf_ptr(from_buf_id++);
						buf_slot_t* to = bm_write_buf_ptr(to_buf_id++);

						to->body[0] = from->body[0];
						to->lba = from->lba;
					}
				}
			}

			// ���� ������� �Ѿ

			fc->wq_front++;

			sector_count = 0;
		}

		if (++current_slice == num_slices_to_write)
		{
			if (sector_count != 0)
			{
				// ���� ó�����̴� ȣ��Ʈ ����� ���������� ���ϰ� ���� �κ� - ������ host_write() �Լ��� �������� ���� ��� ����

				ftl_cmd_t* ftl_cmd = fc->write_queue + fc->wq_front % WQ_SIZE;
				ftl_cmd->lba = lsa * SECTORS_PER_SLICE;
				ftl_cmd->sector_count = (UINT16) sector_count;
			}

			break;
		}
	}

	UINT32 cmd_id;
	write_userdata_t* flash_cmd = (write_userdata_t*) new_flash_cmd(&cmd_id);

	flash_cmd->fop_code = FOP_WRITE_HOST;
	flash_cmd->flag = 0;
	flash_cmd->buf_slot_id = (UINT16) start_buf_slot_id;
	flash_cmd->num_slices = (UINT8) num_slices_to_write;

	issue_flash_cmd(die, cmd_id);

	fc->host_write_wl[die] = wl_index + 1;

	if (fc->host_write_wl[die] == WLS_PER_BLK)
	{
		fc->host_open_blk[die] = FF32;
		fc->num_host_open_blks--;
		fc->valid_slice_count[die][blk_index] &= ~DO_NOT_GC;	// choose_victim()�� ���ؼ� victim���� �����Ǵ� ���� ���
	}

	FTL_STAT(g_ftl_stat.num_user_written_wls++);
	FTL_STAT(g_ftl_stat.num_user_written_slices += num_slices_to_write);

	return TRUE;
}

static void host_read(ftl_cmd_t* ftl_cmd)
{
	UINT32 sector_count = (ftl_cmd->sector_count == 0) ? 0x10000 : ftl_cmd->sector_count;
	UINT32 sector_offset = ftl_cmd->lba % SECTORS_PER_SLICE;
	UINT32 num_remaining_slices = DIV_CEIL(sector_offset + sector_count, SECTORS_PER_SLICE);
	UINT32 lsa = ftl_cmd->lba / SECTORS_PER_SLICE;	// Logical Slice Address
	UINT32 next_psa = FF32;
	UINT32 num_slices_together = 0;
	UINT8 slice_bmp = 0;
	UINT8 ncq_tag = ftl_cmd->ncq_tag;

	FTL_STAT(g_ftl_stat.num_user_read_slices += num_remaining_slices);

	UINT32 count_down = (sector_offset == 0) ? (DATA_FIS_SIZE_MAX / SECTORS_PER_SLICE) : ((DATA_FIS_SIZE_MAX / SECTORS_PER_SLICE) + 1);
	count_down = MIN(count_down, num_remaining_slices);					// read ����� ù��° DATA FIS�� ���ϴ� �����̽� ����
	V8(g_flash_interface.count_down[ncq_tag]) = (UINT8) count_down;		// FOP_READ_HOST �� �Ϸ�� ������ FIL�� finish_command()�� ���ؼ� ���ݾ� ����

	do
	{
		UINT32 psa = (next_psa == FF32) ? g_dram.map[lsa] : next_psa;	// Physical Slice Address

		UINT32 die, blk_index, wl_index, slice_offset;
		psa_decode(psa, &die, &blk_index, &wl_index, &slice_offset);

		BOOL32 together = FALSE;

		if (num_remaining_slices > 1)
		{
			next_psa = g_dram.map[lsa + 1];

			if (next_psa == psa + 1 && next_psa/SLICES_PER_BIG_PAGE == psa/SLICES_PER_BIG_PAGE)
			{
				together = TRUE;
			}
		}

		num_slices_together++;

		slice_bmp |= 1 << (slice_offset % SLICES_PER_BIG_PAGE);

		if (together == FALSE)
		{
			UINT32 buf_slot_id, cmd_id;

			while (bm_alloc_read_buf(num_slices_together * SECTORS_PER_SLICE, &buf_slot_id) == FAIL)
			{
				handle_flash_result(FALSE);
				spend_time();
			}

			// starter�� read ����� ù��° DATA FIS�� ���ԵǴ� slice�� ���Ѵ�.
			// �Ϲ������� DATA FIS�� 8KB �̰� slice�� 4KB �̹Ƿ� 2���� starter�� ���ؼ� NAND �бⰡ �Ϸ�Ǹ� ȣ��Ʈ���� DATA FIS ������ ������ �� �ִ�. (send_read_data()�� ���� ����)
			// sector_offset != 0 �� ��쿡�� 3���� starter�� �ʿ��ϴ�.
			// FOP_READ_HOST�� �Ϸ�Ǵ� ������ �ұ�Ģ���̹Ƿ�, ������ big page �ȿ� �� ���� starter�� ���ԵǾ� �ִ��� ����ص� �ʿ䰡 �ִ�.
			// flash_cmd->num_starters �� �׷��� �������� ���ȴ�.

			UINT32 num_starters;

			if (count_down == 0)
			{
				num_starters = 0;
			}
			else
			{
				num_starters = MIN(count_down, num_slices_together);
				count_down -= num_starters;
			}

			// trimmed slice�� ���ؼ��� psa == NULL && num_slices_together == 1 �̴�.
			// �ϴ� FIL���� ������ ����� ����.

			read_userdata_t* flash_cmd = (read_userdata_t*) new_flash_cmd(&cmd_id);

			UINT32 addr = (psa != NULL) ? (psa & ~(SLICES_PER_BIG_PAGE - 1)) : (BIT(31) | lsa);		// PSA ���� LSA ���� ������ bit 31�� ���ؼ� ����

			flash_cmd->fop_code = FOP_READ_HOST;
			flash_cmd->flag = 0;
			flash_cmd->ncq_tag = ncq_tag;								// HIL�� ���� ����
			flash_cmd->psa = addr;
			flash_cmd->buf_slot_id = (UINT16) buf_slot_id;
			flash_cmd->slice_bmp = slice_bmp;							// big page ������ �������� slice ���� bitmap
			flash_cmd->num_starters = (UINT8) num_starters; 			// NF_NOTIFY�� �����Ƿ� num_starters �� ���� ��ó���� FIL ���

			issue_flash_cmd(die, cmd_id);

			FTL_STAT(g_ftl_stat.num_user_read_pages++);

			num_slices_together = 0;
			slice_bmp = 0;
		}

		lsa++;

	} while (--num_remaining_slices != 0);
}

static void trim(ftl_cmd_t* cmd)
{
	ftl_context_t* fc = &g_ftl_context;

	UINT32 lsa_1 = DIV_CEIL(cmd->lba, SECTORS_PER_SLICE);
	UINT32 lsa_2 = (cmd->lba + cmd->sector_count) / SECTORS_PER_SLICE;
	UINT32 num_slices = lsa_2 - lsa_1;
	UINT32 lsa = lsa_1;

	while (num_slices-- != 0)
	{
		UINT32 die, blk_index, wl_index, slice_offset;

		psa_decode(g_dram.map[lsa], &die, &blk_index, &wl_index, &slice_offset);

		if (blk_index != NULL)
		{
			// blk_index == NULL �̸� �ش� LSA�� �̹� trim �� ���¿���

			mu_clear_bit(fc->valid_slice_bmp[die][blk_index][wl_index], slice_offset);

			UINT16 valid_slice_count = fc->valid_slice_count[die][blk_index];
			ASSERT((valid_slice_count & ~DO_NOT_GC) != 0);
			fc->valid_slice_count[die][blk_index] = valid_slice_count - 1;

			g_dram.map[lsa] = NULL;
		}

		lsa++;
	}
}

static BOOL8 receive_cmd_from_hil(void)
{
	static UINT32 counter = 0;

	ftl_context_t* fc = &g_ftl_context;

	UINT32 q_front = g_ftl_cmd_q.front;
	ftl_cmd_t* cmd = g_ftl_cmd_q.queue + q_front % FTL_CMD_Q_SIZE;

	switch (cmd->req_code)
	{
		case REQ_HOST_READ:
		{
			if (fc->num_wq_slices != 0)
			{
				fc->ftl_state = FTL_FLUSH;	// �׿� �ִ� write�� ���� �о�� read ����
				fc->internal_flush = TRUE;

				return FALSE;
			}
			else
			{
				PRINT_FTL_CMD("%u%4u READ%12u%5u\n", ++counter, cmd->ncq_tag, cmd->lba / SECTORS_PER_SLICE, cmd->sector_count / SECTORS_PER_SLICE);

				host_read(cmd);
				fc->ftl_state = FTL_UP;
			}

			break;
		}
		case REQ_HOST_WRITE:
		{
			PRINT_FTL_CMD("%u%4u WRIT%12u%5u\n", ++counter, cmd->ncq_tag, cmd->lba / SECTORS_PER_SLICE, cmd->sector_count / SECTORS_PER_SLICE);

			receive_write_cmd(cmd);
			fc->ftl_state = FTL_UP;
			break;
		}
		case REQ_TRIM:
		{
			if (fc->num_wq_slices != 0)
			{
				fc->ftl_state = FTL_FLUSH;	// �׿� �ִ� write�� ���� �о�� trim ����
				fc->internal_flush = TRUE;

				return FALSE;
			}
			else
			{
				PRINT_FTL_CMD("%u%4u TRIM%12u%5u\n", ++counter, cmd->ncq_tag, cmd->lba / SECTORS_PER_SLICE, cmd->sector_count / SECTORS_PER_SLICE);

				trim(cmd);
				send_feedback_to_hil(FB_TRIM_DONE, NULL);
				fc->ftl_state = FTL_UP;
			}

			break;
		}
		case REQ_FAST_FLUSH:
		case REQ_SLOW_FLUSH:
		{
			PRINT_FTL_CMD("%u     FLUSH\n", ++counter);

			if (fc->ftl_state == FTL_UP)
			{
				fc->ftl_state = FTL_FLUSH;
				fc->internal_flush = FALSE;
			}
			else
			{
				ASSERT(fc->ftl_state != FTL_FLUSH);
				send_feedback_to_hil(FB_FLUSH_DONE, NULL);
			}

			break;
		}
		default:
		{
			CHECK(FAIL);
		}
	}

	V32(g_ftl_cmd_q.front) = q_front + 1;

	return TRUE;
}

void ftl_format(void)
{
	ftl_context_t* fc = &g_ftl_context;

	STOSD(fc, 0, sizeof(ftl_context_t));
	STOSD(g_dram.map, NULL, sizeof(g_dram.map));

	for (UINT32 die = 0; die < NUM_DIES; die++)
	{
		fc->host_open_blk[die] = FF32;
		fc->gc_open_blk[die] = FF32;
		fc->victim_blk[die] = FF32;
		fc->num_free_blks[die] = BIG_BLKS_PER_DIE - 1;	// 0�� ���� ������ �������� ��� free block
	}

	fc->gc_open_die = NUM_DIES - 1;
	fc->host_open_die = NUM_DIES - 1;
	fc->host_write_die = NUM_DIES - 1;
	fc->gc_read_die = NUM_DIES - 1;
	fc->gc_write_die = NUM_DIES - 1;

	mu_fill(fc->free_blk_bmp, FF32, sizeof(fc->free_blk_bmp));

	for (UINT32 die = 0; die < NUM_DIES; die++)
	{
		mu_clear_bit((UINT8*) fc->free_blk_bmp[die], 0);		// 0�� ���� ������� ����

		mu_fill(fc->valid_slice_count, ((UINT32) DO_NOT_GC << 16) | DO_NOT_GC, sizeof(fc->valid_slice_count));	// free block�� choose_victim()�� ���� ���õ��� �ʵ��� ��ġ
	}
}

void ftl_main(void)
{
	ftl_context_t* fc = &g_ftl_context;

	while (1)
	{
		BOOL8 did_something = FALSE;

		if (g_ftl_cmd_q.front != V32(g_ftl_cmd_q.rear) && g_ftl_context.wq_rear - g_ftl_context.wq_front < WQ_SIZE)
		{
			FTL_STAT(g_ftl_stat.ftl_q_high = MAX(g_ftl_stat.ftl_q_high, g_ftl_cmd_q.rear - g_ftl_cmd_q.front));
			did_something |= receive_cmd_from_hil();
		}

		if (fc->ftl_state != FTL_DOWN)		// FTL_DOWN ���¿����� ���� ���� ��ȭ�� ����Ű�� ���� ȣ��Ʈ�κ��� �߰� ����� ���⸸ ��ٸ�
		{
			did_something |= host_write(fc->ftl_state == FTL_FLUSH);

			did_something |= open_block();

			did_something |= choose_victim();

			did_something |= gc_read();

			did_something |= gc_write();

			did_something |= handle_flash_result(TRUE);

			if (fc->ftl_state == FTL_FLUSH && fc->num_wq_slices == 0 && V32(g_flash_interface.num_commands) == 0)
			{
				fc->ftl_state = FTL_DOWN;

				if (fc->internal_flush == FALSE)
				{
					send_feedback_to_hil(FB_FLUSH_DONE, NULL);
				}
			}
		}

		if (did_something == FALSE)
		{
			// ���� �ð����� �� �̻� ���������� �� �ϵ� ���� ���� ��ȭ�� ���� - �ܺο��� �޽����� ���⸦ ��ٸ�
			// �ùķ��̼��� �ƴ� ���� �ϵ������� spend_time() �Լ� ȣ�� ��ſ� CPU�� sleep state�� ���� (ȣ��Ʈ ����� ���� �������)

			spend_time();
		}
	}
}

#endif	// OPTION_SIMPLE_FTL

