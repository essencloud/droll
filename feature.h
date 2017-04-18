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


#ifndef FEATURE_H
#define FEATURE_H

// �Ʒ� �� ���� �ϳ��� 1�� ����

#define OPTION_DRAM_SSD				0		// ��¥ FTL (DRAM�� ����������� ����ϰ� NAND�� ���� �������� ����)
#define OPTION_SIMPLE_FTL			1		// ������ ����� FTL (GC�� �ϰ� SPOR�� ����� ����ó�� ����)
#define OPTION_DROLL_FTL			0		// ��¥ FTL

#define OPTION_FAST_DRAM_SSD		1		// OPTION_DRAM_SSD �� ����ϴ� ��쿡�� ����
#define OPTION_THREAD_SYNC			3		// 1, 2, 3 ���� �ϳ� (3�� ���� ����, 2�� ���� ���� ��� CPU�� �ſ� ���� ���)
#define OPTION_VERIFY_DATA			1		// SSD���� ���� �����Ͱ� ������ �ű⿡ ��� �����Ͱ� �´��� �˻� - sudden power loss �ÿ��� ��� �Ұ�
#define OPTION_INSIGHT				0		// Agilent LPA ���α׷����� ��� �� �ִ� ���� ���� (���� ���� �ð�ȭ) - Agilent license �ʿ� - �̿ϼ�, �̰���
#define OPTION_ASSERT				1		// �����

#define VERBOSE_HOST_STATISTICS		1
#define VERBOSE_FTL_STATISTICS		1
#define VERBOSE_BUF_STATISTICS		1
#define VERBOSE_NAND_STATISTICS		1
#define VEBOSE_HOST_PROTOCOL		0
#define VEBOSE_HIL_PROTOCOL			0
#define VERBOSE_FTL_CMD				0


#endif	// FEATURE_H
