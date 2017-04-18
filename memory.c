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


// Buffer Manager

#include "droll.h"

dram_t g_dram;

buf_manager_t g_bm;

void bm_init(void)
{
	STOSD(&g_bm, 0, sizeof(g_bm));
	BUF_STAT(reset_buf_statistics());

	#if OPTION_DRAM_SSD == FALSE
	{
		g_bm.num_free_gc_buf_slots = NUM_GCBUF_SLOTS;

		g_bm.free_gc_buf = g_dram.gc_buf;

		for (UINT32 i = 0; i < NUM_GCBUF_SLOTS; i++)
		{
			g_dram.gc_buf[i].next_slot = &g_dram.gc_buf[i + 1];
		}

		g_dram.merge_buf->next_slot = g_dram.merge_buf;
	}
	#endif
}

BOOL32 bm_export_read_data(UINT8* output, UINT32 num_slots, UINT32 lba)
{
	// ���ۿ� ��� �ִ� �����͸� ���� ȣ��Ʈ���� ����

	UINT32 tx_buf_slot = g_bm.tx_buf_slot;

	for (UINT32 i = 0; i < num_slots; i++)
	{
		if (tx_buf_slot == g_bm.read_alloc_ptr)	// memory.h�� "read buffer�� ���� ����" ����
		{
			return FAIL;
		}

		buf_slot_t* buf_ptr = bm_read_buf_ptr(tx_buf_slot++);

		if (buf_ptr->valid == FALSE)	// �� ���� ���Կ��� NAND read data�� ���� ��� ���� ����.
		{
			return FAIL;
		}

		ASSERT(buf_ptr->lba == lba++);	// OPTION_ASSERT == FALSE �̸� lba++ �������� �����Ƿ� ����
	}

	tx_buf_slot = g_bm.tx_buf_slot;

	while (num_slots-- != 0)
	{
		buf_slot_t* buf_ptr = bm_read_buf_ptr(tx_buf_slot++);

		#if OPTION_VERIFY_DATA
		{
			if (buf_ptr->trimmed == FALSE)
			{
				*output++ = buf_ptr->body[0];	// ���ǻ� �� ���ʹ� �� ����Ʈ��
			}
			else
			{
				*output++ = NULL;
			}
		}
		#else
		{
			UNREFERENCED_PARAMETER(output);
		}
		#endif

		buf_ptr->valid = FALSE;
	}

	g_bm.tx_buf_slot = tx_buf_slot;

	return OK;
}

void bm_discard_read_data(UINT32 num_slots)
{
	// NAND �б��� �⺻ ������ sector�� �ƴ� slice �̹Ƿ�, NAND���� �о �����͸� ȣ��Ʈ���� ������ �ʰ� ������ ��찡 ������

	g_bm.tx_buf_slot += num_slots;

	UINT32 buf_slot_id = g_bm.read_release_ptr;

	while (num_slots-- != 0)
	{
		buf_slot_t* buf_ptr = bm_read_buf_ptr(buf_slot_id++);
		buf_ptr->valid = FALSE;
	}

	V32(g_bm.read_release_ptr) = buf_slot_id;

	sim_wake_up(SIM_ENTITY_FTL);
}

void bm_import_write_data(UINT8* input, UINT32 num_sectors, UINT32 lba)
{
	// HIL�� ȣ��Ʈ�κ��� ������ �����͸� ���ۿ� ����

	UINT32 buf_slot_id = g_bm.rx_buf_slot;

	while (num_sectors-- != 0)
	{
		buf_slot_t* buf_ptr = bm_write_buf_ptr(buf_slot_id++);

		buf_ptr->lba = lba++;

		#if OPTION_VERIFY_DATA
		buf_ptr->body[0] = *input++;	// ���ǻ� �� ���ʹ� �� ����Ʈ��
		#else
		UNREFERENCED_PARAMETER(input);
		#endif
	}

	V32(g_bm.rx_buf_slot) = buf_slot_id;
}

void bm_release_write_buf(UINT32 buf_slot_id, UINT32 num_slots)
{
	BUF_STAT(g_buf_stat.write_buf_high = MAX(g_buf_stat.write_buf_high, V32(g_bm.write_alloc_ptr) - g_bm.write_release_ptr));

	if ((UINT16) buf_slot_id == (UINT16) g_bm.write_release_ptr)
	{
		buf_slot_id = g_bm.write_release_ptr + num_slots;

		while (1)
		{
			if (buf_slot_id == g_bm.ftl_write_ptr)
			{
				break;
			}

			buf_slot_t* buf_ptr = bm_write_buf_ptr(buf_slot_id);

			if (buf_ptr->collect == FALSE)
			{
				break;
			}

			buf_ptr->collect = FALSE;
			buf_slot_id++;
		}

		ASSERT(NUM_WBUF_SLOTS - g_bm.write_alloc_ptr + g_bm.write_release_ptr <= NUM_WBUF_SLOTS);

		V32(g_bm.write_release_ptr) = buf_slot_id;

		sim_wake_up(SIM_ENTITY_HIL);
	}
	else
	{
		// NAND ���� ������ �Ϸ�Ǵ� ������ �������� �ʱ� ������ ���� �ݳ��� �̷���� ������ ������ g_bm.write_release_ptr�� ������ ���� ����.

		for (UINT32 i = 0; i < num_slots; i++)
		{
			buf_slot_t* buf_ptr = bm_write_buf_ptr(buf_slot_id + i);
			ASSERT(buf_ptr->collect == FALSE);
			buf_ptr->collect = TRUE;
		}
	}
}

void bm_release_gc_buf(temp_buf_t* buf_ptr, UINT32 num_slices)
{
	// buf_slot�� circular linked list�� ������ ����̴�.

	BUF_STAT(g_buf_stat.gc_buf_high = MAX(g_buf_stat.gc_buf_high, NUM_GCBUF_SLOTS - g_bm.num_free_gc_buf_slots));

	temp_buf_t* temp = buf_ptr->next_slot;		// free buffer list�� ù��° ��尡 �� ���̴�.
	buf_ptr->next_slot = g_bm.free_gc_buf;		// �־��� circular linked list�� free buffer list�� �� �κп� �߰��Ѵ�.
	g_bm.free_gc_buf = temp;
	g_bm.num_free_gc_buf_slots += num_slices;
}


