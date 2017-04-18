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

// 아래 셋 중의 하나를 1로 설정

#define OPTION_DRAM_SSD				0		// 가짜 FTL (DRAM을 저장공간으로 사용하고 NAND는 전혀 접근하지 않음)
#define OPTION_SIMPLE_FTL			1		// 간단한 시험용 FTL (GC만 하고 SPOR을 비롯한 예외처리 없음)
#define OPTION_DROLL_FTL			0		// 진짜 FTL

#define OPTION_FAST_DRAM_SSD		1		// OPTION_DRAM_SSD 를 사용하는 경우에만 적용
#define OPTION_THREAD_SYNC			3		// 1, 2, 3 중의 하나 (3이 가장 빠름, 2는 가장 느린 대신 CPU를 매우 적게 사용)
#define OPTION_VERIFY_DATA			1		// SSD에서 읽은 데이터가 예전에 거기에 썼던 데이터가 맞는지 검사 - sudden power loss 시에는 사용 불가
#define OPTION_INSIGHT				0		// Agilent LPA 프로그램으로 열어볼 수 있는 파일 생성 (내부 동작 시각화) - Agilent license 필요 - 미완성, 미검증
#define OPTION_ASSERT				1		// 디버깅

#define VERBOSE_HOST_STATISTICS		1
#define VERBOSE_FTL_STATISTICS		1
#define VERBOSE_BUF_STATISTICS		1
#define VERBOSE_NAND_STATISTICS		1
#define VEBOSE_HOST_PROTOCOL		0
#define VEBOSE_HIL_PROTOCOL			0
#define VERBOSE_FTL_CMD				0


#endif	// FEATURE_H
