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


#ifndef MEMORY_H
#define MEMORY_H


///////////////////
// DRAM ����
///////////////////

#define NUM_WBUF_SLOTS		8192
#define NUM_RBUF_SLOTS		2048
#define NUM_GCBUF_SLOTS		(SLICES_PER_BIG_PAGE * NUM_DIES * 3)

typedef struct
{
	UINT8	body[BYTES_PER_SECTOR];					// main data
	UINT32	lba;

	union
	{
		// read buffer

		struct
		{
			UINT8	ncq_tag;	// FOP_READ_HOST ó���� ���ؼ� FIL�� HIL���� �����ϴ� ����
			BOOL8	valid;		// NAND �бⰡ ������ �ش� ���� ���Կ� �����Ͱ� ��������� �ǹ�
			BOOL8	trimmed;	// TRUE �̸� body �κп� �����Ͱ� ���� (FOP_READ_HOST ó���� ���ؼ� FTL�� HIL���� �����ϴ� ����)
			UINT8	unused;
		};

		// write buffer

		BOOL32	collect;
	};
} buf_slot_t;

// temp buffer�� GC, slice merge, verify ���� �������� ���ȴ�.
// temp buffer slot�� ũ��� 1 sector�� �ƴϰ� 1 slice�̴�.

typedef struct
{
	UINT8	body[BYTES_PER_SECTOR];					// main data
	UINT32	lba;

} buf_sector_t;

typedef struct temp_buf
{
	buf_sector_t sector[SECTORS_PER_SLICE];

	UINT32	psa;
	UINT32	unused;

	struct temp_buf* next_slot;

} temp_buf_t;


/////////////////
// DRAM ��ȹ
/////////////////

#if OPTION_DRAM_SSD

typedef struct
{
	buf_slot_t	read_buf[NUM_RBUF_SLOTS];
	buf_slot_t	write_buf[NUM_WBUF_SLOTS];
	UINT8		storage[NUM_LSECTORS];

	union
	{
		temp_buf_t	gc_buf[1];
		temp_buf_t	merge_buf[1];
		temp_buf_t	bbm_buf[1];
	};
} dram_t;

#else

typedef struct
{
	// read buffer pool�� write buffer pool�� ���� ���� �����̴�.
	// read buffer pool�� write buffer pool�� �и��Ǿ� �ְ�, buffer slot ID �� ���� 0���� �����Ѵ�.
	// read buffer pool���� ù��° buffer slot��ID �� 0 �̴�.
	// write buffer pool���� ù��° buffer slot��ID �� 0 �̴�.
	// �׷��� bm_read_buf_ptr() �Լ��� bm_write_buf_ptr() �Լ��� ���� �����Ѵ�.

	buf_slot_t	read_buf[NUM_RBUF_SLOTS];
	buf_slot_t	write_buf[NUM_WBUF_SLOTS];

	// �Ʒ��� ������ ������ ���� ���� �и��� buffer pool �̴�.
	// �� pool ���� �������� buffer slot ID�� �ο����� �ʰ� ���ϵ� ID space�� �����Ѵ�.
	// ���� ���, buffer slot ID = 0 �� GC buffer pool�� ù��° buffer slot�̴�.
	// bm_buf_ptr() �Լ��� bm_buf_id() �Լ��� read/write buffer�� ������ ��� buffer�� ���ؼ� ���ȴ�.
	// gc_buf�� ���� ���� �־�� �Ѵ�.

	temp_buf_t	dummy;				// GC buffer�� circular linked list�� head
	temp_buf_t	gc_buf[SLICES_PER_BIG_WL * NUM_DIES];
	temp_buf_t	merge_buf[1];
	temp_buf_t	bbm_buf[SLICES_PER_BIG_WL * NUM_DIES];

	UINT32	map[NUM_LSLICES];				// FTL address mapping table
	UINT32	buf_map[SLICES_PER_BIG_WL];		// unaligned write ó���� ���� ����

} dram_t;

#endif

extern dram_t g_dram;


///////////////////
// Buffer Manager
///////////////////

// ���� �����ڴ� �������� entity��� ���ٴ� ���� entity�� ���ؼ� ȣ��Ǵ� �Լ��� �����̴�.
//
// read buffer�� ���� ����
//
// read buffer�� ���� �����̴�. ���� ���� ��ȣ�� ����Ű�� �����Ͱ� ������ ���� ���� ���� �����Ѵ�.
// 1. read_alloc_ptr: ���� �Ҵ��� �߻��� ������ �����Ѵ�. FTL�� read buffer�� �Ҵ� ���� �ʰ� ����� �ϴ� FIL���� �����Ѵ�.
//      FIL�� FCT �ϵ����� ����� ���� ���� read buffer �Ҵ��� �̷������.
//      read_alloc_ptr�� read_release_ptr�� �������� read buffer�� �����Ǿ��ٴ� ���� �ǹ��Ѵ�.
//      ���� �Ҵ��� ��Ը�� ������ �̷������ read_alloc_ptr�� ���� ���۸� �� ���� ���Ƽ� read_release_ptr�� ������� �Ǵµ�,
//      �̴� ���̻� �Ҵ��� ���� ������ ���ٴ� ���� �ǹ��ϹǷ� ������ ���� ����.
// 2. tx_buf_slot: HIL�� ȣ��Ʈ���� ������ ������ ������ ������ �����Ѵ�.
//      tx_buf_slot�� read_alloc_ptr�� �ڿ��� ���󰡰�, ������ ���� ����.
//      SATA ���� �ӵ��� NAND �б� �ӵ����� ��������� ���� ���� tx_buf_slot�� read_alloc_ptr�� �� �ż��ϰ� ������´�. �� �����Ͱ� �������� ȣ��Ʈ���� ������ �����Ͱ� ���� ���̴�.
//      tx_buf_slot�� read_alloc_ptr ���̿� �ִ� ���Ե��� ���� ���� �ϳ��� ���Ѵ�.
//           A - Ư�� ����� ���� �Ҵ� �Ǿ����� ���� NAND �б� ������ �Ϸ���� �ʾƼ� �����Ͱ� ��� ���� �ʴ�.
//           B - NAND �б� ������ �Ϸ� �����Ͱ� ��� ������ ���� ȣ��Ʈ���� ������ ���۵��� �ʾҴ�.
//           C - ���� ȣ��Ʈ���� ������ �������̴�.
//      SSD������ ���� ���� NAND die�� ���������� ���� �����ϰ� ���� �ð��� �������� �����Ƿ�, A ���¿� B ������ ������ �������ϰ� ���� �ִ�.
//      �׷��� A ���� ���Ե�� B ���� ���Ե��� ��輱�� �����ϴ� �ϳ��� �����͸� �����ϴ� ���� �Ұ����ϴ�.
//      tx_buf_slot�� �����ϴٰ� A ������ ������ ������ ���߾ ��ٸ���, B ���·� �ٲ�� �����Ѵ�.
// 3. read_release_ptr: read buffer�� �ݳ��� ������ �����Ѵ�. ȣ��Ʈ���� �����͸� �����ϰ� ���� �ݳ��� �̷������.
//      read_release_ptr�� tx_buf_slot�� �ڿ��� ���󰡰�, ������ ���� ����.

typedef struct
{
	UINT32	read_alloc_ptr;
	UINT32	tx_buf_slot;
	UINT32	read_release_ptr;

	UINT32	write_alloc_ptr;			// HIL�� ���Ͽ� ���� �Ҵ��� �߻��� ������ ����
	UINT32	rx_buf_slot;				// ȣ��Ʈ�κ��� ������ �����Ͱ� ���� ������ ä�� ������ ����
	UINT32	ftl_write_ptr;				// FTL�� FIL���� ����� ���� ������ ����
	UINT32	write_release_ptr;			// NAND ���Ⱑ �Ϸ�Ǿ� ���۰� �ݳ��� ������ ����

	UINT32	num_free_gc_buf_slots;		// ��� �ִ� GC buffer slot �� ����
	temp_buf_t*	free_gc_buf;			// ��� �ִ� GC buffer slot ���� linked list

} buf_manager_t;

extern buf_manager_t g_bm;

void bm_init(void);
void bm_import_write_data(UINT8* input, UINT32 num_sectors, UINT32 lba);
void bm_release_write_buf(UINT32 buf_slot_id, UINT32 num_slots);
void bm_discard_read_data(UINT32 num_slots);
BOOL32 bm_export_read_data(UINT8* output, UINT32 num_slots, UINT32 lba);
void bm_release_gc_buf(temp_buf_t* buf_ptr, UINT32 num_slices);

INLINE buf_slot_t* bm_read_buf_ptr(UINT32 buf_slot_id)
{
	return g_dram.read_buf + buf_slot_id%NUM_RBUF_SLOTS;
}

INLINE buf_slot_t* bm_write_buf_ptr(UINT32 buf_slot_id)
{
	return g_dram.write_buf + buf_slot_id%NUM_WBUF_SLOTS;
}

INLINE BOOL32 bm_alloc_read_buf(UINT32 num_slots, UINT32* buf_slot_id)
{
	UINT32 num_busy_slots = g_bm.read_alloc_ptr - V32(g_bm.read_release_ptr);
	UINT32 num_free_slots = NUM_RBUF_SLOTS - num_busy_slots;

	if (num_free_slots < num_slots)
	{
		return FAIL;
	}
	else
	{
		*buf_slot_id = g_bm.read_alloc_ptr;
		g_bm.read_alloc_ptr += num_slots;

		BUF_STAT(g_buf_stat.read_buf_high = MAX(g_buf_stat.read_buf_high, num_busy_slots + num_slots));

		return OK;
	}
}

INLINE void bm_release_read_buf(UINT32 num_slots)
{
	BUF_STAT(g_buf_stat.read_buf_high = MAX(g_buf_stat.read_buf_high, V32(g_bm.read_alloc_ptr) - g_bm.read_release_ptr));

	V32(g_bm.read_release_ptr) += num_slots;

	sim_wake_up(SIM_ENTITY_FTL);
}

INLINE UINT32 bm_query_write_buf(void)
{
	return NUM_WBUF_SLOTS - g_bm.write_alloc_ptr + V32(g_bm.write_release_ptr);	// ���� ��� �ִ� ���� ������ ����
}

INLINE BOOL32 bm_alloc_write_buf(UINT32 num_slots)
{
	UINT32 num_busy_slots = g_bm.write_alloc_ptr - V32(g_bm.write_release_ptr);
	UINT32 num_free_slots = NUM_WBUF_SLOTS - num_busy_slots;

	if (num_free_slots < num_slots)
	{
		return FAIL;
	}
	else
	{
		g_bm.write_alloc_ptr += num_slots;
		BUF_STAT(g_buf_stat.write_buf_high = MAX(g_buf_stat.write_buf_high, num_busy_slots + num_slots));
		return OK;
	}
}

INLINE BOOL32 bm_consume_write_data(UINT32 num_slots, UINT32* buf_slot_id)
{
	UINT32 available = V32(g_bm.rx_buf_slot) - g_bm.ftl_write_ptr;	// write buffer�� �׿� �ְ� FTL�� ���ؼ� �Һ���� ���� �������� �� (���� ��)

	ASSERT(available <= NUM_WBUF_SLOTS);

	if (available < num_slots)
	{
		// write buffer�� �����ϰų� SATA�� ������ ���� �����Ͱ� �������� �ʾҴ�.
		return FAIL;
	}
	else
	{
		// write data�� FTL�� �Һ��Ѵٰ� �ؼ� ��� write buffer�� �ݳ��Ǵ� ���� �ƴϴ�.
		// NAND ���� ������ ������ ���߿� �ݳ��� ���̴�.

		*buf_slot_id = g_bm.ftl_write_ptr;
		g_bm.ftl_write_ptr += num_slots;
		return OK;
	}
}

INLINE void bm_skip_write_buf(UINT32 num_slots)
{
	V32(g_bm.rx_buf_slot) = g_bm.rx_buf_slot + num_slots;
}

INLINE UINT32 bm_query_gc_buf(void)
{
	return g_bm.num_free_gc_buf_slots;
}

INLINE temp_buf_t* bm_buf_ptr(UINT32 buf_slot_id)
{
	return g_dram.gc_buf + buf_slot_id;
}

INLINE UINT32 bm_buf_id(temp_buf_t* buf_ptr)
{
	return (UINT32) (buf_ptr - g_dram.gc_buf);
}

#endif	// MEMORY_H
