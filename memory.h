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
// DRAM 버퍼
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
			UINT8	ncq_tag;	// FOP_READ_HOST 처리를 위해서 FIL이 HIL에게 제공하는 정보
			BOOL8	valid;		// NAND 읽기가 끝나서 해당 버퍼 슬롯에 데이터가 들어있음을 의미
			BOOL8	trimmed;	// TRUE 이면 body 부분에 데이터가 없음 (FOP_READ_HOST 처리를 위해서 FTL이 HIL에게 제공하는 정보)
			UINT8	unused;
		};

		// write buffer

		BOOL32	collect;
	};
} buf_slot_t;

// temp buffer는 GC, slice merge, verify 등의 목적으로 사용된다.
// temp buffer slot의 크기는 1 sector가 아니고 1 slice이다.

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
// DRAM 구획
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
	// read buffer pool과 write buffer pool은 각각 원형 버퍼이다.
	// read buffer pool과 write buffer pool은 분리되어 있고, buffer slot ID 는 각각 0에서 시작한다.
	// read buffer pool에서 첫번째 buffer slot의ID 는 0 이다.
	// write buffer pool에서 첫번째 buffer slot의ID 는 0 이다.
	// 그래서 bm_read_buf_ptr() 함수와 bm_write_buf_ptr() 함수가 따로 존재한다.

	buf_slot_t	read_buf[NUM_RBUF_SLOTS];
	buf_slot_t	write_buf[NUM_WBUF_SLOTS];

	// 아래는 각각의 목적을 위해 서로 분리된 buffer pool 이다.
	// 각 pool 마다 독립적인 buffer slot ID가 부여되지 않고 통일된 ID space를 공유한다.
	// 예를 들어, buffer slot ID = 0 은 GC buffer pool의 첫번째 buffer slot이다.
	// bm_buf_ptr() 함수와 bm_buf_id() 함수가 read/write buffer를 제외한 모든 buffer에 대해서 통용된다.
	// gc_buf가 가장 위에 있어야 한다.

	temp_buf_t	dummy;				// GC buffer의 circular linked list의 head
	temp_buf_t	gc_buf[SLICES_PER_BIG_WL * NUM_DIES];
	temp_buf_t	merge_buf[1];
	temp_buf_t	bbm_buf[SLICES_PER_BIG_WL * NUM_DIES];

	UINT32	map[NUM_LSLICES];				// FTL address mapping table
	UINT32	buf_map[SLICES_PER_BIG_WL];		// unaligned write 처리를 위한 정보

} dram_t;

#endif

extern dram_t g_dram;


///////////////////
// Buffer Manager
///////////////////

// 버퍼 관리자는 독립적인 entity라기 보다는 여러 entity에 의해서 호출되는 함수의 집합이다.
//
// read buffer에 관한 설명
//
// read buffer는 원형 버퍼이다. 버퍼 슬롯 번호를 가리키는 포인터가 다음과 같이 여러 가지 존재한다.
// 1. read_alloc_ptr: 버퍼 할당이 발생할 때마다 증가한다. FTL은 read buffer를 할당 받지 않고 명령을 일단 FIL에게 전달한다.
//      FIL이 FCT 하드웨어에게 명령을 보낼 때에 read buffer 할당이 이루어진다.
//      read_alloc_ptr이 read_release_ptr과 같아지면 read buffer가 소진되었다는 것을 의미한다.
//      버퍼 할당이 대규모로 빠르게 이루어지면 read_alloc_ptr이 원형 버퍼를 한 바퀴 돌아서 read_release_ptr을 따라잡게 되는데,
//      이는 더이상 할당할 버퍼 슬롯이 없다는 것을 의미하므로 앞지를 수는 없다.
// 2. tx_buf_slot: HIL이 호스트에게 데이터 전송을 시작할 때마다 증가한다.
//      tx_buf_slot은 read_alloc_ptr를 뒤에서 따라가고, 앞지를 수는 없다.
//      SATA 전송 속도가 NAND 읽기 속도보다 상대적으로 빠를 수록 tx_buf_slot이 read_alloc_ptr을 더 신속하게 따라잡는다. 두 포인터가 같아지면 호스트에게 전송할 데이터가 없는 것이다.
//      tx_buf_slot과 read_alloc_ptr 사이에 있는 슬롯들은 다음 중의 하나에 속한다.
//           A - 특정 명령을 위해 할당 되었으나 아직 NAND 읽기 동작이 완료되지 않아서 데이터가 들어 있지 않다.
//           B - NAND 읽기 동작이 완료어서 데이터가 들어 있지만 아직 호스트에게 전송이 시작되지 않았다.
//           C - 현재 호스트에게 전송이 진행중이다.
//      SSD에서는 여러 개의 NAND die가 독립적으로 병렬 동작하고 동작 시간이 일정하지 않으므로, A 상태와 B 상태의 슬롯이 무질서하게 섞여 있다.
//      그래서 A 상태 슬롯들과 B 상태 슬롯들의 경계선을 구분하는 하나의 포인터를 정의하는 것이 불가능하다.
//      tx_buf_slot이 증가하다가 A 상태의 슬롯을 만나면 멈추어서 기다리고, B 상태로 바뀌면 진행한다.
// 3. read_release_ptr: read buffer가 반납될 때마다 증가한다. 호스트에게 데이터를 전송하고 나면 반납이 이루어진다.
//      read_release_ptr은 tx_buf_slot을 뒤에서 따라가고, 앞지를 수는 없다.

typedef struct
{
	UINT32	read_alloc_ptr;
	UINT32	tx_buf_slot;
	UINT32	read_release_ptr;

	UINT32	write_alloc_ptr;			// HIL에 의하여 버퍼 할당이 발생할 때마다 증가
	UINT32	rx_buf_slot;				// 호스트로부터 수신한 데이터가 버퍼 슬롯을 채울 때마다 증가
	UINT32	ftl_write_ptr;				// FTL이 FIL에게 명령을 보낼 때마다 증가
	UINT32	write_release_ptr;			// NAND 쓰기가 완료되어 버퍼가 반납될 때마다 증가

	UINT32	num_free_gc_buf_slots;		// 놀고 있는 GC buffer slot 의 개수
	temp_buf_t*	free_gc_buf;			// 놀고 있는 GC buffer slot 들의 linked list

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
	return NUM_WBUF_SLOTS - g_bm.write_alloc_ptr + V32(g_bm.write_release_ptr);	// 현재 놀고 있는 버퍼 슬롯의 개수
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
	UINT32 available = V32(g_bm.rx_buf_slot) - g_bm.ftl_write_ptr;	// write buffer에 쌓여 있고 FTL에 의해서 소비되지 않은 데이터의 양 (섹터 수)

	ASSERT(available <= NUM_WBUF_SLOTS);

	if (available < num_slots)
	{
		// write buffer가 부족하거나 SATA가 느려서 아직 데이터가 도착하지 않았다.
		return FAIL;
	}
	else
	{
		// write data를 FTL이 소비한다고 해서 즉시 write buffer가 반납되는 것은 아니다.
		// NAND 쓰기 동작이 끝나고 나중에 반납될 것이다.

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
