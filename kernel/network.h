#ifndef OSP_NETWORK_H
#define OSP_NETWORK_H

#include "kernel.h"

void network_initialize(void);
void network_poll(void);
void network_status(void);
void network_dhcp(void);
void network_ping(const char *target);
void network_dns(const char *hostname);
void network_http(const char *url);
void network_configure(char *arguments);

#endif
