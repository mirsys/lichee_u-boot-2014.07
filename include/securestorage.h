/*
 * Copyright (C) Freescale Semiconductor, Inc. 2006.
 * Author: Jason Jin<Jason.jin@freescale.com>
 *         Zhang Wei<wei.zhang@freescale.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 */
#ifndef __SECURE_STORAGE_H__
#define __SECURE_STORAGE_H__

extern int sunxi_secure_storage_init(void);
extern int sunxi_secure_storage_exit(void);

extern int sunxi_secure_storage_list(void);
extern int sunxi_secure_storage_probe(const char *item_name);
extern int sunxi_secure_storage_read(const char *item_name, char *buffer, int length, int *data_len);

extern int sunxi_secure_storage_write(const char *item_name, char *buffer, int length);
extern int sunxi_secure_storage_erase(const char *item_name);
extern int sunxi_secure_storage_erase_all(void);
extern int sunxi_secure_storage_erase_data_only(const char *item_name);

extern int sunxi_secure_object_down( const char *name , char *buf, int len, int encrypt, int write_protect);
extern int sunxi_secure_object_up(const char *name);



extern int smc_load_sst_encrypt(
		char *name,
		char *in, unsigned int len,
		char *out, unsigned int *outLen);

typedef struct
{
    //以下信息重复，表示每个key的信息
    char     name[64];      //key的名称
	uint32_t len;           //长度
	uint32_t encrypted;     //判断是否经过加密，需要解密
	uint32_t write_protect; //判断是否允许覆盖
    char    key_data[4096 - 64 - 4 - 4 - 4];//这是一个数组，存放key的全部信息，数据长度由len指定
                            //注意，传入的数据中，这个位置存放的直接就是数据，而不是地址
}
sunxi_secure_storage_info_t;


#endif
