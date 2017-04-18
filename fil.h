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


// Flash Command (FTL�κ��� FIL���� �ϴ�)

typedef struct
{
	UINT8	fop_code;								// FOP_READ_HOST, FOP_READ_GC, FOP_READ_INTERNAL
	UINT8	status;									// NS_XXX
	UINT8	flag;
	union { UINT8 ncq_tag; UINT8 num_slices; };		// FOP_READ_HOST�� ��쿡�� nct_tag

	UINT32	psa;									// �������� big page�� ù��° slice

	UINT16	buf_slot_id;							// buffer slot ID
	UINT8	slice_bmp;								// big page ������ �������� slice ���� bitmap
	UINT8	num_starters;							// host_read()�� ���� ����

	// �ϳ��� ������� ���Դ� slice �ϳ�, ���Դ� big page ��ü���� ����.
	// half page read �� multiplane read ���δ� slice_bmp �� �ٰ��Ͽ� FIL�� ������ �Ǵ� (FTL�� NF_SMALL �������� ����)
	// psa �� ������ big page�� ù��° slice�� �ּ�

} read_userdata_t;

typedef struct
{
	UINT8	fop_code;		// FOP_READ_METADATA, FOP_READ_BUFFER
	UINT8	status;			// NS_XXX
	UINT8	flag;			// NF_XXX
	UINT8	num_slices;

	UINT32	psa;			// �������� slice�� �ּ�

	UINT32	dram_addr;

	// half page read �� multiplane read ���δ� psa �� num_slices �� �ٰ��Ͽ� FIL�� ������ �Ǵ� (FTL�� NF_SMALL �������� ����)
	// FOP_READ_BUFFER �� ��쿡�� flag, num_slices, psa ������� ���� (������ big page)

} read_metadata_t;

#if 1
typedef struct // SKKU
{
	UINT8	fop_code;		// FOP_WRITE_HOST or FOP_WRITE_GC
	UINT8	status;			// NS_XXX
	UINT8	flag;			// NF_SMALL ����.
	UINT8	num_slices;		// �ּ� 1, �ִ� SLICES_PER_BIG_WL

	UINT16	buf_slot_id;	// write buffer slot ID

	UINT16	unused[3];		// FOP_OPEN_XXX ���� �� �ּҸ� �̸� FIL���� �˷��־����Ƿ� nand_addr �ʵ� ����

	// ��� �ϳ��� big wordline �ϳ� ó��
	// num_slices != SLICES_PER_BIG_WL �̸� ������ �κ��� FIL�� random pattern���� ä��

} write_userdata_t;
#else
typedef struct
{
	UINT8	fop_code;		// FOP_WRITE_HOST or FOP_WRITE_GC
	UINT8	status;			// NS_XXX
	UINT8	flag;			// NF_SMALL ����.
	UINT8	num_slices;		// �ּ� 1, �ִ� SLICES_PER_BIG_WL

	UINT16	buf_slot_id[BIG_PAGES_PER_BIG_WL];			// L, C, M ������ ���� write buffer slot ID  - FOP_WRITE_GC �� ��쿡�� buf_slot_id[0]�� �� ��� ����

	UINT16	unused[4 - BIT_PER_CELL];					// FOP_OPEN_XXX ���� �� �ּҸ� �̸� FIL���� �˷��־����Ƿ� nand_addr �ʵ� ����

														// ��� �ϳ��� big wordline �ϳ� ó��
														// num_slices != SLICES_PER_BIG_WL �̸� ������ �κ��� FIL�� random pattern���� ä��

} write_userdata_t;
#endif

typedef struct
{
	UINT8	fop_code;		// FOP_WRITE_METADATA, FOP_WRITE_BUFFER
	UINT8	status;			// NS_XXX
	UINT8	flag;			// NF_XXX
	UINT8	num_slices;		// ������ slice�� FIL�� random pattern���� ä��

	UINT16	blk_index;		// die �������� big block index �Ǵ� small block index (NF_SMALL ���ο� ����)
	UINT16	wl_index;		// big block �������� wordline ��ȣ

	UINT32	dram_addr;

	// ��� �ϳ��� big wordline �Ǵ� small wordline �ϳ� ó�� (NF_SMALL ���� ���ο� ����)
	// FOP_WRITE_BUFFER�� ��쿡�� flag, num_slices, nand_addr ������� ���� (������ big page)

} write_metadata_t;

typedef struct
{
	UINT8	fop_code;		// FOP_ERASE
	UINT8	status;
	UINT8	flag;
	UINT8	unused;

	UINT32	blk_index;		// die �������� big block index �Ǵ� small block index (NF_SMALL ���ο� ����)

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
	UINT32			next_slot;		// �ش� ������ ��� ���� ���� dword[0]�� linked list of free slots in Flash Command Table

	struct 		// dword[0]�� ��� ����� ������ ������ ����
	{
		UINT8	fop_code;
		UINT8	status;
		UINT8	flag;
		UINT8	unused;
	};

	// �Ʒ��� ��� 12����Ʈ¥�� ����ü (FTL�� ����� �� ������ ä���� ����)

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
	FOP_READ_HOST,		// ȣ��Ʈ read ���
	FOP_READ_GC,		// garbage collection�� ���� user data �б�
	FOP_READ_METADATA,	// mapping table, journal ���� �б�
	FOP_READ_MERGE, 	// ȣ��Ʈ write ��� (partial slice write�� ���� read-modify-write)
	FOP_READ_INTERNAL,	// user data�� �о ȣ��Ʈ���� ������ ���� (verify
	FOP_READ_BUFFER,	// NAND cell read ���� �ʰ� page buffer�� ������ ����
	FOP_CLASS_READ,

	FOP_WRITE_HOST,		// ȣ��Ʈ write ���
	FOP_WRITE_GC,		// garbage collection
	FOP_WRITE_METADATA,	// mapping table, journal ���� ���
	FOP_WRITE_BUFFER,	// NAND cell program ���� �ʰ� page buffer������ ����
	FOP_CLASS_WRITE,

	FOP_ERASE,

	FOP_WAKE_UP,			// power on init�� FIL�� ������ �����ϹǷ� FTL ��� ���ʿ�
	FOP_OPEN_HOST,			// ���� FOP_WRITE_HOST ���� ����ϰ� �� big block number�� FIL���� �̸� �˷���
	FOP_OPEN_GC,			// ���� FOP_WRITE_GC ���� ����ϰ� �� big block number�� FIL���� �̸� �˷���
	FOP_CLASS_CONTROL,

} fop_t;

// flag

#define NF_SMALL			0x01	// single plane write/erase
#define NF_NOTIFY			0x02	// Result Queue�� ���ؼ� FTL���� ��� ����
#define NF_CTRL				0x04	// FOP_CLASS_CONTROL�� ���ϴ� ���

// command status

#define CS_FREE			0		// �� ����
#define CS_WAITING		1		// ���� �ϵ����� ���޵Ǳ� ���� ����
#define CS_SUBMITTED	2		// �ϵ���� ť�� �� ����
#define CS_DONE			3		// ������ ���������� �Ϸ������ ���� FTL�� ���Ͽ� ���������� ���� ����


#define FLASH_CMD_TABLE_SIZE	(NUM_DIES * 2)				// Flash Command Table�� ����� �� �ִ� �׸��� �ִ� ���� - S48 TLC 1TB �������� 96��
#define FLASH_QUEUE_SIZE		4							// die �� comand queue ����, must be power of two
#define RESULT_QUEUE_SIZE		POT(FLASH_CMD_TABLE_SIZE)

typedef struct
{
	// �� die ���� queue �ϳ��� ����

	UINT8	queue[FLASH_QUEUE_SIZE];	// queue�� �� �׸��� Flash Command Table�� slot ��ȣ
	UINT32	front;
	UINT32	rear;

} flash_cmd_queue_t;

typedef struct
{
	// FTL�� FIL���� ����� ������ ���� ������ ���� ������ ������.
	//
	// 1. Flash Command Table�� �� ������ ���� ������ ��ٸ���.
	// 2. Flash Command Table�� �� ������ �Ҵ� �޾Ƽ� ��� ������ ä���. Table�� ���� ��ȣ�� �ҽ� �ڵ忡���� cmd_id ��� ��Ī�Ѵ�.
	// 3. ��� die�� Flash Command Queue�� �� ĭ�� ���� ������ ��ٸ���.
	// 4. cmd_id �� Flash Command Queue�� �־��ش�.
	// 5. FIL�� die X�� idle �����̰� �ش� die�� Flash Command Queue�� ��� ���� ������ ����� ������ Flash Controller �ϵ����� �����Ѵ�.
	// 6. FIL�� die X�� ��� ó���� ��ġ�� ����� Result Queue�� �־��ش�. ��ɿ� NF_NOTIFY flag�� ������ ����� Result Queue�� ���� �ʰ� FIL�� ���� Table���� ����� �����Ѵ�.
	// 7. FTL�� Result Queue�� ���ؼ� ����� �ް� �ش� ����� Table���� �����Ѵ�. �ش� ����� �ִ� Table ������ ���� �� ������ �Ǿ���.
	//
	// A = ��� die�� Command Queue�� �ִ� �׸��� ����
	// B = FIL�� ���ؼ� �����ǰ� ���� result�� �������� ���� ����� ����
	// C = Result Queue�� �ִ� �׸��� ����
	// S = Flash Command Table�� ��ϵ� ��� ����� ����
	//
	// ������ S = A + B + C�� �����Ѵ�. Result Queue�� Table�� ��� �׸��� ������ �� ���� ��ŭ ũ��.
	// �׷��Ƿ�, Result Queue�� ���� á�ٸ� Table�� �� ĭ�� ���ٴ� ���� �ǹ��ϰ�, FTL�� ���̻� ����� ������ �� ����.
	// FIL�� Result Queue�� ��ġ�� ��Ȳ�� ����� �ʿ䰡 ����. Result Queue�� �� ĭ�� ���ٸ� Table���� �� ĭ�� ��� FTL�� ���̻� ����� ���� �� ���� �����̴�.
	//
	// FTL�� Table�� �� ĭ�� ����⸦ ��ٸ��� ���� Result Queue�� ó���ؾ� �Ѵ�. Result�� ó���ؾ� ��μ� Table�� �� ĭ�� Ȯ���ȴ�.
	//
	// Flash Command Queue�� �� die���� �ϳ��� �����ϰ�, �� Command Queue�� Table ��ü�� ������ ��ŭ ũ�� �����Ƿ�,
	// Table�� �� ĭ�� Ȯ���ߴٰ� �ؼ� Command Queue���� �� ĭ�� �ִٴ� ���� ��������� �ʴ´�.
	// Comand Queue�� �ֱ� ���� �� ĭ�� ���� ������ ��ٷ��� �Ѵ�.
	// Result Queue�� ó���ؾ߸� Command Queue�� �� ĭ�� Ȯ���Ǵ� ���� �ƴϴ�. �ð��� ������ FIL�� ����� ������ �� ĭ�� �����. ������ �׳� ��� ���ϳ�.


	UINT32	free_slot_index;	// Flash Command Table�� �� ���Ե�� �̷���� linked list
	UINT32	num_commands;		// Flash Command Table�� ��ϵǾ� �ִ� �׸��� ����

	flash_cmd_t table[FLASH_CMD_TABLE_SIZE];		// Flash Command Table; �ϳ��� ���̺��� ��� die�� ����

	flash_cmd_queue_t flash_cmd_q[NUM_DIES];		// �� die���� queue �ϳ��� ����

	UINT8	result_queue[RESULT_QUEUE_SIZE];		// ���� �Ϸ� ������ ���� queue; FIL�� FTL���� ��� �뺸; �� �׸��� Flash Command Table�� slot ��ȣ
	UINT32	rq_front;
	UINT32	rq_rear;

	UINT8	count_down[NCQ_SIZE];

} flash_interface_t;

extern flash_interface_t g_flash_interface;		// FTL�� FIL ������ ������ ���� ����


#endif	// FIL_H

