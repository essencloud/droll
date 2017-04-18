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


#include "droll.h"

// Memory Utility functions


UINT32 _mu_search(UINT8* array, UINT32 cmd, UINT32 num_items, UINT32 value, UINT32* found_value)
{
	UINT32 index = num_items;

	ASSERT(num_items != 0);

	switch (cmd)
	{
		// num_items 값이 일반적으로 매우 크므로, for loop 내부에서 conditional branch의 개수를 최소화하는 것이 중요

		case MU_EQU_8:
		{
			for (UINT32 i = 0; i < num_items; i++)
			{
				if (*(((UINT8*)array) + i) == value)
				{
					index = i;
					break;
				}
			}

			break;
		}
		case MU_EQU_16:
		{
			ASSERT(((UINT32) array) % sizeof(UINT16) == 0);

			for (UINT32 i = 0; i < num_items; i++)
			{
				if (*(((UINT16*)array) + i) == value)
				{
					index = i;
					break;
				}
			}

			break;
		}
		case MU_EQU_32:
		{
			ASSERT(((UINT32) array) % sizeof(UINT32) == 0);

			for (UINT32 i = 0; i < num_items; i++)
			{
				if (*(((UINT32*)array) + i) == value)
				{
					index = i;
					break;
				}
			}

			break;
		}
		case MU_NEQ_8:
		{
			for (UINT32 i = 0; i < num_items; i++)
			{
				if (*(((UINT8*)array) + i) != value)
				{
					index = i;
					break;
				}
			}

			break;
		}
		case MU_NEQ_16:
		{
			ASSERT(((UINT32)array) % sizeof(UINT16) == 0);

			for (UINT32 i = 0; i < num_items; i++)
			{
				if (*(((UINT16*)array) + i) != value)
				{
					index = i;
					break;
				}
			}

			break;
		}
		case MU_NEQ_32:
		{
			ASSERT(((UINT32)array) % sizeof(UINT32) == 0);

			for (UINT32 i = 0; i < num_items; i++)
			{
				if (*(((UINT32*)array) + i) != value)
				{
					index = i;
					break;
				}
			}

			break;
		}
		case MU_MIN_8:
		{
			value = FF8;

			for (UINT32 i = 0; i < num_items; i++)
			{
				UINT32 sample = *(((UINT8*)array) + i);

				if (sample <= value)
				{
					value = sample;
					index = i;

					if (value == 0)
						break;
				}
			}

			break;
		}
		case MU_MIN_16:
		{
			ASSERT(((UINT32)array) % sizeof(UINT16) == 0);

			value = FF16;

			for (UINT32 i = 0; i < num_items; i++)
			{
				UINT32 sample = *(((UINT16*)array) + i);

				if (sample <= value)
				{
					value = sample;
					index = i;

					if (value == 0)
						break;
				}
			}

			break;
		}
		case MU_MIN_32:
		{
			ASSERT(((UINT32)array) % sizeof(UINT32) == 0);

			value = FF32;

			for (UINT32 i = 0; i < num_items; i++)
			{
				UINT32 sample = *(((UINT32*)array) + i);

				if (sample <= value)
				{
					value = sample;
					index = i;

					if (value == 0)
						break;
				}
			}

			break;
		}
		case MU_MAX_8:
		{
			value = 0;

			for (UINT32 i = 0; i < num_items; i++)
			{
				UINT32 sample = *(((UINT8*)array) + i);

				if (sample >= value)
				{
					value = sample;
					index = i;

					if (value == FF8)
						break;
				}
			}

			break;
		}
		case MU_MAX_16:
		{
			ASSERT(((UINT32)array) % sizeof(UINT16) == 0);

			value = 0;

			for (UINT32 i = 0; i < num_items; i++)
			{
				UINT32 sample = *(((UINT16*)array) + i);

				if (sample >= value)
				{
					value = sample;
					index = i;

					if (value == FF16)
						break;
				}
			}

			break;
		}
		case MU_MAX_32:
		{
			ASSERT((((UINT32) array)) % sizeof(UINT32) == 0);

			value = 0;

			for (UINT32 i = 0; i < num_items; i++)
			{
				UINT32 sample = *(((UINT32*)array) + i);

				if (sample >= value)
				{
					value = sample;
					index = i;

					if (value == FF32)
						break;
				}
			}

			break;
		}
		default:
		{
			ASSERT(FAIL);
		}
	}

	*found_value = value;

	return index;
}

UINT32 mu_bmp_search(UINT32* bitmap, UINT32 num_bits)
{
	ASSERT((UINT64)bitmap % sizeof(UINT32) == 0);
	ASSERT(num_bits > 32);			// num_bits == 32이면 find_one() 사용을 권장

	UINT32 bit_index = 0;

	if ((UINT64)bitmap % sizeof(UINT64) != 0)
	{
		UINT32 sample = *bitmap;

		if (sample != 0)
		{
			return bit_index + find_one(sample);
		}

		bitmap++;
		bit_index = 32;
	}

	UINT64* ptr = (UINT64*) bitmap;

	while (1)
	{
		UINT64 sample = *ptr++;

		if (sample != 0)
		{
			UINT32 bit_offset;
			_BitScanForward64((DWORD*) &bit_offset, sample);

			return bit_index + bit_offset;
		}

		bit_index += 64;

		if (bit_index >= num_bits)
		{
			return num_bits;
		}
	}
}

