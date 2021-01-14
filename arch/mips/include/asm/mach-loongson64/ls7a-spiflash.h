/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2019 Loongson Technology Corp.
 * Author: Chenyang Li, lichenyang@loongson.cn
 */

#ifndef __LS_SPIFLASH_H__
#define __LS_SPIFLASH_H__

unsigned char ls_spiflash_read_status(void);
int ls_spiflash_sectors_erase(int addr, int data_len);
int ls_spiflash_read(int addr, unsigned char *buf,int data_len);
int ls_spiflash_write(int addr, unsigned char *buf,int data_len);

#endif
