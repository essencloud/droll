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


// 테스트를 위한 간단한 FTL
// 지원 되는 기능: read, write, trim, flush, GC

#include "droll.h"

#if OPTION_SIMPLE_FTL == TRUE

ftl_cmd_queue_t g_ftl_cmd_q;	// FTL Command Queue
feedback_queue_t g_feedback_q;


#define FREE_BLKS_LOW_THRESHOLD		5


#define WQ_SIZE				POT(SLICES_PER_BIG_WL)
#define DATA_IN_GC_BUF		0x80000000	// flag bit in map entry
#define DO_NOT_GC			0x8000		// valid_slice_count의 값을 크게 만들어서 choose_victim()이 선택하지 못하도록 조치


typedef enum { FTL_INIT, FTL_UP, FTL_FLUSH, FTL_DOWN } fs_t;

typedef struct
{
	UINT8	valid_slice_bmp[NUM_DIES][BIG_BLKS_PER_DIE][WLS_PER_BLK][SLICES_PER_BIG_WL / 8];
	UINT16	valid_slice_count[NUM_DIES][BIG_BLKS_PER_DIE];
	UINT32	free_blk_bmp[NUM_DIES][DIV_CEIL(BIG_BLKS_PER_DIE, 32)];
	UINT32	num_free_blks[NUM_DIES];

	UINT32	host_open_blk[NUM_DIES];		// 각 die마다 host write를 위한 open block이 0개 또는 1개 존재
	UINT32	num_host_open_blks;				// host open block을 가진 die의 개수
	UINT32	host_write_wl[NUM_DIES];		// 각 host open block에서 다음에 write할 wordline 위치
	ftl_cmd_t write_queue[WQ_SIZE];			// FTL에 의해서 접수되고 처리가 미루어지고 있는 명령들
	UINT32	wq_front;
	UINT32	wq_rear;
	UINT32	num_wq_slices;					// write queue에 쌓여 있는 write data 크기의 총합
	UINT32	num_wq_partial_slices;			// write queue에 쌓여 있는 partial slice 개수의 총합

	UINT32	gc_open_blk[NUM_DIES];			// GC open block의 주소 (각 die마다 GC write를 위한 open block이 0개 또는 1개 존재)
	UINT32	num_gc_open_blks;				// GC open block을 가진 die의 개수
	UINT32	gc_write_wl[NUM_DIES];			// GC open block 내에서 write할 wordline 위치 - gc_wrte()에서 처리할 때마다 앞으로 전진
	UINT32	victim_blk[NUM_DIES];			// victim block의 주소 (각 die마다 victim이 0개 또는 1개 존재)
	UINT32	num_victim_blks;				// victim block을 가진 die의 개수
	UINT32	gc_count_target[NUM_DIES];		// victim block에서 읽어야 할 valid slice의 개수
	UINT32	gc_count[NUM_DIES];				// victim block에서 읽은 valid slice의 개수 (gc_read()에서 처리할 때마다 조금씩 증가)
	UINT32	gc_scan_wl[NUM_DIES];			// victim block에서 처리할 페이지 위치 - gc_read()에서 처리할 때마다 조금씩 앞으로 전진
	UINT32	gc_scan_page[NUM_DIES];			// victim block에서 처리할 페이지 위치 - gc_read()에서 처리할 때마다 조금씩 앞으로 전진
	UINT32	gc_pending_slices;				// NAND read가 완료되고 아직 gc_write()에 의해 처리되지 않은 slice의 개수
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

	temp_buf_t*	gc_buf_list;				// NAND read가 완료되고 아직 gc_write()에 의해 처리되지 않은 slice의 circular linked list

} ftl_context_t;

static ftl_context_t g_ftl_context;

SANITY_CHECK(SLICES_PER_BIG_PAGE == 8);		// valid_slice_bmp
SANITY_CHECK(SLICES_PER_BIG_WL % 8 == 0);	// valid_slice_bmp

void ftl_open(void)
{
	if (g_sim_context.session == 2)
	{
		printf("simple FTL은 power cycling을 지원하지 않음\n");
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
		if (fi->rq_front == V32(fi->rq_rear))	// Result Queue가 비었음
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
				// gc_read()에 의해서 GC buffer slot들의 circular linked list가 형성되어 있고, 거기에 NAND read data가 들어있다.
				// cmd->buf_slot_id가 이 리스트의 마지막 노드를 가리킨다.
				// 아래의 코드는 이 리스트(A)를, 기존에 쌓여 있는 GC buffer slot들로 이루어진 circular linked list (B)에 추가한다.
				// g_ftl_context.gc_buf_list는 B의 마지막 노드 (최신 노드)를 가리킨다.
				// B는 gc_write()에 의해서 조금씩 처리되고 줄어든다.

				read_userdata_t* cmd = (read_userdata_t*) done_command;
				temp_buf_t* buf = bm_buf_ptr(cmd->buf_slot_id);			// A의 마지막 노드
				fc->gc_buf_list->next_slot = buf->next_slot;			// A의 첫번째 노드를 B의 뒷부분에 추가
				buf->next_slot= &g_dram.dummy;
				fc->gc_buf_list = buf;									// A의 마지막 노드

				fc->gc_pending_slices += cmd->num_slices;

				break;
			}
			case FOP_WRITE_GC:
			{
				write_userdata_t* cmd = (write_userdata_t*) done_command;
				temp_buf_t* buf_ptr = bm_buf_ptr(cmd->buf_slot_id); 		// gc_write()에 의해서 만들어진 circular linked list의 마지막 노드
				bm_release_gc_buf(buf_ptr, cmd->num_slices);				// circular linked list의 마지막 노드를 넘겨준다.
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
			return did_something;	// 당장 급하게 할 일이 있으니 더이상 result 처리하지 말고 돌아감
		}
	}
}

static flash_cmd_t* new_flash_cmd(UINT32* cmd_id)
{
	while (V32(g_flash_interface.num_commands) == FLASH_CMD_TABLE_SIZE)
	{
		// 테이블에 빈 칸이 없으므로 기존 명령들의 result 기다렸다가 처리해야 함

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
		handle_flash_result(FALSE);	// 꼭 호출해야 하는 것은 아님
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
		// 이 함수가 gc_write()에 의해서 호출될 때, 대개의 경우 g_ftl_context.victim_blk[die] 값이 이미 gc_read()에 의해서 FF32가 되거나 새로운 블럭으로 바뀌어 있다.
		// 다음과 같은 상황에서는 오류의 가능성이 있다.
		//
		// 1. gc_scan_page 값이 #16까지 진행한 상태이다.
		// 2. host write 또는 trim에 의해서 #18부터 끝까지 invalidate된다.
		// 3. gc_read()가 #17을 읽는다. 이것은 해당 블럭에 단 하나 남은 valid data이다.
		// 4. gc_write()에서 #17을 처리하고 나면 valid_slice_count가 0이 되므로 release_blk() 한다.
		// 5. 해당 블럭이 다시 open 되고 host write에 의해서 #0부터 #18까지 쓰여진다.
		// 6. gc_read()가 #18을 읽는다. (그 사이에 victim이 이미 release 되고 다시 open 되어서 채워졌다는 사실을 모른다.)
		//
		// 아래의 코드는 victim이 release 되었다는 사실을 gc_read()에게 알려주기 위한 것이다.

		g_ftl_context.victim_blk[die] = FF32;
		g_ftl_context.num_victim_blks--;
	}

	g_ftl_context.valid_slice_count[die][blk_index] = DO_NOT_GC;	// choose_victim()이 free block을 선택하지 않는다.

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
		return FALSE;	// 할 일이 없음
	}
	else if (bm_query_gc_buf() < SLICES_PER_BIG_PAGE)
	{
		return FALSE;	// GC 버퍼가 부족할 가능성이 있으므로 일을 할 수 없음
	}
	else if (fc->ftl_state == FTL_FLUSH && (fc->num_wq_slices == 0 || fc->num_host_open_blks != 0))
	{
		return FALSE;	// 지금은 GC를 할 때가 아님
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

	UINT32 wl_index = fc->gc_scan_wl[die];	// big block 내에서 big wordline의 번호
	UINT32 lcm = fc->gc_scan_page[die];		// big WL 내에서 big page의 번호 (0=L, 1=C, 2=M)
	UINT32 bitmap;							// big page 내에서 각 slice당 한 bit씩

	ASSERT(wl_index < WLS_PER_BLK && lcm < PAGES_PER_WL);

	// 읽을 big page 결정 (valid slice가 하나 이상 포함된 big page)

	while (1)
	{
		bitmap = fc->valid_slice_bmp[die][victim_blk_index][wl_index][lcm];		// 하나의 big page 내에서 slice 들의 bitmap

		if (bitmap != 0)
		{
			break;
		}

		if (++lcm == PAGES_PER_WL)
		{
			lcm = 0;

			if (++wl_index == WLS_PER_BLK)
			{
				// gc_count가 gc_count_target에 도달하지 못했음에도 불구하고 scan 과정에서 블럭의 끝까지 왔다.
				// victim에 있던 일부 slice가 host write에 의해서 invalidate 되면 이런 상황이 발생한다.

				fc->victim_blk[die] = FF32;
				fc->num_victim_blks--;

				if (fc->gc_count[die] == 0)
				{
					// 이 victim block에서 읽은 slice가 하나도 없으므로, 이 victim을 gc_write()에서 release할 수 없다.
					release_blk(die, victim_blk_index);
				}

				return TRUE;

			}
		}
	}

	// GC 버퍼 할당 및 원형 버퍼 형태로 변경

	UINT32 num_slices_to_read = 0;
	UINT32 bmp_backup = bitmap;
	UINT32 psa = psa_encode(die, victim_blk_index, wl_index, lcm * SLICES_PER_BIG_PAGE);	// big page의 첫번째 slice의 PSA

	temp_buf_t* buf_ptr = g_bm.free_gc_buf;
	temp_buf_t* last_buf_ptr = buf_ptr;
	temp_buf_t* first_buf_ptr = buf_ptr;

	while (1)
	{
		UINT32 slice = find_one(bitmap);						// big page 내에서의 slice 번호

		if (slice == 32)
		{
			break;
		}

		bitmap &= ~BIT(slice);

		num_slices_to_read++;

		buf_ptr->psa = psa + slice;								// gc_write() 에서 사용할 정보

		last_buf_ptr = buf_ptr;

		buf_ptr = buf_ptr->next_slot;
	}

	g_bm.free_gc_buf = buf_ptr;
	g_bm.num_free_gc_buf_slots -= num_slices_to_read;

	BUF_STAT(g_buf_stat.gc_buf_high = MAX(g_buf_stat.gc_buf_high, NUM_GCBUF_SLOTS - g_bm.num_free_gc_buf_slots));

	last_buf_ptr->next_slot = first_buf_ptr;					// FIL에게 넘겨주기 전에 circular linked list 형태로 변경 (handle_flash_result()에서 while loop를 만들지 않기 위해)


	// FIL에게 읽기 명령 보내기

	UINT32 cmd_id;
	read_userdata_t* cmd = (read_userdata_t*) new_flash_cmd(&cmd_id);

	cmd->fop_code = FOP_READ_GC;
	cmd->flag = NF_NOTIFY;
	cmd->psa = psa;
	cmd->buf_slot_id = (UINT16) bm_buf_id(last_buf_ptr);			// circular linked list의 마지막 노드
	cmd->slice_bmp = (UINT8) bmp_backup;
	cmd->num_slices = (UINT8) num_slices_to_read;

	issue_flash_cmd(die, cmd_id);

	FTL_STAT(g_ftl_stat.num_gc_read_pages++);

	// 다음번 호출을 위한 정보

	fc->gc_read_count += num_slices_to_read;	// gc_read() 함수의 누적 노동량
	fc->gc_count[die] += num_slices_to_read;	// current victim에 대해 gc_read()에서 처리한 slice의 개수 (새로운 victim을 선정하면 0에서 다시 시작)

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

		fc->gc_scan_wl[die] = wl_index;			// 다음번에 gc_read()를 호출하면 이 위치에서부터 scan을 진행
		fc->gc_scan_page[die] = lcm;
	}

	return TRUE;
}

static BOOL8 gc_write(void)
{
	// gc_read()는 한번 호출에 하나의 big page를 읽지만, gc_write()는 한번 호출에 하나의 big wordline을 쓴다.

	ftl_context_t* fc = &g_ftl_context;

	if (fc->gc_read_count == fc->gc_write_count)
	{
		return FALSE;	// 할 일이 없음
	}
	else if (fc->num_gc_open_blks == 0)
	{
		return FALSE;	// 일을 할 수 없음
	}
	else if (fc->gc_pending_slices < SLICES_PER_BIG_WL)
	{
		return FALSE;	// 할 일이 너무 작으므로 나중으로 미룸
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

	UINT32 num_processed_slices = 0;				// write 할 wordline 내에서의 slice offset
	UINT32 num_invalid_slices = 0;

	temp_buf_t* first_buf = g_dram.dummy.next_slot;		// gc_buf_list (circular linked list) 의 첫번째 (가장 오래된) 노드
	temp_buf_t* buf_ptr = first_buf;
	UINT32 write_wl = fc->gc_write_wl[die];

	while (1)
	{
		// loop를 한번 돌 때마다 num_processed_slices 가 1씩 증가하다가 SLICES_PER_BIG_WL 에 도달하면 종료

		UINT32 lsa = buf_ptr->sector[0].lba / SECTORS_PER_SLICE;

		UINT32 read_psa = buf_ptr->psa;					// 이 데이터가 들어있던 NAND 주소

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
			// gc_read()에서 읽어낸 직후에 이 데이터가 host write 또는 trim에 의해서 invalidate 되었다.
			num_invalid_slices++;
		}

		if (++num_processed_slices == SLICES_PER_BIG_WL)
		{
			fc->gc_pending_slices -= num_processed_slices;
			break;
		}

		buf_ptr = buf_ptr->next_slot;
	}

	g_dram.dummy.next_slot = buf_ptr->next_slot;							// 이제까지 처리한 node들을 gc_buf_list에서 제거

	if (fc->gc_buf_list == buf_ptr)										// 제거한 결과 리스트가 비었음
	{
		ASSERT(buf_ptr->next_slot == &(g_dram.dummy) && fc->gc_pending_slices == 0);
		fc->gc_buf_list = buf_ptr->next_slot;
	}

	fc->gc_write_count += num_processed_slices;							// gc_write() 함수의 누적 노동량

	buf_ptr->next_slot = first_buf; 									// circular linked list 형태로 만든다.

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
		cmd->buf_slot_id = (UINT16) bm_buf_id(buf_ptr);				// circular linked list의 마지막 노드를 넘겨준다.
		cmd->num_slices = (UINT8) num_processed_slices;

		issue_flash_cmd(die, cmd_id);

		FTL_STAT(g_ftl_stat.num_gc_written_wls++);
		FTL_STAT(g_ftl_stat.num_gc_written_slices += num_processed_slices);

		fc->gc_write_wl[die]++;

		if (fc->gc_write_wl[die] == WLS_PER_BLK)
		{
			fc->gc_open_blk[die] = FF32;
			fc->num_gc_open_blks--;
			fc->valid_slice_count[die][write_blk_index] &= ~DO_NOT_GC;	// choose_victim()에 의해 선택되는 것을 허용
		}
	}
	else
	{
		// valid slice가 전혀 없으면 버림

		bm_release_gc_buf(buf_ptr, num_processed_slices);				// circular linked list의 마지막 노드를 넘겨준다.
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
	// 호스트 명령의 시작 위치가 slice 경계선에 align 되어 있지 않은 경우
	// sector_offset == 3 이면 NAND에 존재하는 기존 데이터 3 sector를 읽어서
	// write buffer에 적재해야 한다. write buffer는 원형 버퍼이고 각 buffer slot의 크기는 1 sector 인데,
	// HIL이 host data를 수신하여 write buffer를 채울 때에 3 sector를 건너 뛰었다.
	// NAND 읽기의 최소 단위는 4KB 이므로, 일단은 별도의 임시 버퍼(temp buffer)를 통해서 읽어내고
	// copy (temp buffer to write buffer) 동작을 해야 한다.
	// merge가 끝나고 나면 해당 호스트 명령은 확장된다.
	// 예를 들어, 주어진 호스트 명령이 lba = 3, sector_count = 13 이었다면 lba = 0, sector_count = 16 으로 확장된다.
	// 확장하고 난 뒤에도 명령 크기를 slice 개수로 따져보면 변함이 없다.


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
		flash_cmd->slice_bmp = BIT(psa % SLICES_PER_BIG_PAGE);		// big page 내에서 읽으려는 slice 의 bitmap
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
		// NAND에서 읽을 데이터가 존재하지 않으므로 zero pattern으로 채운다.

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
	// 호스트 명령의 끝 위치가 slice 경계선에 align 되어 있지 않은 경우 처리
	// written_sectors == 5 이면 NAND에 존재하는 기존 데이터 SECTORS_PER_SLICE - 5 = 3 sector를 읽어서
	// write buffer에 적재해야 한다. write buffer는 원형 버퍼이고 각 buffer slot의 크기는 1 sector 인데,
	// HIL이 host data를 수신하여 write buffer 5 sector를 채운 뒤에 3 sector를 건너 뛰었다. (다음 명령을 시작할 때 건너 뛴다.)
	// NAND 읽기의 최소 단위는 4KB 이므로, 일단은 별도의 임시 버퍼(temp buffer)를 통해서 읽어내고
	// copy (temp buffer to write buffer) 동작을 해야 한다.

	if (psa != NULL)
	{
		if (lsa != g_ftl_context.merge_lsa)
		{
			// lsa == merge_lsa 이면 read_merge_fore() 에서 이미 읽은 slice 이므로 NAND를 다시 읽을 필요가 없다.
			// (호스트 명령의 크기가 1 slice 보다 작은 경우)

			UINT32 die, blk_index, wl_index, slice_offset;
			psa_decode(psa, &die, &blk_index, &wl_index, &slice_offset);

			UINT32 cmd_id;
			read_userdata_t* flash_cmd = (read_userdata_t*) new_flash_cmd(&cmd_id);

			flash_cmd->fop_code = FOP_READ_INTERNAL;
			flash_cmd->flag = 0;
			flash_cmd->num_slices = 1;
			flash_cmd->psa = psa & ~(SLICES_PER_BIG_PAGE - 1);
			flash_cmd->buf_slot_id = (UINT16) bm_buf_id(g_dram.merge_buf);
			flash_cmd->slice_bmp = BIT(psa % SLICES_PER_BIG_PAGE);		// big page 내에서 읽으려는 slice 의 bitmap
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
	// write 명령은 일단 write queue에 넣고 처리를 미룬다.
	// 누적 데이터 크기가 big wordline을 채울 만큼 충분히 쌓이면 처리한다.
	// NAND 공간 낭비를 없애고 성능 저하를 막으려면 big wordline 전체를 한번에 써야 하기 때문이다.

	g_ftl_context.write_queue[g_ftl_context.wq_rear++ % WQ_SIZE] = *cmd;				// 구조체를 deep copy (8 byte)

	UINT32 sector_count = (cmd->sector_count == 0) ? 0x10000 : cmd->sector_count;
	UINT32 sector_offset = cmd->lba % SECTORS_PER_SLICE;
	UINT32 slice_count = DIV_CEIL(sector_offset + sector_count, SECTORS_PER_SLICE);		// partial slice까지 포함한 개수

	g_ftl_context.num_wq_slices += slice_count;											// write queue에 누적된 명령들의 slice 개수의 총합
}

static BOOL8 host_write(BOOL32 flush)
{
	ftl_context_t* fc = &g_ftl_context;

	if (fc->num_wq_slices == 0)
	{
		return FALSE;		// 할 일이 전혀 없음 (write queue가 비어 있음)
	}
	else if (fc->num_wq_slices < SLICES_PER_BIG_WL && flush == FALSE)
	{
		return FALSE;		// 할 일이 너무 작아서 뒤로 미룸 (write 명령이 충분히 누적되지 않았음)
	}
	else if (fc->num_host_open_blks == 0)
	{
		return FALSE;		// 일을 할 수 없어서 뒤로 미룸 (open block이 없어서 host data를 write할 수 없음)
	}

	UINT32 num_slices_to_write = MIN(fc->num_wq_slices, SLICES_PER_BIG_WL);		// host_write()가 리턴하기 전에 처리할 slice의 개수
	UINT32 start_buf_slot_id;

	if (bm_consume_write_data(num_slices_to_write * SECTORS_PER_SLICE, &start_buf_slot_id) == FAIL)
	{
		return FALSE;		// data가 아직 호스트로부터 도착하지 않았음
	}

	ASSERT(start_buf_slot_id % SECTORS_PER_SLICE == 0);

	fc->num_wq_slices -= num_slices_to_write;

	UINT32 blk_index;
	UINT32 die = fc->host_write_die;

	while (1)
	{
		die = (die + 1) % NUM_DIES;

		blk_index = fc->host_open_blk[die]; 	// die 내에서의 big block 번호

		if (blk_index != FF32)					// FF32이면 해당 die에는 open block이 없다는 의미
			break;
	}

	fc->host_write_die = die;

	UINT32 wl_index = fc->host_write_wl[die];					// 이번에 write할 wordline
	UINT32 current_slice = 0;									// big wordline 내에서의 slice 번호 - 아래 while 루프 한번 돌 때마다 1씩 증가하다가 num_slices_to_write 에 도달하면 종료
	UINT32 new_psa = psa_encode(die, blk_index, wl_index, 0);	// 이번에 write할 wordline의 첫번째 slice의 주소
	UINT32 sector_count = 0, sector_offset, lsa;

	// while loop 한 번에 slice 하나씩 처리하다가 big wordline 하나를 채우고 리턴한다.
	// 호스트 명령의 크기가 작으면 리턴하기 전에 둘 이상의 명령을 처리하게 되고, 큰 명령이 주어지면 다 처리하지 못하고 리턴하게 된다.

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
				// 호스트 명령의 시작이 slice 경계선에 align 되지 않음

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

				sector_count += sector_offset;	// 명령 확장
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
			psa_decode(old_psa, &old_die, &old_blk_index, &old_wl_index, &old_slice_offset);	// 해당 LSA의 기존 주소

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

		g_dram.buf_map[SLICES_PER_BIG_WL - 1 - current_slice] = lsa;	// mu_search()를 사용하여 가장 최근의 항목을 찾기 위해 역방향으로 기록

		if (sector_count > SECTORS_PER_SLICE)
		{
			sector_count -= SECTORS_PER_SLICE;
			lsa++;
		}
		else
		{
			if (sector_count < SECTORS_PER_SLICE)
			{
				// 호스트 명령의 끝이 slice 경계선에 align 되지 않음

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

			// 다음 명령으로 넘어감

			fc->wq_front++;

			sector_count = 0;
		}

		if (++current_slice == num_slices_to_write)
		{
			if (sector_count != 0)
			{
				// 현재 처리중이던 호스트 명령을 마무리하지 못하고 남은 부분 - 다음에 host_write() 함수에 진입했을 때에 계속 진행

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
		fc->valid_slice_count[die][blk_index] &= ~DO_NOT_GC;	// choose_victim()에 의해서 victim으로 지정되는 것을 허용
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
	count_down = MIN(count_down, num_remaining_slices);					// read 명령의 첫번째 DATA FIS에 속하는 슬라이스 개수
	V8(g_flash_interface.count_down[ncq_tag]) = (UINT8) count_down;		// FOP_READ_HOST 가 완료될 때마다 FIL의 finish_command()에 의해서 조금씩 감소

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

			// starter는 read 명령의 첫번째 DATA FIS에 포함되는 slice를 말한다.
			// 일반적으로 DATA FIS는 8KB 이고 slice는 4KB 이므로 2개의 starter에 대해서 NAND 읽기가 완료되면 호스트에게 DATA FIS 전송을 시작할 수 있다. (send_read_data()의 설명 참고)
			// sector_offset != 0 인 경우에는 3개의 starter가 필요하다.
			// FOP_READ_HOST가 완료되는 순서는 불규칙적이므로, 각각의 big page 안에 몇 개의 starter가 포함되어 있는지 기억해둘 필요가 있다.
			// flash_cmd->num_starters 가 그러한 목적으로 사용된다.

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

			// trimmed slice에 대해서는 psa == NULL && num_slices_together == 1 이다.
			// 일단 FIL에게 무조건 명령을 전달.

			read_userdata_t* flash_cmd = (read_userdata_t*) new_flash_cmd(&cmd_id);

			UINT32 addr = (psa != NULL) ? (psa & ~(SLICES_PER_BIG_PAGE - 1)) : (BIT(31) | lsa);		// PSA 인지 LSA 인지 구분은 bit 31에 의해서 가능

			flash_cmd->fop_code = FOP_READ_HOST;
			flash_cmd->flag = 0;
			flash_cmd->ncq_tag = ncq_tag;								// HIL을 위한 정보
			flash_cmd->psa = addr;
			flash_cmd->buf_slot_id = (UINT16) buf_slot_id;
			flash_cmd->slice_bmp = slice_bmp;							// big page 내에서 읽으려는 slice 들의 bitmap
			flash_cmd->num_starters = (UINT8) num_starters; 			// NF_NOTIFY가 없으므로 num_starters 에 대한 뒤처리는 FIL 담당

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
			// blk_index == NULL 이면 해당 LSA는 이미 trim 된 상태였음

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
				fc->ftl_state = FTL_FLUSH;	// 쌓여 있던 write를 먼저 털어내고 read 시작
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
				fc->ftl_state = FTL_FLUSH;	// 쌓여 있던 write를 먼저 털어내고 trim 시작
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
		fc->num_free_blks[die] = BIG_BLKS_PER_DIE - 1;	// 0번 블럭을 제외한 나머지는 모두 free block
	}

	fc->gc_open_die = NUM_DIES - 1;
	fc->host_open_die = NUM_DIES - 1;
	fc->host_write_die = NUM_DIES - 1;
	fc->gc_read_die = NUM_DIES - 1;
	fc->gc_write_die = NUM_DIES - 1;

	mu_fill(fc->free_blk_bmp, FF32, sizeof(fc->free_blk_bmp));

	for (UINT32 die = 0; die < NUM_DIES; die++)
	{
		mu_clear_bit((UINT8*) fc->free_blk_bmp[die], 0);		// 0번 블럭은 사용하지 않음

		mu_fill(fc->valid_slice_count, ((UINT32) DO_NOT_GC << 16) | DO_NOT_GC, sizeof(fc->valid_slice_count));	// free block이 choose_victim()에 의해 선택되지 않도록 조치
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

		if (fc->ftl_state != FTL_DOWN)		// FTL_DOWN 상태에서는 내부 상태 변화를 일으키지 말고 호스트로부터 추가 명령이 오기만 기다림
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
			// 현재 시각에는 더 이상 내부적으로 할 일도 없고 상태 변화도 없음 - 외부에서 메시지가 오기를 기다림
			// 시뮬레이션이 아닌 실제 하드웨어에서는 spend_time() 함수 호출 대신에 CPU를 sleep state로 보냄 (호스트 명령이 오면 깨어나도록)

			spend_time();
		}
	}
}

#endif	// OPTION_SIMPLE_FTL

