#ifndef _TCP_SERVER_H_
#define _TCP_SERVER_H_

#include "evweb.h"

void start_tcp_server(EV_P, int port, evweb_server_settings* settings);
void close_tcp_server();

#endif

