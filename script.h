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


// Simulation script의 작성에 사용되는 기본 요소

enum	// Script Instructions - 순서가 중요하므로 바꾸면 안됨 - run() 코드 참고
{
	READ		= REQ_HOST_READ,
	WRITE		= REQ_HOST_WRITE,
	TRIM		= REQ_TRIM,
	FAST_FLUSH	= REQ_FAST_FLUSH,		// REQ_FAST_FLUSH를 보내고 기다림
	SLOW_FLUSH	= REQ_SLOW_FLUSH,		// REQ_SLOW_FLUSH를 보내고 기다림
	FINISH_ALL, 	// 추가적인 명령을 보내지 않고 num_pending_cmds == 0 될때까지 기다림
	NOP,			// 추가적인 명령을 보내지 않고 지정된 시간동안 기다림
	RANDOM_CMD,		// 여기까지는 랜덤하게 생성될 수 있는 명령들

	BEGIN_LOOP,
	END_LOOP,

	ALIGN,				// aligned read와 write의 확률을 각각 지정 (1024 = 100%)
	SET_LBA,			// for SEQ_LBA and SAME_LBA
	SET_LBA_RANGE,		// RANDOM_LBA에 의해서 생성되는 LBA의 범위를 지정. 기존의 hot spot은 disable 되므로 다시 enable 해야 함.
	SET_NOP_PERIOD,		// NOP 구간의 최소, 최대 길이 (sim-seconds)
	SET_CMD_P,			// RANDOM_CMD 에 의해 생성되는 명령의 확률
	SET_MAX_QD,			// queue depth 제한

	ENABLE_HOT_SPOT,	// RANDOM_LBA에 의해서 자주 생성되는 LBA 영역을 생성
	DISABLE_HOT_SPOT,

	STOP,
	PRINT_STAT,
	BEGIN_INSIGHT,		// OPTION_INSIGHT가 켜져 있어야 정보가 출력됨
	END_INSIGHT,
	END_OF_SCRIPT,
} inst_t;


enum
{
	CYCLES, SECONDS, MINUTES, HOURS, SECTORS, MEGABYTES, GIGABYTES, FOREVER,
};

#define RANDOM_LBA		0xFFFFFFFDUL
#define SEQ_LBA			0xFFFFFFFEUL
#define SAME_LBA		0xFFFFFFFFUL

#define RANDOM_SIZE		0xFFFFFFFEUL
#define SAME_SIZE		0xFFFFFFFFUL


////////////////
// 스크립트
////////////////

static UINT32 scr_prepare_1[] =
{
	SET_CMD_P,	READ,      6930,			// RANDOM_CMD 에 의해 생성되는 명령의 확률
				WRITE,     9074,			// 합은 2의 거듭제곱
				TRIM,       300,
				FAST_FLUSH,  20,
				SLOW_FLUSH,  20,
				FINISH_ALL,  20,
				NOP,   	     20,

//	ALIGN, 1024, 1024,			// always aligned
	ALIGN, 1023, 1021,			// aligned read 확률 1023/1024, aligned write 확률 1021/1024
	SET_NOP_PERIOD, 0, 2,
	ENABLE_HOT_SPOT,

	END_OF_SCRIPT,
};

static UINT32 scr_prepare_2[] =
{
	SET_CMD_P,	READ,      4096,
				WRITE,     4096,
				TRIM,         0,
				FAST_FLUSH,   0,
				SLOW_FLUSH,   0,
				FINISH_ALL,   0,
				NOP,   	      0,

	ALIGN, 1024, 1024,			// always aligned
//	ALIGN, 1023, 1021,			// aligned read 확률 1023/1024, aligned write 확률 1021/1024
	ENABLE_HOT_SPOT,

	END_OF_SCRIPT,
};

static UINT32 scr_random[] =
{
	BEGIN_LOOP, FOREVER, 0,
//	BEGIN_LOOP, HOURS, 4,
//	BEGIN_LOOP, MINUTES, 30,
	RANDOM_CMD,
	END_LOOP,

	FAST_FLUSH,
	PRINT_STAT,
	END_OF_SCRIPT,
};

static UINT32 scr_write_all[] =	// 드라이브 전체를 sequential write
{
	SET_LBA, 0,

	BEGIN_LOOP, SECTORS, NUM_LSECTORS,
	WRITE, SEQ_LBA, 256,
	END_LOOP,

	FAST_FLUSH,
	PRINT_STAT,
	END_OF_SCRIPT
};

static UINT32 scr_read_all[] =	// 드라이브 전체를 sequential read
{
	SET_LBA, 0,

	BEGIN_LOOP, SECTORS, NUM_LSECTORS,
	READ, SEQ_LBA, 256,
	END_LOOP,

	FAST_FLUSH,
	PRINT_STAT,
	END_OF_SCRIPT
};

static UINT32 scr_sw_16gb[] =		// sequential write 16GB
{
	SET_LBA, 0,						// LBA 0에서 시작

	BEGIN_LOOP, GIGABYTES, 16,
	WRITE, SEQ_LBA, 256,
	END_LOOP,

	FAST_FLUSH,
	PRINT_STAT,
	END_OF_SCRIPT
};


static UINT32 scr_sr_16gb[] =		// sequential read 16GB
{
	SET_LBA, 0,						// LBA 0에서 시작

	BEGIN_LOOP, GIGABYTES, 16,
	READ, SEQ_LBA, 256,
	END_LOOP,

	FAST_FLUSH,
	PRINT_STAT,
	END_OF_SCRIPT
};

static UINT32 scr_rw_32gb[] =		// random write 32GB
{
	ALIGN, 1024, 1024,
	SET_LBA_RANGE, 0, 16 * (1024*1024*1024/BYTES_PER_SECTOR) - 1,		// RANDOM_LBA 영역을 16GB로 제한

	BEGIN_INSIGHT, 128,				// OPTION_INSIGHT에 의한 덤프를 시작함. 생성되는 덤프 파일의 크기는 128MB로 제한.

	BEGIN_LOOP, GIGABYTES, 32,
	WRITE, RANDOM_LBA, 8,
	END_LOOP,

	FAST_FLUSH,

	END_INSIGHT,					// 덤프 종료

	PRINT_STAT,
	END_OF_SCRIPT
};

static UINT32 scr_rr_16gb[] =		// random read 16GB (16GB 영역 내에서)
{
	ALIGN, 1024, 1024,
	SET_LBA_RANGE, 0, 16 * (1024*1024*1024/BYTES_PER_SECTOR) - 1,		// RANDOM_LBA 영역을 16GB로 제한

	BEGIN_LOOP, GIGABYTES, 16,
	READ, RANDOM_LBA, 8,
	END_LOOP,

	FAST_FLUSH,
	PRINT_STAT,
	END_OF_SCRIPT
};

static UINT32 scr_write_same_slice[] =
{
	SET_LBA, RANDOM_LBA,

	BEGIN_LOOP, CYCLES, 10000,
	WRITE, SAME_LBA, SECTORS_PER_SLICE,
	END_LOOP,

	READ, SAME_LBA, SECTORS_PER_SLICE,

	END_OF_SCRIPT,
};

static UINT32 scr_rwv[] =
{
//	BEGIN_LOOP, CYCLES, 10000,
	BEGIN_LOOP, FOREVER, 0,
	WRITE, RANDOM_LBA, RANDOM_SIZE,
	READ, SAME_LBA, SAME_SIZE,
	END_LOOP,

	END_OF_SCRIPT,
};

static UINT32 scr_standard_test[] =
{
	// 드라이브 전체 sequential write 128KB

	SET_LBA, 0,
	BEGIN_LOOP, SECTORS, NUM_LSECTORS,
	WRITE, SEQ_LBA, 256,
	END_LOOP,
	FAST_FLUSH,
	PRINT_STAT,

	// 드라이브 전체 sequential read 128KB

	SET_LBA, 0,
	BEGIN_LOOP, SECTORS, NUM_LSECTORS,
	READ, SEQ_LBA, 256,
	END_LOOP,
	FAST_FLUSH,
	PRINT_STAT,

	// random write 4KB, 드라이브 전체 용량의 32분의 1 (GC 발생하지 않을 만큼만)

	ALIGN, 1024, 1024,			// always aligned
	DISABLE_HOT_SPOT,

	BEGIN_LOOP, SECTORS, NUM_LSECTORS / 32,
	WRITE, RANDOM_LBA, 8,
	END_LOOP,
	FAST_FLUSH,
	PRINT_STAT,

	// random read 4KB (총 16GB)

	BEGIN_LOOP, GIGABYTES, 16,
	READ, RANDOM_LBA, 8,
	END_LOOP,
	FAST_FLUSH,
	PRINT_STAT,

	// random commands

	SET_CMD_P,	READ,      6930,			// RANDOM_CMD 에 의해 생성되는 명령의 확률
				WRITE,     9094,			// 합은 2의 거듭제곱
				TRIM,       300,
				FAST_FLUSH,  20,
				SLOW_FLUSH,  20,
				FINISH_ALL,  20,
				NOP,   	      0,

	ENABLE_HOT_SPOT,
	SET_NOP_PERIOD, 0, 0,		// NOP 구간의 최소, 최대 길이 (sim-seconds)
//	ALIGN, 1024, 1024,			// always aligned
	ALIGN, 1023, 1021,			// aligned read 확률 1023/1024, aligned write 확률 1021/1024

	BEGIN_LOOP, HOURS, 8,
	RANDOM_CMD,
	END_LOOP,

	FAST_FLUSH,

	PRINT_STAT,

	// 드라이브 전체 sequential read 128KB

	SET_LBA, 0,
	BEGIN_LOOP, SECTORS, NUM_LSECTORS,
	READ, SEQ_LBA, 256,
	END_LOOP,

	FAST_FLUSH,

	PRINT_STAT,

	END_OF_SCRIPT,
};

