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


// Definition of basic types and elementary macros that are common across the entire project.
// Do not #include other headers from here.
// This header must be completely independent and self-contained.

#ifndef COMMON_H
#define COMMON_H

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;

// Signed integer should be used only when necessary.

typedef char SINT8;
typedef short SINT16;
typedef int SINT32;
typedef long long SINT64;

typedef unsigned char BOOL8;
typedef unsigned short BOOL16;
typedef unsigned int BOOL32;

#define FF8    ((unsigned char) 0xFF)
#define FF16   ((unsigned short) 0xFFFF)
#define FF32   0xFFFFFFFFUL
#define FF64   0xFFFFFFFFFFFFFFFFULL

#define TRUE	1
#define FALSE	0

#define OK		1
#define FAIL	0

#define BIT(X)	(((UINT32) 1) << (X))

#define BIT0	BIT(0)
#define BIT1	BIT(1)
#define BIT2	BIT(2)
#define BIT3	BIT(3)
#define BIT4	BIT(4)
#define BIT5	BIT(5)
#define BIT6	BIT(6)
#define BIT7	BIT(7)
#define BIT8	BIT(8)
#define BIT9	BIT(9)
#define BIT10	BIT(10)
#define BIT11	BIT(11)
#define BIT12	BIT(12)
#define BIT13	BIT(13)
#define BIT14	BIT(14)
#define BIT15	BIT(15)
#define BIT16	BIT(16)
#define BIT17	BIT(17)
#define BIT18	BIT(18)
#define BIT19	BIT(19)
#define BIT20	BIT(20)
#define BIT21	BIT(21)
#define BIT22	BIT(22)
#define BIT23	BIT(23)
#define BIT24	BIT(24)
#define BIT25	BIT(25)
#define BIT26	BIT(26)
#define BIT27	BIT(27)
#define BIT28	BIT(28)
#define BIT29	BIT(29)
#define BIT30	BIT(30)
#define BIT31	BIT(31)


// LOG2(1024) = 10. LOG2(1025) = 11.
// LOG2 매크로의 인자는 #define 상수이어야 한다.
// X가 변수이면 컴파일 결과가 매우 비효율적이므로,
// ARM의 __clz intrinsic을 사용할 것을 권장한다.

#define LOG2(X)	(((X) > 0x1) + \
				((X) > 0x2) + \
				((X) > 0x4) + \
				((X) > 0x8) + \
				((X) > 0x10) + \
				((X) > 0x20) + \
				((X) > 0x40) + \
				((X) > 0x80) + \
				((X) > 0x100) + \
				((X) > 0x200) + \
				((X) > 0x400) + \
				((X) > 0x800) + \
				((X) > 0x1000) + \
				((X) > 0x2000) + \
				((X) > 0x4000) + \
				((X) > 0x8000) + \
				((X) > 0x10000) + \
				((X) > 0x20000) + \
				((X) > 0x40000) + \
				((X) > 0x80000) + \
				((X) > 0x100000) + \
				((X) > 0x200000) + \
				((X) > 0x400000) + \
				((X) > 0x800000) + \
				((X) > 0x1000000) + \
				((X) > 0x2000000) + \
				((X) > 0x4000000) + \
				((X) > 0x8000000) + \
				((X) > 0x10000000) + \
				((X) > 0x20000000) + \
				((X) > 0x40000000) + \
				((X) > 0x80000000))

#define POT(X)				(1 << LOG2(X))				// power of two로 올림. POT(512) = 512. POT(513) = 1024.
#define ROUND_UP(X, Y)		((X + Y - 1) / (Y) * (Y))	// X를 Y의 배수로 올림. ROUND_UP(1024, 512) = 1024. ROUND_UP(1025, 512) = 1536.
#define ROUND_DOWN(X, Y)		((X) / (Y) * (Y))		// X를 Y의 배수로 내림. ROUND_DOWN(1023, 512) = 512. ROUND_DOWN(1024, 512) = 1024.
#define IS_POWER_OF_TWO(X)	(((X) != 0) && (((X) & ((X)-1)) == 0))
#define DIV_CEIL(A, B)		(((A) + (B) - 1) / (B))		// 나눗셈 결과를 정수로 올림. DIV_CEIL(15, 8) = 2. DIV_CEIL(16, 8) = 2
#define MIN(A, B)			(((A) < (B)) ? (A) : (B))
#define MAX(A, B)			(((A) > (B)) ? (A) : (B))

#define V8(X)		(*((volatile UINT8*) &(X)))
#define V16(X)		(*((volatile UINT16*) &(X)))
#define V32(X)		(*((volatile UINT32*) &(X)))
#define V64(X)		(*((volatile UINT64*) &(X)))

#define SETREG(ADDR, VAL)	((*(volatile UINT32*)(ADDR)) = (VAL))
#define GETREG(ADDR)		(*(volatile UINT32*)(ADDR))

#define INLINE	static __inline

#define STOSD(ADDR, VAL_32, NUM_BYTES)		__stosd((PDWORD) (ADDR), (VAL_32), (NUM_BYTES) / 4)
#define STOSQ(ADDR, VAL_64, NUM_BYTES)		__stosq((PDWORD64) (ADDR), (VAL_64), (NUM_BYTES) / 8)

#define PRINT_SIZE(X)	printf("%-32s%10zu\n", #X, sizeof(X))
#define PRINT_NUM(X)	printf("%-32s%10u\n", #X, (X))

#define count_ones(...)		__popcnt(__VA_ARGS__)

INLINE UINT32 find_one(UINT32 w32)
{
	// least significant bit부터 시작하여 왼쪽으로 가다가 최초의 1을 만나면 그 위치(0..31)를 리턴한다.

	UINT32 bit_offset;

	if (_BitScanForward((LPDWORD) &bit_offset, w32) == 0)
	{
		return 32;	// w32 == 0 인 경우
	}
	else
	{
		return bit_offset;
	}
}

#endif	// COMMON_H

