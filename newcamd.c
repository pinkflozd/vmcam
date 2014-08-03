/**
 * Copyright (c) 2014 Iwan Timmer
 * 
 * This file is part of VMCam.
 * 
 * VMCam is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * VMCam is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with VMCam.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _XOPEN_SOURCE 700

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <openssl/md5.h>

#include "crc32.h"
#include "newcamd.h"

#define NEWCAMD_HDR_LEN 8
#define NEWCAMD_MSG_SIZE 400
#define CWS_FIRSTCMDNO 0xe0

typedef enum {
	MSG_CLIENT_2_SERVER_LOGIN = CWS_FIRSTCMDNO,
	MSG_CLIENT_2_SERVER_LOGIN_ACK,
	MSG_CLIENT_2_SERVER_LOGIN_NAK,
	MSG_CARD_DATA_REQ,
	MSG_CARD_DATA,
	MSG_SERVER_2_CLIENT_NAME,
	MSG_SERVER_2_CLIENT_NAME_ACK,
	MSG_SERVER_2_CLIENT_NAME_NAK,
	MSG_SERVER_2_CLIENT_LOGIN,
	MSG_SERVER_2_CLIENT_LOGIN_ACK,
	MSG_SERVER_2_CLIENT_LOGIN_NAK,
	MSG_ADMIN,
	MSG_ADMIN_ACK,
	MSG_ADMIN_LOGIN,
	MSG_ADMIN_LOGIN_ACK,
	MSG_ADMIN_LOGIN_NAK,
	MSG_ADMIN_COMMAND,
	MSG_ADMIN_COMMAND_ACK,
	MSG_ADMIN_COMMAND_NAK,
	MSG_KEEPALIVE = CWS_FIRSTCMDNO + 0x1d,
} net_msg_type_t;

static void des_key_spread(unsigned char *key, unsigned char *spread) {
	spread[0] =  key[0] & 0xfe;
	spread[1] = ((key[0] << 7) | (key[1] >> 1)) & 0xfe;
	spread[2] = ((key[1] << 6) | (key[2] >> 2)) & 0xfe;
	spread[3] = ((key[2] << 5) | (key[3] >> 3)) & 0xfe;
	spread[4] = ((key[3] << 4) | (key[4] >> 4)) & 0xfe;
	spread[5] = ((key[4] << 3) | (key[5] >> 5)) & 0xfe;
	spread[6] = ((key[5] << 2) | (key[6] >> 6)) & 0xfe;
	spread[7] = key[6] << 1;
	spread[8] = key[7] & 0xfe;
	spread[9] = ((key[7] << 7)  | (key[8] >> 1)) & 0xfe;
	spread[10] = ((key[8] << 6)  | (key[9] >> 2)) & 0xfe;
	spread[11] = ((key[9] << 5)  | (key[10] >> 3)) & 0xfe;
	spread[12] = ((key[10] << 4) | (key[11] >> 4)) & 0xfe;
	spread[13] = ((key[11] << 3) | (key[12] >> 5)) & 0xfe;
	spread[14] = ((key[12] << 2) | (key[13] >> 6)) & 0xfe;
	spread[15] = key[13] << 1;

	DES_set_odd_parity((DES_cblock *)&spread[0]);
	DES_set_odd_parity((DES_cblock *)&spread[8]);
}

int newcamd_init(struct newcamd *c, const unsigned char* user, const unsigned char* pass, const unsigned char* key) {
	unsigned char random[14];
	unsigned char spread[16];
	int i;

	write(c->client_fd, random, sizeof(random));

	memcpy(c->key, key, 14);
	c->pass = crypt(pass, "$1$abcdefgh$");

	for(i = 0; i < 14; ++i) {
		random[i] = random[i] ^ key[i];
	}
	des_key_spread(random, spread);

	DES_key_sched((DES_cblock *)&spread[0], &c->ks1);
	DES_key_sched((DES_cblock *)&spread[8], &c->ks2);
}

void printHex(char* type, unsigned char* data, int length) {
	int i;
	printf("%s", type);
	for (i = 0;i<length;i++)
		printf(" %02x", data[i]);

	printf("\n");
}

int newcamd_handle(struct newcamd *c, int32_t (*f)(unsigned char*, unsigned char*)) {
	unsigned char data[NEWCAMD_MSG_SIZE];
	unsigned char response[NEWCAMD_MSG_SIZE];
	unsigned char key[16];
	unsigned int data_len, i;
	unsigned char *user, *password;
	uint16_t msg_id, service_id;

	if ((data_len = newcamd_recv(c, data, &service_id, &msg_id)) == -1)
		return -1;

	switch(data[0]) {
		case MSG_CLIENT_2_SERVER_LOGIN:
			user = data + 2;
			password = user + strlen(user) + 1;

			response[0] = MSG_CLIENT_2_SERVER_LOGIN_ACK;
			if (strcmp(password, c->pass)==0) {
				response[0] = MSG_CLIENT_2_SERVER_LOGIN_ACK;
				newcamd_send(c, response, 3, 0, 0);

				for (i = 0; i < strlen(password); i++)
					c->key[i%14] ^= password[i];

				des_key_spread(c->key, key);
				DES_key_sched((DES_cblock *)&key[0], &c->ks1);
				DES_key_sched((DES_cblock *)&key[8], &c->ks2);
				break;
			} else {
				response[0] = MSG_CLIENT_2_SERVER_LOGIN_NAK;
				newcamd_send(c, response, 3, 0, 0);
				printf("[NEWCAMD] Password incorrect\n");
				return -1;
			}			
		case MSG_CARD_DATA_REQ:
			memset(response, 0, 14+12);
			response[0] = MSG_CARD_DATA;

			//Provide CAID
			response[4] = 0x56;
			response[5] = 0x01;

			response[14] = 1; //Set number of cards
			response[17] = 1; //Set provider ID of card 1
			newcamd_send(c, response, 14+12, service_id, msg_id);
			break;
		case MSG_KEEPALIVE:
			newcamd_send(c, data, data_len, service_id, msg_id);
		case 0x80:
		case 0x81:
			f(response + 3, data);
			response[0] = data[0];
			newcamd_send(c, response, 32 + 3, service_id, msg_id);
			break;
		case 0x00:
			printf("[NEWCAMD] Strange code %d\n", data[0]);
			break;
		default:
			printf("[NEWCAMD] Unknown code %d\n", data[0]);
			return -1;
	}
}

int newcamd_recv(struct newcamd *c, unsigned char* data, uint16_t* service_id, uint16_t* msg_id) {
	DES_cblock ivec;
	unsigned char buffer[NEWCAMD_MSG_SIZE];
	unsigned char checksum = 0;
	unsigned int len, retlen, i;

	if (!read(c->client_fd, buffer, 2))
		return -1;

	len = ((buffer[0] << 8) | buffer[1]) & 0xFFFF;

	if (len > NEWCAMD_MSG_SIZE) {
		printf("[NEWCAMD] Message too long\n");
		return -1;
	}

	if (read(c->client_fd, buffer, len) < len) {
		printf("[NEWCAMD] Received message too short\n");
	}

	len -= sizeof(ivec);
	memcpy(ivec, buffer+len, sizeof(ivec));
	DES_ede2_cbc_encrypt(buffer, buffer, len, &c->ks1, &c->ks2, (DES_cblock *)ivec, DES_DECRYPT);

	for (i = 0; i < len; i++)
		checksum ^= buffer[i];

	if (checksum) {
		printf("[NEWCAMD] Checksum failed.\n");
		return -1;
	}

	*msg_id = ((buffer[2] << 8) | buffer[3]) & 0xFFFF;
	*service_id = ((buffer[4] << 8) | buffer[5]) & 0xFFFF;

	retlen = (((buffer[3 + NEWCAMD_HDR_LEN] << 8) | buffer[4 + NEWCAMD_HDR_LEN]) & 0x0FFF) + 3;
	memcpy(data, buffer + 2 + NEWCAMD_HDR_LEN, retlen);
	return retlen;
}

int newcamd_send(struct newcamd *c, unsigned char* data, int data_len, uint16_t service_id, uint16_t msg_id) {
	unsigned char checksum;
	unsigned char buffer[NEWCAMD_MSG_SIZE];
	unsigned int padding_len, buf_len, i;

	memset(buffer + 2, 0, NEWCAMD_HDR_LEN + 2);
	memcpy(buffer + NEWCAMD_HDR_LEN + 4, data, data_len);

	buffer[NEWCAMD_HDR_LEN + 4 + 1] = (data[1] & 0xF0) | (((data_len - 3) >> 8) & 0x0F);
	buffer[NEWCAMD_HDR_LEN + 4 + 2] = (data_len - 3) & 0xFF;

	buffer[2] = msg_id >> 8;
	buffer[3] = msg_id & 0xFF;
	buffer[4] = service_id >> 8;
	buffer[5] = service_id & 0xFF;

	DES_cblock padding;
	buf_len = data_len + NEWCAMD_HDR_LEN + 4;
	padding_len = (8 - ((buf_len - 1) % 8)) % 8;

	DES_random_key(&padding);
	memcpy(buffer + buf_len, padding, padding_len);
	buf_len += padding_len;
	for (i = 2; i < buf_len; i++)
		checksum ^= buffer[i];

	buffer[buf_len++] = checksum;

	DES_cblock ivec;
	DES_random_key(&ivec);
	memcpy(buffer + buf_len, ivec, sizeof(ivec));
	DES_ede2_cbc_encrypt(buffer + 2, buffer + 2, buf_len - 2, &c->ks1, &c->ks2, (DES_cblock *)ivec, DES_ENCRYPT);

	buf_len += sizeof(DES_cblock);
	buffer[0] = (buf_len - 2) >> 8;
	buffer[1] = (buf_len - 2) & 0xFF;

	return write(c->client_fd, buffer, buf_len);
}