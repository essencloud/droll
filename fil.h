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


#ifndef FIL_H
#define FIL_H


// Flash Command (FTL로부터 FIL에게 하달)

typedef struct
{
	UINT8	fop_code;								// FOP_READ_HOST, FOP_READ_GC, FOP_READ_INTERNAL
	UINT8	status;									// NS_XXX
	UINT8	flag;
	union { UINT8 ncq_tag; UINT8 num_slices; };		// FOP_READ_HOST의 경우에는 nct_tag

	UINT32	psa;									// 읽으려는 big page의 첫번째 slice

	UINT16	buf_slot_id;							// buffer slot ID
	UINT8	slice_bmp;								// big page 내에서 읽으려는 slice 들의 bitmap
	UINT8	num_starters;							// host_read()의 설명 참고

	// 하나의 명령으로 적게는 slice 하나, 많게는 big page 전체까지 읽음.
	// half page read 및 multiplane read 여부는 slice_bmp 에 근거하여 FIL이 스스로 판단 (FTL은 NF_SMALL 지정하지 않음)
	// psa 는 언제나 big page의 첫번째 slice의 주소

} read_userdata_t;

typedef struct
{
	UINT8	fop_code;		// FOP_READ_METADATA, FOP_READ_BUFFER
	UINT8	status;			// NS_XXX
	UINT8	flag;			// NF_XXX
	UINT8	num_slices;

	UINT32	psa;			// 읽으려는 slice의 주소

	UINT32	dram_addr;

	// half page read 및 multiplane read 여부는 psa 와 num_slices 에 근거하여 FIL이 스스로 판단 (FTL은 NF_SMALL 지정하지 않음)
	// FOP_READ_BUFFER 의 경우에는 flag, num_slices, psa 사용하지 않음 (무조건 big page)

} read_metadata_t;

#if 1
typedef struct // SKKU
{
	UINT8	fop_code;		// FOP_WRITE_HOST or FOP_WRITE_GC
	UINT8	status;			// NS_XXX
	UINT8	flag;			// NF_SMALL 없음.
	UINT8	num_slices;		// 최소 1, 최대 SLICES_PER_BIG_WL

	UINT16	buf_slot_id;	// write buffer slot ID

	UINT16	unused[3];		// FOP_OPEN_XXX 통해 블럭 주소를 미리 FIL에게 알려주었으므로 nand_addr 필드 없음

	// 명령 하나당 big wordline 하나 처리
	// num_slices != SLICES_PER_BIG_WL 이면 부족한 부분은 FIL이 random pattern으로 채움

} write_userdata_t;
#else
typedef struct
{
	UINT8	fop_code;		// FOP_WRITE_HOST or FOP_WRITE_GC
	UINT8	status;			// NS_XXX
	UINT8	flag;			// NF_SMALL 없음.
	UINT8	num_slices;		// 최소 1, 최대 SLICES_PER_BIG_WL

	UINT16	buf_slot_id[BIG_PAGES_PER_BIG_WL];			// L, C, M 각각을 위한 write buffer slot ID  - FOP_WRITE_GC 의 경우에는 buf_slot_id[0]에 다 들어 있음

	UINT16	unused[4 - BIT_PER_CELL];					// FOP_OPEN_XXX 통해 블럭 주소를 미리 FIL에게 알려주었으므로 nand_addr 필드 없음

														// 명령 하나당 big wordline 하나 처리
														// num_slices != SLICES_PER_BIG_WL 이면 부족한 부분은 FIL이 random pattern으로 채움

} write_userdata_t;
#endif

typedef struct
{
	UINT8	fop_code;		// FOP_WRITE_METADATA, FOP_WRITE_BUFFER
	UINT8	status;			// NS_XXX
	UINT8	flag;			// NF_XXX
	UINT8	num_slices;		// 부족한 slice는 FIL이 random pattern으로 채움

	UINT16	blk_index;		// die 내에서의 big block index 또는 small block index (NF_SMALL 여부에 따라)
	UINT16	wl_index;		// big block 내에서의 wordline 번호

	UINT32	dram_addr;

	// 명령 하나당 big wordline 또는 small wordline 하나 처리 (NF_SMALL 지정 여부에 따라)
	// FOP_WRITE_BUFFER의 경우에는 flag, num_slices, nand_addr 사용하지 않음 (무조건 big page)

} write_metadata_t;

typedef struct
{
	UINT8	fop_code;		// FOP_ERASE
	UINT8	status;
	UINT8	flag;
	UINT8	unused;

	UINT32	blk_index;		// die 내에서의 big block index 또는 small block index (NF_SMALL 여부에 따라)

	UINT32	unused2;

} erase_t;

typedef struct
{
	UINT8	fop_code;
	UINT8	status;
	UINT8	flag;
	UINT8	arg_8;

	UINT32	arg_32_1;

	UINT32	arg_32_2;

} flash_control_t;

typedef union	// union (12 byte per command)
{
	UINT32			w32[3];
	UINT32			next_slot;		// 해당 슬롯이 비어 있을 때에 dword[0]은 linked list of free slots in Flash Command Table

	struct 		// dword[0]은 모든 명령이 동일한 형식을 따름
	{
		UINT8	fop_code;
		UINT8	status;
		UINT8	flag;
		UINT8	unused;
	};

	// 아래는 모두 12바이트짜리 구조체 (FTL이 명령의 상세 정보를 채워서 전달)

	read_userdata_t		read_userdata;
	read_metadata_t		read_metadata;
	write_userdata_t	write_userdata;
	write_metadata_t	write_metadata;
	erase_t				erase;
	flash_control_t 	flash_control;
} flash_cmd_t;

// command code

typedef enum
{
	FOP_READ_HOST,		// 호스트 read 명령
	FOP_READ_GC,		// garbage collection을 위해 user data 읽기
	FOP_READ_METADATA,	// mapping table, journal 등을 읽기
	FOP_READ_MERGE, 	// 호스트 write 명령 (partial slice write를 위한 read-modify-write)
	FOP_READ_INTERNAL,	// user data를 읽어서 호스트에게 보내지 않음 (verify
	FOP_READ_BUFFER,	// NAND cell read 하지 않고 page buffer의 내용을 꺼냄
	FOP_CLASS_READ,

	FOP_WRITE_HOST,		// 호스트 write 명령
	FOP_WRITE_GC,		// garbage collection
	FOP_WRITE_METADATA,	// mapping table, journal 등을 기록
	FOP_WRITE_BUFFER,	// NAND cell program 하지 않고 page buffer까지만 보냄
	FOP_CLASS_WRITE,

	FOP_ERASE,

	FOP_WAKE_UP,			// power on init은 FIL이 스스로 수행하므로 FTL 명령 불필요
	FOP_OPEN_HOST,			// 향후 FOP_WRITE_HOST 에서 사용하게 될 big block number를 FIL에게 미리 알려줌
	FOP_OPEN_GC,			// 향후 FOP_WRITE_GC 에서 사용하게 될 big block number를 FIL에게 미리 알려줌
	FOP_CLASS_CONTROL,

} fop_t;

// flag

#define NF_SMALL			0x01	// single plane write/erase
#define NF_NOTIFY			0x02	// Result Queue를 통해서 FTL에게 결과 통지
#define NF_CTRL				0x04	// FOP_CLASS_CONTROL에 속하는 명령

// command status

#define CS_FREE			0		// 빈 슬롯
#define CS_WAITING		1		// 아직 하드웨어에게 전달되기 전의 상태
#define CS_SUBMITTED	2		// 하드웨어 큐에 들어간 상태
#define CS_DONE			3		// 동작이 성공적으로 완료었으나 아직 FTL에 의하여 마무리되지 않은 상태


#define FLASH_CMD_TABLE_SIZE	(NUM_DIES * 2)				// Flash Command Table에 등록할 수 있는 항목의 최대 개수 - S48 TLC 1TB 기준으로 96개
#define FLASH_QUEUE_SIZE		4							// die 당 comand queue 길이, must be power of two
#define RESULT_QUEUE_SIZE		POT(FLASH_CMD_TABLE_SIZE)

typedef struct
{
	// 각 die 마다 queue 하나씩 존재

	UINT8	queue[FLASH_QUEUE_SIZE];	// queue의 각 항목은 Flash Command Table의 slot 번호
	UINT32	front;
	UINT32	rear;

} flash_cmd_queue_t;

typedef struct
{
	// FTL은 FIL에게 명령을 내리기 위해 다음과 같은 순서를 따른다.
	//
	// 1. Flash Command Table의 빈 슬롯이 생길 때까지 기다린다.
	// 2. Flash Command Table의 빈 슬롯을 할당 받아서 명령 내용을 채운다. Table의 슬롯 번호를 소스 코드에서는 cmd_id 라고 지칭한다.
	// 3. 대상 die의 Flash Command Queue에 빈 칸이 생길 때까지 기다린다.
	// 4. cmd_id 를 Flash Command Queue에 넣어준다.
	// 5. FIL은 die X가 idle 상태이고 해당 die의 Flash Command Queue가 비어 있지 않으면 명령을 꺼내서 Flash Controller 하드웨어에게 전달한다.
	// 6. FIL은 die X가 명령 처리를 마치면 결과를 Result Queue에 넣어준다. 명령에 NF_NOTIFY flag가 없으면 결과를 Result Queue에 넣지 않고 FIL이 직접 Table에서 명령을 제거한다.
	// 7. FTL은 Result Queue를 통해서 결과를 받고 해당 명령을 Table에서 제거한다. 해당 명령이 있던 Table 슬롯은 이제 빈 슬롯이 되었다.
	//
	// A = 모든 die의 Command Queue에 있는 항목의 개수
	// B = FIL에 의해서 접수되고 아직 result가 생성되지 않은 명령의 개수
	// C = Result Queue에 있는 항목의 개수
	// S = Flash Command Table에 등록된 모든 명령의 개수
	//
	// 언제나 S = A + B + C가 성립한다. Result Queue는 Table의 모든 항목을 수용할 수 있을 만큼 크다.
	// 그러므로, Result Queue가 가득 찼다면 Table에 빈 칸이 없다는 것을 의미하고, FTL은 더이상 명령을 생성할 수 없다.
	// FIL은 Result Queue가 넘치는 상황을 고려할 필요가 없다. Result Queue에 빈 칸이 없다면 Table에도 빈 칸이 없어서 FTL이 더이상 명령을 보낼 수 없기 때문이다.
	//
	// FTL은 Table에 빈 칸이 생기기를 기다리는 동안 Result Queue를 처리해야 한다. Result를 처리해야 비로소 Table에 빈 칸이 확보된다.
	//
	// Flash Command Queue는 각 die마다 하나씩 존재하고, 각 Command Queue는 Table 전체를 수용할 만큼 크지 않으므로,
	// Table에 빈 칸을 확보했다고 해서 Command Queue에도 빈 칸이 있다는 것이 보장되지는 않는다.
	// Comand Queue에 넣기 전에 빈 칸이 생길 때까지 기다려야 한다.
	// Result Queue를 처리해야만 Command Queue에 빈 칸이 확보되는 것은 아니다. 시간이 지나면 FIL이 명령을 꺼내고 빈 칸이 생긴다. 하지만 그냥 놀면 뭐하나.


	UINT32	free_slot_index;	// Flash Command Table의 빈 슬롯들로 이루어진 linked list
	UINT32	num_commands;		// Flash Command Table에 등록되어 있는 항목의 개수

	flash_cmd_t table[FLASH_CMD_TABLE_SIZE];		// Flash Command Table; 하나의 테이블을 모든 die가 공유

	flash_cmd_queue_t flash_cmd_q[NUM_DIES];		// 각 die마다 queue 하나씩 존재

	UINT8	result_queue[RESULT_QUEUE_SIZE];		// 동작 완료 순서에 따른 queue; FIL이 FTL에게 결과 통보; 각 항목은 Flash Command Table의 slot 번호
	UINT32	rq_front;
	UINT32	rq_rear;

	UINT8	count_down[NCQ_SIZE];

} flash_interface_t;

extern flash_interface_t g_flash_interface;		// FTL과 FIL 사이의 유일한 소통 수단


#endif	// FIL_H

