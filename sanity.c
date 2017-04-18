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

void sanity_check(void)
{
	#if OPTION_DRAM_SSD == FALSE
	SANITY_CHECK(NUM_LSLICES < NUM_PSLICES);
	#endif

	SANITY_CHECK(IS_POWER_OF_TWO(SLICES_PER_BIG_PAGE));
	SANITY_CHECK(SLICES_PER_BIG_PAGE <= 8);	// slice_bmp 를 UINT8로 표현

	SANITY_CHECK(NUM_LSECTORS % SECTORS_PER_SLICE == 0);

	SANITY_CHECK(SPARE_BYTES_PER_SMALL_PAGE % SLICES_PER_SMALL_PAGE == 0);

	SANITY_CHECK(DRAM_SIZE >= sizeof(g_dram));

	SANITY_CHECK(IS_POWER_OF_TWO(NCQ_SIZE) == TRUE);
	SANITY_CHECK(IS_POWER_OF_TWO(SECTORS_PER_SLICE) == TRUE);
	SANITY_CHECK(IS_POWER_OF_TWO(BYTES_PER_SECTOR) == TRUE);

	SANITY_CHECK(NUM_RBUF_SLOTS <= 65536);	// UINT16 buf_slot_id
	SANITY_CHECK(NUM_WBUF_SLOTS <= 65536);	// UINT16 buf_slot_id
	SANITY_CHECK(NUM_GCBUF_SLOTS <= 65536);	// UINT16 buf_slot_id

	SANITY_CHECK(IS_POWER_OF_TWO(NUM_RBUF_SLOTS) == TRUE);
	SANITY_CHECK(IS_POWER_OF_TWO(NUM_WBUF_SLOTS) == TRUE);

	SANITY_CHECK(IS_POWER_OF_TWO(FLASH_QUEUE_SIZE) == TRUE);
	SANITY_CHECK(IS_POWER_OF_TWO(RESULT_QUEUE_SIZE) == TRUE);
	SANITY_CHECK(FLASH_CMD_TABLE_SIZE <= 256);	// flash_cmd_queue_t::queue[]와 flash_interface_t::result_queue[]가 UINT8로 정의되어 있다.

	SANITY_CHECK(sizeof(flash_cmd_t) == 12);
}

