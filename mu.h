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


#ifndef MU_H
#define MU_H

// Memory Utility functions

enum	// _mu_search() 함수에 주어지는 명령 코드
{
	MU_EQU_8, MU_EQU_16, MU_EQU_32,
	MU_NEQ_8, MU_NEQ_16, MU_NEQ_32,
	MU_MIN_8, MU_MIN_16, MU_MIN_32,
	MU_MAX_8, MU_MAX_16, MU_MAX_32,
};

#define MF_8		0x01	// item의 크기 = 1 byte
#define MF_16		0x02	// item의 크기 = 2 byte
#define MF_32		0x04	// item의 크기 = 4 byte

#define MF_EQU		0x10	// value와 같은 값 찾기
#define MF_NEQ		0x20	// value와 다른 최초의 item 찾기
#define MF_MIN		0x40	// 최소값 찾기
#define MF_MAX		0x80	// 최대값 찾기

#define mu_search(ARRAY, ...)	_mu_search((UINT8*) (ARRAY), __VA_ARGS__)
UINT32 _mu_search(UINT8* array, UINT32 cmd, UINT32 num_items, UINT32 value, UINT32* found_value);	// 리턴값은 item의 index


UINT32 mu_bmp_search(UINT32* bmp, UINT32 num_bits);			// 주어진 bitmap에서 최초의 1을 찾음


#define mu_test_bit(BITMAP, BIT_INDEX)		(*(((UINT8*)(BITMAP)) + (BIT_INDEX) / 8) & (1 << ((BIT_INDEX) % 8)))	// 해당 bit의 값이 1이면 non-zero 값을 리턴 (TRUE 값을 리턴하는 것이 아님)


INLINE void mu_set_bit(UINT8* bmp, UINT32 bit_index)		// 해당 bit를 1로 변경
{
	ASSERT(mu_test_bit(bmp, bit_index) == 0);

	*((UINT8*)bmp + bit_index / 8) |= 1 << (bit_index % 8);
}


INLINE void mu_clear_bit(UINT8* bmp, UINT32 bit_index)		// 해당 bit를 0으로 변경
{
	ASSERT(mu_test_bit(bmp, bit_index) != 0);

	*((UINT8*)bmp + bit_index / 8) &= ~(1 << (bit_index % 8));
}


#define mu_fill		STOSD
#define mu_copy(TO, FROM, NUM_BYTES)		memcpy((void*) (TO), (void*) (FROM), (NUM_BYTES))

#endif	// MU_H

