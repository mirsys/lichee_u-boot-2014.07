/*
 * (C) Copyright 2000-2003
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
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
 */

/*
 * Allwinner secure storage data format
 */
#include <common.h>
#include <command.h>
#include <asm-generic/errno.h>
#include <malloc.h>
#include <asm/io.h>
#include <securestorage.h>
#include <sunxi_board.h>
#include <smc.h>
#include "sprite_storage_crypt.h"
#include <asm/arch/ss.h>
#include <linux/ctype.h>

int sunxi_secure_object_down( const char *name , char *buf, int len, int encrypt, int write_protect)
{
	int  ret, i;
	char lower_name[64];
	sunxi_secure_storage_info_t secdata;

	if (len > 4096) {
		printf("the input key is too long!\n");

		return -1;
	}
	memset(&secdata, 0, sizeof(secdata));
	i = 0;
	memset(lower_name, 0, 64);
	while(name[i] != '\0')
	{
		lower_name[i] = tolower(name[i]);
		i++;
	}
	strcpy(secdata.name, lower_name);
	secdata.encrypted = encrypt;
	secdata.write_protect = write_protect;
	secdata.len = len;
	ret = smc_tee_ssk_encrypt(secdata.key_data, buf, len);
	if (ret) {
		printf("ssk encrypt failed\n");

		return -1;
	}

	sunxi_dump(secdata.key_data, len);

	ret = sunxi_secure_storage_write(lower_name, (char *)&secdata, 64 + 4 + 4 + 4 + len);
	if (ret) {
		printf("secure storage write fail\n");

		return -1;
	}

	return 0;
}

int sunxi_secure_object_up(const char *name)
{
	sunxi_secure_storage_info_t secdata;
	int data_len;
	int ret;

	memset(&secdata, 0, sizeof(secdata));
	ret = sunxi_secure_storage_read(name, (char *)&secdata, 4096, &data_len);
	if (ret) {
		printf("secure storage read fail\n");

		return -1;
	}

	ret = smc_tee_keybox_store(name, (char *)&secdata, data_len);
	if (ret) {
		printf("ssk encrypt failed\n");

		return -1;
	}

	return 0;
}

static int cmd_secure_object(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	int ret = -1;
//
//	if(argc >3 || argc <1){
//		printf("wrong argc\n");
//		return -1 ;
//	}
//
//	if( sunxi_secure_storage_init() < 0  ){
//		printf("secure storage init fail\n");
//		return -1 ;
//	}
//
//	if ( (strncmp("clean", argv[1],strlen("clean")) == 0)){
//		if( strncmp("all", argv[2], strlen("all") ) ==0  ){
//			ret = clear_secure_store(0xffff);
//		}else{
//			unsigned int index = simple_strtoul( (const char *)argv[2], NULL, 10 ) ;
//			ret = clear_secure_store(index) ;
//		}
//	}else if(strncmp("dump", argv[1], strlen("dump"))== 0){
//		ret = dump_secure_store(argv[2]);
//	}else if( (strncmp("test", argv[1],strlen("test")) == 0) ) {
//		#ifdef _SO_TEST_
//		ret = secure_object_op_test();
//		#else
//		ret = cmd_usage(cmdtp);
//		#endif
//	}else if(strncmp("crypt", argv[1], strlen("crypt"))== 0){
//#ifdef CONFIG_SUNXI_SECURE_SYSTEM
//		extern int smc_load_sst_test(void);
//		smc_load_sst_test();
//#endif
//		ret = 0 ;
//	}else
//		ret = cmd_usage(cmdtp);
//
//	if( sunxi_secure_storage_exit() < 0  ){
//		printf("secure storage exit fail\n");
//		return -1 ;
//	}
	return ret;
}

U_BOOT_CMD(
	sunxi_so, 3, 1,	cmd_secure_object,
	"sunxi_so sub-system",
	"sunxi_so <cmd> \n"
	"\t Allwinner secure object storage \n"
	);

//extern int smc_tee_ssk_decrypt(char *out_buf, char *in_buf, int len);

#if defined(CONFIG_SUNXI_WIDEVINE_KEYBOX)
int sunxi_widevine_keybox_install(void)
{
	int ret = -1;
	char buffer[4096];
	//char en_buf[4096];
	//char de_buf[4096];
	int  data_len;
//	sunxi_secure_storage_info_t *secdata;
	int workmode = uboot_spare_head.boot_data.work_mode;

	if(!((workmode == WORK_MODE_BOOT) ||
		(workmode == WORK_MODE_CARD_PRODUCT) ||
		(workmode == WORK_MODE_SPRITE_RECOVERY)))
	return 0;

	if( sunxi_secure_storage_init() < 0  ) {
		printf("secure storage init fail\n");
		return 0;
	}

	memset(buffer, 0, 4096);
	ret = sunxi_secure_storage_read("widevine", buffer, 4096, &data_len);
	if (ret) {
		printf("secure storage read fail\n");
		return 0;
	}

//	secdata = (sunxi_secure_storage_info_t *)buffer;
//	printf("total data len=%d\n", data_len);
//	printf("source data_len=%d\n", secdata->len);
//	sunxi_dump(secdata->key_data, secdata->len);

//	ret = smc_tee_ssk_decrypt(en_buf, secdata->key_data, secdata->len);
//	if (ret) {
//		printf("secure storage decrypt fail\n");
//		return -1;
//	}
//
//	printf("en data data_len=%d\n", secdata->len);
//	sunxi_dump(en_buf, secdata->len);
//
//	printf("secure os en\n");
//	memset(de_buf, 0, 4096);
//	ret = smc_tee_ssk_encrypt(de_buf, en_buf, secdata->len);
//	if (ret) {
//		printf("secure storage encrypt fail\n");
//		return -1;
//	}
//	sunxi_dump(de_buf, secdata->len);

//	printf("test\n");
//	sunxi_dump(buffer, data_len);
	ret = smc_tee_keybox_store("widevine", buffer, data_len);
	if (!ret)
		printf("key install finish");
	else
		printf("key install fail");

	return 0;
}
#endif
