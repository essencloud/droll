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

#ifndef NAND_H
#define NAND_H


//#define NAND_CONFIG			CFG_P99_240GB	// �ùķ��̼ǿ� ����� NAND ���� - �Ʒ� 12 �� �߿��� ���� �Ǵ� �ʿ信 ���� �߰�
#define NAND_CONFIG			CFG_X49_1TB


///////////////////////
// PCB configuration
///////////////////////

#define CFG_P99_240GB	1
#define CFG_P99_480GB	2
#define CFG_P99_960GB	3

#define CFG_P99_256GB	4
#define CFG_P99_512GB	5
#define CFG_P99_1TB		6

#define CFG_X49_240GB	7
#define CFG_X49_480GB	8
#define CFG_X49_960GB	9

#define CFG_X49_256GB	10
#define CFG_X49_512GB	11
#define CFG_X49_1TB		12


#if NAND_CONFIG == CFG_P99_240GB || NAND_CONFIG == CFG_P99_256GB
#define NAND_PART_NUMBER		HYCRON_P99_TLC		// ��ǰ�� ���� NAND�� ��Ʈ ��ȣ
#define NUM_CHANNELS			4					// ä���� ����, �� ä���� 8bit
#define CES_PER_CHANNEL			1					// ä�δ� CE signal�� ����
#define DIES_PER_CE				1					// CE signal�� die(LUN)�� ����
#define DRAM_SIZE				(256 * 1048576)		// ��ǰ�� ������ DRAM�� ũ�� (byte)
#endif

#if NAND_CONFIG == CFG_P99_480GB || NAND_CONFIG == CFG_P99_512GB
#define NAND_PART_NUMBER		HYCRON_P99_TLC
#define NUM_CHANNELS			4
#define CES_PER_CHANNEL			2
#define DIES_PER_CE				1
#define DRAM_SIZE				(512 * 1048576)
#endif

#if NAND_CONFIG == CFG_P99_960GB || NAND_CONFIG == CFG_P99_1TB
#define NAND_PART_NUMBER		HYCRON_P99_TLC
#define NUM_CHANNELS			4
#define CES_PER_CHANNEL			2
#define DIES_PER_CE				2
#define DRAM_SIZE				(1024 * 1048576)
#endif

#if NAND_CONFIG == CFG_X49_240GB || NAND_CONFIG == CFG_X49_256GB
#define NAND_PART_NUMBER		SAMSHIBA_X49_TLC
#define NUM_CHANNELS			4
#define CES_PER_CHANNEL			1
#define DIES_PER_CE				2
#define DRAM_SIZE				(256 * 1048576)
#endif

#if NAND_CONFIG == CFG_X49_480GB || NAND_CONFIG == CFG_X49_512GB
#define NAND_PART_NUMBER		SAMSHIBA_X49_TLC
#define NUM_CHANNELS			4
#define CES_PER_CHANNEL			2
#define DIES_PER_CE				2
#define DRAM_SIZE				(512 * 1048576)
#endif

#if NAND_CONFIG == CFG_X49_960GB || NAND_CONFIG == CFG_X49_1TB
#define NAND_PART_NUMBER		SAMSHIBA_X49_TLC
#define NUM_CHANNELS			4
#define CES_PER_CHANNEL			4
#define DIES_PER_CE				2
#define DRAM_SIZE				(1024 * 1048576)
#endif



///////////////////////
// NAND specification
///////////////////////

#define SAMSHIBA_X49_TLC	1	// ��ù� 49�� 3D TLC 256Gb NAND
#define HYCRON_P99_TLC		2	// ����ũ�� 99�� 3D TLC 512Gb NAND

#define SLC		1
#define MLC		2
#define	TLC		3

#define LSB		0
#define CSB		1
#define MSB		2

#if NAND_PART_NUMBER == SAMSHIBA_X49_TLC

	// ������ �����ϴ� Ư�� ȸ���� ��ǰ���� ���� ��� �����Ƿ� ������ ��ġ�� ����

	#define BIT_PER_CELL				TLC
	#define SLICES_PER_SMALL_PAGE		4
	#define PAGES_PER_BLK				768
	#define SMALL_BLKS_PER_PLANE		1500
	#define PLANES_PER_DIE				2
	#define SPARE_BYTES_PER_SMALL_PAGE	128

	#define NAND_T_R				80000		// 80us
	#define NAND_T_PROGO			2000000		// 2ms
	#define NAND_T_BERS				4000000		// 4ms
	#define NAND_T_TRAN				20000		// 20us (max) - write data ���۽ÿ� L->C �� C->M �Ѿ ���� �ҿ� �ð� (MSB �ڿ��� tTRAN �����ϰ� �ٷ� tPROGO)
	#define NAND_T_DBSY				500			// 500ns (max) - big page ������ ���� plane���� �Ѿ ���� �ҿ� �ð� (read �� write)

#endif

#if NAND_PART_NUMBER == HYCRON_P99_TLC

	// ������ �����ϴ� Ư�� ȸ���� ��ǰ���� ���� ��� �����Ƿ� ������ ��ġ�� ����

	#define BIT_PER_CELL				TLC
	#define SLICES_PER_SMALL_PAGE		2
	#define PAGES_PER_BLK				2100
	#define SMALL_BLKS_PER_PLANE		1100
	#define PLANES_PER_DIE				4
	#define SPARE_BYTES_PER_SMALL_PAGE	256

	#define NAND_T_R				70000		// 70us
	#define NAND_T_PROGO			2000000		// 2ms
	#define NAND_T_BERS				4000000		// 4ms
	#define NAND_T_TRAN				20000		// 20us (max) - write data ���۽ÿ� L->C �� C->M �Ѿ ���� �ҿ� �ð� (MSB �ڿ��� tTRAN �����ϰ� �ٷ� tPROGO)
	#define NAND_T_DBSY				500			// 500ns (max) - big page ������ ���� plane���� �Ѿ ���� �ҿ� �ð� (read �� write)

#endif



////////////////
// Dimension
////////////////

// �ּ�ü��.xls ����
//
// 1 sector = 512 bytes, SATA read/write�� �ּ� ����
// SATA read/write ��ɿ��� ���� ��ȣ�� ���� ������ �־����µ�, ���� ��ȣ�� LBA ��� �θ���.
//
// 1 slice = 4KB = 8 sectors, address mapping�� �⺻ ����
//
// physical NAND page�� small page ��� �θ���� �Ѵ�. (NAND program�� read�� �ּ� ����)
//
// physical NAND block�� small block �̶�� �θ���� �Ѵ�. (NAND erase�� �ּ� ����)
// small block ������ small page�� numbering�� �� �ǹ̰� ����.
// �� small page�� ä��� ���� ���� small block ������ ������ ���� small page�� �Ѿ�� ���� �ƴϴ�.
// �� small page�� ä��� ���� ���� big page ������ ������ ���� small page�� �Ѿ��. (multiplane program)
//
// multiplane ���ۿ� ���� ���̴� small page ���� ������ big page ��� �θ���� �Ѵ�.
// multiplane ���ۿ� ���� ���̴� small block ���� ������ big block �̶�� �θ���� �Ѵ�.
// �� ������Ʈ������ read,program,erase�� �־ �⺻������ multiplane ����� ����ϹǷ�, big block�� big page�� small ���� �߿��� �����̴�.
// �־��� NAND�� 4 plane �� ���,
// big block 0 = small block 0, 1, 2, 3
// big block 1 = small block 4, 5, 6, 7
// big block n = small block 4n thru 4n+3
// ���⼭, small block 4n+m�� plane m�� ���Ѵ�.
// big block�� k��° big page��
// small block 0�� small page k ��
// small block 1�� small page k ��
// small block 2�� small page k ��
// small block 3�� small page k
// �� �����ȴ�.
//
// 3D NAND������ program ������ ������ NAND �����簡 one shot ����� �����ߴ�.
// MLC���� one shot ����� �����ϸ� �ش� wordline�� ���ϴ� LSB page�� MSB page�� ���ÿ� program �ȴ�.
// TLC���� one shot ����� �����ϸ� �ش� wordline�� ���ϴ� LSB page, CSB page, MSB page�� ���ÿ� program �ȴ�.
// one shot program�� ����ϸ� �ϳ��� wordline�� �� �� program �ϴ� ���� ����.
// MLC�� ��������, 2���� small page (LSB, MSB) �� ��� small wordline�̶�� �θ���,
// 2���� big page (LSB, MSB) �� ��� big wordline�̶�� �θ���� �Ѵ�.


#define BYTES_PER_SECTOR			512

#define SECTORS_PER_SLICE			8		// 4KB - FTL address mapping�� �⺻ ����
#define BYTES_PER_SLICE				(BYTES_PER_SECTOR * SECTORS_PER_SLICE)
#define BYTES_PER_SLICE_EX			(BYTES_PER_SLICE + SPARE_BYTES_PER_SMALL_PAGE / SLICES_PER_SMALL_PAGE)

#define SECTORS_PER_SMALL_PAGE		(SECTORS_PER_SLICE * SLICES_PER_SMALL_PAGE)
#define BYTES_PER_SMALL_PAGE		(BYTES_PER_SECTOR * SECTORS_PER_SMALL_PAGE)
#define BYTES_PER_SMALL_PAGE_EX		(BYTES_PER_SMALL_PAGE + SPARE_BYTES_PER_SMALL_PAGE)

#define SMALL_PAGES_PER_BIG_PAGE	PLANES_PER_DIE
#define SLICES_PER_BIG_PAGE			(SLICES_PER_SMALL_PAGE * SMALL_PAGES_PER_BIG_PAGE)
#define SECTORS_PER_BIG_PAGE		(SECTORS_PER_SLICE * SLICES_PER_BIG_PAGE)
#define BYTES_PER_BIG_PAGE			(BYTES_PER_SECTOR * SECTORS_PER_BIG_PAGE)
#define BYTES_PER_BIG_PAGE_EX		(BYTES_PER_SMALL_PAGE_EX * SMALL_PAGES_PER_BIG_PAGE)

#define SMALL_PAGES_PER_SMALL_WL	BIT_PER_CELL
#define SLICES_PER_SMALL_WL			(SLICES_PER_SMALL_PAGE * SMALL_PAGES_PER_SMALL_WL)
#define SECTORS_PER_SMALL_WL		(SECTORS_PER_SLICE * SLICES_PER_SMALL_WL)
#define BYTES_PER_SMALL_WL			(BYTES_PER_SECTOR * SECTORS_PER_SMALL_WL)
#define BYTES_PER_SMALL_WL_EX		(BYTES_PER_SMALL_PAGE_EX * SMALL_PAGES_PER_SMALL_WL)

#define BIG_PAGES_PER_BIG_WL		BIT_PER_CELL
#define SMALL_PAGES_PER_BIG_WL		(SMALL_PAGES_PER_BIG_PAGE * BIG_PAGES_PER_BIG_WL)
#define SLICES_PER_BIG_WL			(SLICES_PER_SMALL_PAGE * SMALL_PAGES_PER_BIG_WL)
#define SECTORS_PER_BIG_WL			(SECTORS_PER_SLICE * SLICES_PER_BIG_WL)
#define BYTES_PER_BIG_WL			(BYTES_PER_SECTOR * SECTORS_PER_BIG_WL)
#define BYTES_PER_BIG_WL_EX			(BYTES_PER_BIG_PAGE_EX * BIG_PAGES_PER_BIG_WL)

#define BYTES_PER_SMALL_BLK			(BYTES_PER_SMALL_PAGE * PAGES_PER_BLK)

#define WLS_PER_BLK					(PAGES_PER_BLK / BIT_PER_CELL)	// block�� wordline ����
#define SMALL_BLKS_PER_BIG_BLK		PLANES_PER_DIE
#define SLICES_PER_BIG_BLK			(SLICES_PER_BIG_PAGE * PAGES_PER_BLK)

#define BIG_BLKS_PER_DIE			SMALL_BLKS_PER_PLANE
#define SMALL_BLKS_PER_DIE			(SMALL_BLKS_PER_BIG_BLK * BIG_BLKS_PER_DIE)
#define BIG_PAGES_PER_DIE			(PAGES_PER_BLK * BIG_BLKS_PER_DIE)
#define SMALL_PAGES_PER_DIE			(PAGES_PER_BLK * SMALL_BLKS_PER_DIE)
#define SLICES_PER_DIE				(SLICES_PER_SMALL_PAGE * SMALL_PAGES_PER_DIE)

#define PAGES_PER_WL				BIT_PER_CELL
#define DIES_PER_CHANNEL			(DIES_PER_CE * CES_PER_CHANNEL)
#define NUM_DIES					(DIES_PER_CHANNEL * NUM_CHANNELS)
#define NUM_PSLICES					(SLICES_PER_DIE * NUM_DIES)


#if NAND_CONFIG == CFG_P99_240GB || NAND_CONFIG == CFG_X49_240GB
#define NUM_LSECTORS 	468862000
#endif

#if NAND_CONFIG == CFG_P99_480GB || NAND_CONFIG == CFG_X49_480GB
#define NUM_LSECTORS 	937703000
#endif

#if NAND_CONFIG == CFG_P99_960GB || NAND_CONFIG == CFG_X49_960GB
#define NUM_LSECTORS 	1875385000
#endif

#if NAND_CONFIG == CFG_P99_256GB || NAND_CONFIG == CFG_X49_256GB
#define NUM_LSECTORS 	500118000
#endif

#if NAND_CONFIG == CFG_P99_512GB || NAND_CONFIG == CFG_X49_512GB
#define NUM_LSECTORS 	1000215000
#endif

#if NAND_CONFIG == CFG_P99_1TB || NAND_CONFIG == CFG_X49_1TB
#define NUM_LSECTORS 	2000409000
#endif

#ifndef NUM_LSECTORS
#error
#endif

#if OPTION_DRAM_SSD
#undef NUM_LSECTORS
#define NUM_LSECTORS 	(DRAM_SIZE - 1048576*28)
#endif

#ifndef NUM_LSECTORS
#error
#endif

#define NUM_LSLICES		(NUM_LSECTORS / SECTORS_PER_SLICE)



//#define SIM_NAND_BYTE_PER_SEC			533000000ULL							// 533MB/s per channel (533MHz * 8bit)
#define SIM_NAND_BYTE_PER_SEC			400000000ULL							// 400MB/s per channel (400MHz * 8bit)
#define SIM_NAND_NANOSEC_PER_BYTE		(1000000000.0 / SIM_NAND_BYTE_PER_SEC)	// 2.5 nsec

#define STATUS_CHECK_TIME				500										// status check ������ �ѹ� �� ������ �ҿ�Ǵ� �ð� = 500ns
#define STATUS_CHECK_INTERVAL			5000									// status check ������ �ֱ� = 5 usec


/////////////////
// addressing
/////////////////

// �ּ�ü��.xlsx �� ���� ����

typedef union
{
	struct
	{
		UINT32	slice_offset	: LOG2(SLICES_PER_BIG_WL);		// big wordline �������� slice ��ȣ
		UINT32	wl_index		: LOG2(WLS_PER_BLK);			// big block �������� big wordline ��ȣ
		UINT32	big_blk_index	: LOG2(BIG_BLKS_PER_DIE);		// die	�������� big block ��ȣ
		UINT32	die 			: 32 - LOG2(SLICES_PER_BIG_WL) - LOG2(WLS_PER_BLK) - LOG2(BIG_BLKS_PER_DIE);
	};

	UINT32	w32;

} psa_t;

INLINE UINT32 psa_encode(UINT32 die, UINT32 big_blk_index, UINT32 wl_index, UINT32 slice_offset)
{
	// multiplane ������ ���� �Ķ���͵��� �����Ͽ� PSA �� ���

	psa_t psa;
	psa.die = die;
	psa.big_blk_index = big_blk_index;
	psa.wl_index = wl_index;
	psa.slice_offset = slice_offset;
	return psa.w32;
}

INLINE void psa_decode(UINT32 psa, UINT32* die, UINT32* big_blk_index, UINT32* wl_index, UINT32* slice_offset)
{
	// PSA �����κ��� multiplane ������ ���� �Ķ���͵��� ���

	psa_t temp;
	temp.w32 = psa;
	*die = temp.die;
	*big_blk_index = temp.big_blk_index;
	*wl_index = temp.wl_index;
	*slice_offset= temp.slice_offset;
}

INLINE UINT32 psa_encode_2(UINT32 die, UINT32 small_blk_index, UINT32 wl_index, UINT32 lcm, UINT32 slice)
{
	// single plane ������ ���� �Ķ���͵��� �����Ͽ� PSA �� ���
	// �Է¿� ���� ����:
	// die: die ��ȣ
	// small_blk_index: die �������� small block ��ȣ
	// wl_index: small block �������� small wordline ��ȣ
	// lcm: small wordline �������� small page ��ȣ (0 = LSB page, 1 = CSB page, 2 = MSB page)
	// slice: small page �������� slice ��ȣ

	psa_t psa;
	psa.die = die;
	psa.big_blk_index = small_blk_index / SMALL_BLKS_PER_BIG_BLK;
	psa.wl_index = wl_index;
	psa.slice_offset = lcm * SLICES_PER_BIG_PAGE + (small_blk_index % SMALL_BLKS_PER_BIG_BLK) * SLICES_PER_SMALL_PAGE + slice;
	return psa.w32;
}

// psa_decode_2() �Լ��� �������� ����
// big_blk_index�� �ԷµǸ� PLANES_PER_DIE ��ŭ�� small_blk_index�� ����ؾ� �ϹǷ� �����ϴ�.


INLINE UINT32 lcm_decode(UINT32 slice)
{
	// �־��� ���� slice�� PSA �Ǵ� big wordline �������� slice offset

	UINT32 temp = slice / SLICES_PER_BIG_PAGE;

	#if BIT_PER_CELL == TLC
	return temp & 3;
	#endif

	#if BIT_PER_CELL == MLC
	return temp & 1;		// MLC������ L, C, M�� �ƴϰ� LSB�� MSB�� �� �� �ϳ�
	#endif
}

// TLC�� ���, PSA ������ slice_offset (big WL �������� slice ��ȣ) �� �����ϱ� ���� psa % SLICES_PER_BIG_WL �ϸ� Ʋ�� ���� ��´�.

#define SLICE_OFFSET_MASK	((1 << LOG2(SLICES_PER_BIG_WL)) - 1)

#endif	// NAND_H

