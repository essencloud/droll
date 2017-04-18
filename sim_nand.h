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


// NAND와 FIL 사이의 인터페이스

typedef struct
{
	// 컨트롤러에서 NAND로 전송되는 data packet (big wordline)

	UINT8	main_data[PAGES_PER_WL][PLANES_PER_DIE][SECTORS_PER_SMALL_PAGE];		// user data의 경우에는 한 섹터당 한 바이트만
	UINT32	extra_data[PAGES_PER_WL][PLANES_PER_DIE][SLICES_PER_SMALL_PAGE];		// 4KB당 4byte

} nand_packet_t;


UINT64 sim_nand_read_userdata(UINT8 die_index, UINT16 small_blk_index, UINT16 wl_index, UINT8 lcm, UINT8 slice, UINT32* extra_data);
void sim_nand_read_metadata(UINT8 die_index, UINT16 small_blk_index, UINT16 wl_index, UINT8 lcm, UINT8 slice, UINT8* buf);
void sim_nand_write_userdata(UINT32 die, UINT16* small_blk_index, UINT32 wl_index, nand_packet_t* nand_pkt);
void sim_nand_write_metadata_sp(UINT32 die, UINT16 small_blk_index, UINT32 wl_index, UINT8* buf);
void sim_nand_write_metadata_mp(UINT32 die, UINT16* small_blk_index, UINT32 wl_index, UINT8* buf);
void sim_nand_erase_sp(UINT32 die, UINT16 small_blk_index);
void sim_nand_erase_mp(UINT32 die, UINT16* small_blk_index);
BOOL8 sim_nand_busy(UINT32 die);


