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


#ifndef SATA_H
#define SATA_H

#define NCQ_SIZE			32		// SATA �Ծ࿡�� ����ϴ� outstanding command�� �ִ� ����. must be power of two
#define DATA_FIS_SIZE_MAX	16		// SATA �Ծ࿡�� ����ϴ� Data FIS�� �ִ� ���̴� 16 sectors = 8KB
#define SECTOR_COUNT_MAX	65536	// SATA �Ծ࿡�� ����ϴ� command size�� �ִ� 65536 sectors = 32MB


#define SIM_SATA_BYTE_PER_SEC			(6000000000ULL * 8 / 10 / 8)		// 6Gbps with 8/10 encoding = 4800000000 bit/sec = 600000000 byte/sec = 572.2MiB/sec
#define SIM_SATA_NANOSEC_PER_SECTOR		((UINT64) (BYTES_PER_SECTOR * 1000000000.0 / SIM_SATA_BYTE_PER_SEC))	// 853 nsec
#define SIM_SATA_NANOSEC_PER_BYTE		(1000000000.0 / SIM_SATA_BYTE_PER_SEC)	// 1.67 nsec

// �Ʒ��� ������ ȣ��Ʈ �� ����̽� �ϵ������ ���ɿ� ���� �޶���

#define SIM_SATA_CMD_DELAY				1350		// SATA IDLE ���°� �ǰ� ���� ȣ��Ʈ ���(Register FIS H2D)�� ������ ���۵Ǳ������ �ð� ����
#define SIM_SATA_CMD_TIME				((UINT64) (36 * SIM_SATA_NANOSEC_PER_BYTE + 300))				// ȣ��Ʈ ����� �����ϴ� ���� �ɸ��� �ð�

#define SIM_SATA_ACK_DELAY				200			// ȣ��Ʈ ����� �ް� ���� ����(Register FIS D2H)�� ���۵Ǳ������ �ð� ����
#define SIM_SATA_ACK_TIME				((UINT64) (36 * SIM_SATA_NANOSEC_PER_BYTE + 300))			// ������ �����ϴ� ���� �ɸ��� �ð�

#define SIM_SATA_SETUP_R_DELAY			500			// DMA SETUP FIS for read
#define SIM_SATA_SETUP_W_DELAY			500			// DMA SETUP FIS for write
#define SIM_SATA_SETUP_TIME				((UINT64) (44 * SIM_SATA_NANOSEC_PER_BYTE + 300))

#define SIM_SATA_ACTV_DELAY				200			// DMA ACTIVATE FIS
#define SIM_SATA_ACTV_TIME				((UINT64) (20 * SIM_SATA_NANOSEC_PER_BYTE + 300))

#define SIM_SATA_SDB_DELAY				100			// Set Device Bits FIS
#define SIM_SATA_SDB_TIME				((UINT64) (24 * SIM_SATA_NANOSEC_PER_BYTE + 300))

#define SIM_SATA_WDATA_DELAY			200
#define SIM_SATA_RDATA_DELAY			300


#define HOT_SPOT_SIZE			(SECTORS_PER_BIG_WL * WLS_PER_BLK * NUM_DIES)



typedef struct sata_cmd
{
	UINT8	code;
	UINT8	ncq_tag;
	UINT16	unused_2;
	UINT32	lba;		// �б�/���� ����� ���� ���� �ּ�

	UINT32	num_sectors_requested;
	UINT32	num_sectors_completed;

	UINT64	submit_time;

	struct sata_cmd* link_ptr;
} sim_sata_cmd_t;


#endif

