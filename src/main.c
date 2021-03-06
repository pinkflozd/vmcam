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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <err.h>
#include <string.h>
#include <pthread.h>

#include "newcamd.h"
#include "cs378x.h"
#include "keyblock.h"
#include "vm_api.h"
#include "log.h"
#include "var_func.h"

struct handler {
	int sock;
	void * (*callback)(void *);
	char * user;
	char * pass;
	char * des_key;
};

struct client_data {
	int client_fd;
	char * user;
	char * pass;
	char * des_key;
};

void *handle_client(void * handle) {
	struct sockaddr_in cli_addr;
	socklen_t sin_len = sizeof(cli_addr);
	struct handler* server = handle;
	pthread_t thread;
	struct client_data client_data;

	while (1) {
		client_data.client_fd = accept(server->sock, (struct sockaddr *) &cli_addr, &sin_len);
		if (client_data.client_fd == -1) {
			perror("[VMCAM] Can't accept");
			continue;
		}

		LOG(INFO, "[VMCAM] Got connection");

		client_data.user = (*server).user;
		client_data.pass = (*server).pass;
		client_data.des_key = (*server).des_key;

		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		pthread_create(&thread, &attr, server->callback, &client_data);

		pthread_attr_destroy(&attr);
	}
}

void *handle_client_newcamd(void * client_data) {
	struct client_data * cd = client_data;
	int fd = (*cd).client_fd;
	struct newcamd c;

	c.client_fd = fd;
	newcamd_init(&c, (*cd).user, (*cd).pass, (*cd).des_key);
	while (newcamd_handle(&c, keyblock_analyse_file) != -1);
	LOG(INFO, "[VMCAM] Connection closed");

	close(fd);
}

void *handle_client_cs378x(void * client_data) {
	struct client_data * cd = client_data;
	int fd = (*cd).client_fd;
	struct cs378x c;

	c.client_fd = fd;
	cs378x_init(&c, (*cd).user, (*cd).pass);
	while (cs378x_handle(&c, keyblock_analyse_file) != -1);
	LOG(INFO, "[VMCAM] Connection closed");

	close(fd);
}

int open_socket(char* interface, char* host, int port) {
	int one = 1;
	struct sockaddr_in svr_addr;
	int sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0)
		err(1, "[VMCAM] Can't open socket");

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));

	svr_addr.sin_family = AF_INET;
	inet_aton(host, &svr_addr.sin_addr);
	svr_addr.sin_port = htons(port);

	if (bind(sock, (struct sockaddr *) &svr_addr, sizeof(svr_addr)) == -1) {
		close(sock);
		err(1, "[VMCAM] Can't bind on %s:%d for %s", host, port, interface);
	}

	listen(sock, 5);
	LOG(INFO, "[VMCAM] Start %s server on port %d", interface, port);

	return sock;
}

int main(int argc, char *argv[]) {
	int ret;
	int i;
	int usage = 0;
	int initial = 1;

	// vm_api config
	char * vm_api_company = NULL;
	char * vm_aminoMAC = NULL;
	char * vm_machineID = NULL;
	char * vm_cache_dir = NULL;
	char * vm_VCAS_server = NULL;
	char * vm_VKS_server = NULL;
	unsigned int vm_VCAS_port = 0;
	unsigned int vm_VKS_port = 0;
	unsigned int vm_key_interval = 300;

	unsigned int keyblockonly = 0;
	unsigned int port_cs378x = 15080;
	unsigned int port_newcamd = 15050;
	char * user = NULL;
	char * pass = NULL;
	char des_key[14];
        char default_des_key[14] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14};
        
        
	char * config = NULL;
	char * host = NULL;
	int vm_protocolVersion = 1154;
        int debug = -1;
	struct handler newcamd_handler, cs378x_handler;
	pthread_t thread;
	debug_level = 0;

        FILE * fp;
        int scan;
        char key[31], value[31];

	printf("VMCam - VCAS SoftCAM for IPTV\n");

	// Initialise default values
	str_realloc_copy(&vm_cache_dir, "/var/cache/vmcam");
	str_realloc_copy(&config, "/etc/vmcam.ini");
	str_realloc_copy(&host, "0.0.0.0");
	str_realloc_copy(&user, "user");
	str_realloc_copy(&pass, "pass");
        str_realloc_copy(&vm_aminoMAC, "001122334455");
        memcpy(des_key, default_des_key, 14);
        
	// Load config file first...
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			if (i+1 >= argc) {
				printf("Need to provide a config file\n");
				return -1;
			}
			config = argv[i+1];
			i++;
		}
	}
        if ((fp = fopen(config, "r"))) {
                while ((scan = fscanf(fp, "%30[^=\n]=%255s\n?", key, value)) != EOF) {
                        if (scan == 1) {
                                fseek(fp, 1, SEEK_CUR); //Skip EOL
                        } else {
				if (strcmp(key, "CACHE_DIR") == 0) {
                                        str_realloc_copy(&vm_cache_dir, value);
                                } else if (strcmp(key, "DEBUG_LEVEL") == 0) {
                                        debug_level = atoi(value);
                                } else if (strcmp(key, "PROTOCOL") == 0) {
                                        vm_protocolVersion = atoi(value);
                               	} else if (strcmp(key, "AMINOMAC") == 0) {
	                                strncpy(vm_aminoMAC, value, 12);
                                        vm_aminoMAC[12] = '\0';
                               	} else if (strcmp(key, "MACHINEID") == 0) {
                                        vm_machineID = malloc(64);
	                                strncpy(vm_machineID, value, 64);
                                        vm_machineID[63] = '\0';
				} else if (strcmp(key, "VCASSERVERADDRESS") == 0) {
	                                str_realloc_copy(&vm_VCAS_server, value);
                                } else if (strcmp(key, "VCASSERVERPORT") == 0) {
	                                vm_VCAS_port = atoi(value);
                                } else if (strcmp(key, "VKSSERVERADDRESS") == 0) {
	                                str_realloc_copy(&vm_VKS_server, value);
                                } else if (strcmp(key, "VKSSERVERPORT") == 0) {
                                       	vm_VKS_port = atoi(value);
				} else if (strcmp(key, "COMPANY") == 0) {
					str_realloc_copy(&vm_api_company, value);
                                } else if (strcmp(key, "KEY_INTERVAL") == 0) {
	                                vm_key_interval = atoi(value);
				} else if (strcmp(key, "NEWCAMD_PORT") == 0) {
					port_newcamd = atoi(value);
				} else if (strcmp(key, "CS378X_PORT") == 0) {
					port_cs378x = atoi(value);
				} else if (strcmp(key, "LISTEN_IP") == 0) {
					str_realloc_copy(&host, value);
				} else if (strcmp(key, "USERNAME") == 0) {
					str_realloc_copy(&user, value);
				} else if (strcmp(key, "PASSWORD") == 0) {
					str_realloc_copy(&pass, value);
				} else if (strcmp(key, "DES_KEY") == 0) {
					ret = sscanf(value, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
							&des_key[0], &des_key[1], &des_key[2], &des_key[3],
							&des_key[4], &des_key[5], &des_key[6], &des_key[7],
							&des_key[8], &des_key[9], &des_key[10], &des_key[11],
							&des_key[12], &des_key[13]);
                       		        if (ret != 14) {
               		                        printf("Provided key is not a DES key\n");
               		                        return -1;
	                                }
                                }
                        }
                }
                LOG(INFO, "[VMCAM] Config file loaded");
                free(config);
                config = NULL;
        } else {
                LOG(ERROR, "[VMCAM] Unable to read configfile %s", config);
                return -1;
        }
        
	// ...then override from command line
	for (i = 1; i < argc && usage == 0; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			i++;
		} else if (strcmp(argv[i], "-a") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide a MAC address\n");
					return -1;
				}
				strncpy(vm_aminoMAC, argv[i+1], 12);
				i++;
		} else if (strcmp(argv[i], "-pn") == 0) {
				if (keyblockonly == 1)
					break;
				if (i+1 >= argc) {
					printf("Need to provide a Newcamd port number\n");
					return -1;
				}
				port_newcamd = atoi(argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-pc") == 0) {
				if (keyblockonly == 1)
					break;
				if (i+1 >= argc) {
					printf("Need to provide a CS378x port number\n");
					return -1;
				}
				port_cs378x = atoi(argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-d") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide a debug level\n");
					return -1;
				}
				debug_level = debug = atoi(argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-i") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide a machine ID\n");
					return -1;
				}
                                vm_machineID = malloc(64);
				strncpy(vm_machineID, argv[i+1], 64);
				i++;
		} else if (strcmp(argv[i], "-m") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide a protocol version\n");
					return -1;
				}
				vm_protocolVersion = atoi(argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-u") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide a username\n");
					return -1;
				}
				str_realloc_copy(&user, argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-p") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide a password\n");
					return -1;
				}
				str_realloc_copy(&pass, argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-k") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide a DES key\n");
					return -1;
				}
				ret = sscanf(argv[i+1], "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
						&des_key[0], &des_key[1], &des_key[2], &des_key[3],
						&des_key[4], &des_key[5], &des_key[6], &des_key[7],
						&des_key[8], &des_key[9], &des_key[10], &des_key[11],
						&des_key[12], &des_key[13]);
				if (ret != 14) {
					printf("Provided key is not a DES key\n");
					return -1;
				}
				i++;
		} else if (strcmp(argv[i], "-l") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide a ip address\n");
					return -1;
				}
				str_realloc_copy(&host, argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-noinitial") == 0) {
				initial = 0;
		} else if (strcmp(argv[i], "-ps") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide the VCAS port number\n");
					return -1;
				}
				vm_VCAS_port = atoi(argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-pk") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide the VKS port number\n");
					return -1;
				}
				vm_VKS_port = atoi(argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-sk") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide the VKS address\n");
					return -1;
				}
				str_realloc_copy(&vm_VKS_server, argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-ss") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide the VCAS address\n");
					return -1;
				}
				str_realloc_copy(&vm_VCAS_server, argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-C") == 0) {
				if (i+1 >= argc) {
					printf("Need to provide the company name\n");
					return -1;
				}
				str_realloc_copy(&vm_api_company, argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-t") == 0) {
				if (i+1 >= argc) {
					printf("Need interval of key retrieval updates\n");
					return -1;
				}
				vm_key_interval = atoi(argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-e") == 0) {
				if (i+1 >= argc) {
					printf("Need name of cache directory\n");
					return -1;
				}
				str_realloc_copy(&vm_cache_dir, argv[i+1]);
				i++;
		} else if (strcmp(argv[i], "-keyblockonly") == 0) {
				port_newcamd = 0;
				port_cs378x = 0;
				keyblockonly = 1;
		} else {
			printf("Unknown option '%s'\n", argv[i]);
			usage = 1;
		}
	}

	if (usage) {
		printf("Usage: vmcam [options]\n\n");
		printf("\t-e [directory]\t\tDirectory to store cache files [default: /var/cache/vmcam]\n");
		printf("\t-d [debug level]\tSet debug level [default: 0]\n\n");
		printf("  VCAS/VKS:\n\n");
		printf("\t-c [configfile]\t\tVCAS configfile [default: vmcam.ini]\n");
		printf("\t-a [Amino MAC]\t\tYour Amino MAC address [format: 010203040506]\n");
		printf("\t-i [Machine ID]\t\tYour Amino machine ID [default: <Amino MAC>]\n");
		printf("\t-m [protocol version]\tProtocol verion to use [1154 and 1155 supported]\n");
		printf("\t-ss [VCAS address]\tSet VCAS hostname to connect to\n");
		printf("\t-sk [VKS address]\tSet VKS hostname to connect to\n");
		printf("\t-ps [VCAS port]\t\tSet VCAS port number to connect to\n");
		printf("\t-pk [VKS port]\t\tSet VKS port number to connect to\n");
		printf("\t-C [Company name]\tSet name of company for key retreival\n");
		printf("\t-t [interval]\t\tInterval for updating keys [default: 300]\n");
		printf("\t-noinitial\t\tSkip initial keyblock retrieval\n\n");
		printf("  Newcamd/CS378x:\n\n");
		printf("\t-pn [Newcamd port]\tSet Newcamd port number or 0 to disable [default: 15050]\n");
		printf("\t-pc [CS378x port]\tSet CS378x port number or 0 to disable [default: 15080]\n");
		printf("\t-l [ip addres]\t\tListen on ip address [default: 0.0.0.0]\n");
		printf("\t-u [username]\t\tSet allowed user on server [default: user]\n");
		printf("\t-p [password]\t\tSet password for server [default: pass]\n");
		printf("\t-k [DES key]\t\tSet DES key for Newcamd [default: 0102030405060708091011121314]\n");
		printf("\t-keyblockonly\t\tDisable Newcamd and CS378x (will override related port settings)\n");
		return -1;
	}

	vm_config(vm_VCAS_server, vm_VCAS_port, vm_VKS_server, vm_VKS_port, vm_api_company, vm_cache_dir, vm_aminoMAC, vm_machineID, vm_protocolVersion);
        free(vm_VCAS_server);
        free(vm_VKS_server);
        free(vm_api_company);
        free(vm_cache_dir);
        if (vm_machineID != NULL) {
            free(vm_machineID);
        }
        vm_VCAS_server = NULL;
        vm_VKS_server = NULL;
        vm_api_company = NULL;
        vm_cache_dir = NULL;

	if ((ret = init_vmapi()) == EXIT_FAILURE)
		return ret;

	if (initial) {
		if ((ret = load_keyblock()) == EXIT_FAILURE)
			return ret;
	}

	if (port_newcamd > 0) {
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		newcamd_handler.sock = open_socket("Newcamd", host, port_newcamd);
		newcamd_handler.callback = handle_client_newcamd;
		newcamd_handler.user = user;
		newcamd_handler.pass = pass;
		newcamd_handler.des_key = des_key;
		pthread_create(&thread, &attr, handle_client, &newcamd_handler);

		pthread_attr_destroy(&attr);
	}

	if (port_cs378x > 0) {
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		cs378x_handler.sock = open_socket("CS378x", host, port_cs378x);
		cs378x_handler.callback = handle_client_cs378x;
		cs378x_handler.user = user;
		cs378x_handler.pass = pass;
		cs378x_handler.des_key = NULL;
		pthread_create(&thread, &attr, handle_client, &cs378x_handler);

		pthread_attr_destroy(&attr);
	}

	while (1) {
		LOG(INFO, "[VMCAM] Next keyblock update in %d seconds", vm_key_interval);
		sleep(vm_key_interval);
		load_keyblock();
	}
}
