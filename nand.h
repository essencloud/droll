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


//#define NAND_CONFIG			CFG_P99_240GB	// 시뮬레이션에 사용할 NAND 구성 - 아래 12 개 중에서 선택 또는 필요에 따라 추가
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
#define NAND_PART_NUMBER		HYCRON_P99_TLC		// 제품에 사용된 NAND의 파트 번호
#define NUM_CHANNELS			4					// 채널의 개수, 각 채널은 8bit
#define CES_PER_CHANNEL			1					// 채널당 CE signal의 개수
#define DIES_PER_CE				1					// CE signal당 die(LUN)의 개수
#define DRAM_SIZE				(256 * 1048576)		// 제품에 부착된 DRAM의 크기 (byte)
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

#define SAMSHIBA_X49_TLC	1	// 삼시바 49단 3D TLC 256Gb NAND
#define HYCRON_P99_TLC		2	// 하이크론 99단 3D TLC 512Gb NAND

#define SLC		1
#define MLC		2
#define	TLC		3

#define LSB		0
#define CSB		1
#define MSB		2

#if NAND_PART_NUMBER == SAMSHIBA_X49_TLC

	// 실제로 존재하는 특정 회사의 제품과는 전혀 상관 없으므로 참고할 가치도 없음

	#define BIT_PER_CELL				TLC
	#define SLICES_PER_SMALL_PAGE		4
	#define PAGES_PER_BLK				768
	#define SMALL_BLKS_PER_PLANE		1500
	#define PLANES_PER_DIE				2
	#define SPARE_BYTES_PER_SMALL_PAGE	128

	#define NAND_T_R				80000		// 80us
	#define NAND_T_PROGO			2000000		// 2ms
	#define NAND_T_BERS				4000000		// 4ms
	#define NAND_T_TRAN				20000		// 20us (max) - write data 전송시에 L->C 및 C->M 넘어갈 때의 소요 시간 (MSB 뒤에는 tTRAN 생략하고 바로 tPROGO)
	#define NAND_T_DBSY				500			// 500ns (max) - big page 내에서 다음 plane으로 넘어갈 때의 소요 시간 (read 및 write)

#endif

#if NAND_PART_NUMBER == HYCRON_P99_TLC

	// 실제로 존재하는 특정 회사의 제품과는 전혀 상관 없으므로 참고할 가치도 없음

	#define BIT_PER_CELL				TLC
	#define SLICES_PER_SMALL_PAGE		2
	#define PAGES_PER_BLK				2100
	#define SMALL_BLKS_PER_PLANE		1100
	#define PLANES_PER_DIE				4
	#define SPARE_BYTES_PER_SMALL_PAGE	256

	#define NAND_T_R				70000		// 70us
	#define NAND_T_PROGO			2000000		// 2ms
	#define NAND_T_BERS				4000000		// 4ms
	#define NAND_T_TRAN				20000		// 20us (max) - write data 전송시에 L->C 및 C->M 넘어갈 때의 소요 시간 (MSB 뒤에는 tTRAN 생략하고 바로 tPROGO)
	#define NAND_T_DBSY				500			// 500ns (max) - big page 내에서 다음 plane으로 넘어갈 때의 소요 시간 (read 및 write)

#endif



////////////////
// Dimension
////////////////

// 주소체계.xls 참고
//
// 1 sector = 512 bytes, SATA read/write의 최소 단위
// SATA read/write 명령에는 섹터 번호와 섹터 개수가 주어지는데, 섹터 번호를 LBA 라고 부른다.
//
// 1 slice = 4KB = 8 sectors, address mapping의 기본 단위
//
// physical NAND page를 small page 라고 부르기로 한다. (NAND program과 read의 최소 단위)
//
// physical NAND block을 small block 이라고 부르기로 한다. (NAND erase의 최소 단위)
// small block 내에서 small page의 numbering은 별 의미가 없다.
// 한 small page를 채우고 나면 같은 small block 내에서 인접한 다음 small page로 넘어가는 것이 아니다.
// 한 small page를 채우고 나면 같은 big page 내에서 인접한 다음 small page로 넘어간다. (multiplane program)
//
// multiplane 동작에 의해 묶이는 small page 들의 집합을 big page 라고 부르기로 한다.
// multiplane 동작에 의해 묶이는 small block 들의 집합을 big block 이라고 부르기로 한다.
// 본 프로젝트에서는 read,program,erase에 있어서 기본적으로 multiplane 방식을 사용하므로, big block과 big page가 small 보다 중요한 개념이다.
// 주어진 NAND가 4 plane 일 경우,
// big block 0 = small block 0, 1, 2, 3
// big block 1 = small block 4, 5, 6, 7
// big block n = small block 4n thru 4n+3
// 여기서, small block 4n+m은 plane m에 속한다.
// big block의 k번째 big page는
// small block 0의 small page k 및
// small block 1의 small page k 및
// small block 2의 small page k 및
// small block 3의 small page k
// 로 구성된다.
//
// 3D NAND에서의 program 동작은 상당수의 NAND 제조사가 one shot 방식을 도입했다.
// MLC에서 one shot 방식을 적용하면 해당 wordline에 속하는 LSB page와 MSB page가 동시에 program 된다.
// TLC에서 one shot 방식을 적용하면 해당 wordline에 속하는 LSB page, CSB page, MSB page가 동시에 program 된다.
// one shot program을 사용하면 하나의 wordline을 두 번 program 하는 일이 없다.
// MLC를 기준으로, 2개의 small page (LSB, MSB) 를 묶어서 small wordline이라고 부르고,
// 2개의 big page (LSB, MSB) 를 묶어서 big wordline이라고 부르기로 한다.


#define BYTES_PER_SECTOR			512

#define SECTORS_PER_SLICE			8		// 4KB - FTL address mapping의 기본 단위
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

#define WLS_PER_BLK					(PAGES_PER_BLK / BIT_PER_CELL)	// block당 wordline 개수
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

#define STATUS_CHECK_TIME				500										// status check 동작을 한번 할 때마다 소요되는 시간 = 500ns
#define STATUS_CHECK_INTERVAL			5000									// status check 동작의 주기 = 5 usec


/////////////////
// addressing
/////////////////

// 주소체계.xlsx 의 설명 참고

typedef union
{
	struct
	{
		UINT32	slice_offset	: LOG2(SLICES_PER_BIG_WL);		// big wordline 내에서의 slice 번호
		UINT32	wl_index		: LOG2(WLS_PER_BLK);			// big block 내에서의 big wordline 번호
		UINT32	big_blk_index	: LOG2(BIG_BLKS_PER_DIE);		// die	내에서의 big block 번호
		UINT32	die 			: 32 - LOG2(SLICES_PER_BIG_WL) - LOG2(WLS_PER_BLK) - LOG2(BIG_BLKS_PER_DIE);
	};

	UINT32	w32;

} psa_t;

INLINE UINT32 psa_encode(UINT32 die, UINT32 big_blk_index, UINT32 wl_index, UINT32 slice_offset)
{
	// multiplane 동작을 위한 파라메터들을 조합하여 PSA 값 계산

	psa_t psa;
	psa.die = die;
	psa.big_blk_index = big_blk_index;
	psa.wl_index = wl_index;
	psa.slice_offset = slice_offset;
	return psa.w32;
}

INLINE void psa_decode(UINT32 psa, UINT32* die, UINT32* big_blk_index, UINT32* wl_index, UINT32* slice_offset)
{
	// PSA 값으로부터 multiplane 동작을 위한 파라메터들을 계산

	psa_t temp;
	temp.w32 = psa;
	*die = temp.die;
	*big_blk_index = temp.big_blk_index;
	*wl_index = temp.wl_index;
	*slice_offset= temp.slice_offset;
}

INLINE UINT32 psa_encode_2(UINT32 die, UINT32 small_blk_index, UINT32 wl_index, UINT32 lcm, UINT32 slice)
{
	// single plane 동작을 위한 파라메터들을 조합하여 PSA 값 계산
	// 입력에 대한 설명:
	// die: die 번호
	// small_blk_index: die 내에서의 small block 번호
	// wl_index: small block 내에서의 small wordline 번호
	// lcm: small wordline 내에서의 small page 번호 (0 = LSB page, 1 = CSB page, 2 = MSB page)
	// slice: small page 내에서의 slice 번호

	psa_t psa;
	psa.die = die;
	psa.big_blk_index = small_blk_index / SMALL_BLKS_PER_BIG_BLK;
	psa.wl_index = wl_index;
	psa.slice_offset = lcm * SLICES_PER_BIG_PAGE + (small_blk_index % SMALL_BLKS_PER_BIG_BLK) * SLICES_PER_SMALL_PAGE + slice;
	return psa.w32;
}

// psa_decode_2() 함수는 존재하지 않음
// big_blk_index가 입력되면 PLANES_PER_DIE 만큼의 small_blk_index를 출력해야 하므로 번잡하다.


INLINE UINT32 lcm_decode(UINT32 slice)
{
	// 주어진 인자 slice는 PSA 또는 big wordline 내에서의 slice offset

	UINT32 temp = slice / SLICES_PER_BIG_PAGE;

	#if BIT_PER_CELL == TLC
	return temp & 3;
	#endif

	#if BIT_PER_CELL == MLC
	return temp & 1;		// MLC에서는 L, C, M이 아니고 LSB와 MSB의 둘 중 하나
	#endif
}

// TLC의 경우, PSA 값에서 slice_offset (big WL 내에서의 slice 번호) 을 추출하기 위해 psa % SLICES_PER_BIG_WL 하면 틀린 값을 얻는다.

#define SLICE_OFFSET_MASK	((1 << LOG2(SLICES_PER_BIG_WL)) - 1)

#endif	// NAND_H

