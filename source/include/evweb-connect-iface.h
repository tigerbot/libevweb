#ifndef _EVWEB_CONNECT_IFACE_H_
#define _EVWEB_CONNECT_IFACE_H_

#include <ev.h>
#include "evweb.h"

typedef struct {
  int cb_count;
  int max_cb_count;
  void* cbs;
} evweb_connect_iface;

typedef void (evweb_connect_cb)(evweb_request* request, evweb_response* response, bool* next);

void evweb_init_connect_iface(evweb_connect_iface* iface);
int evweb_connect_add_function(evweb_connect_iface* iface, evweb_connect_cb cb);
int evweb_connect_add_router(evweb_connect_iface* iface, enum http_method, char* resource, evweb_connect_cb cb);
int evweb_connect_add_static(evweb_connect_iface* iface, char* directory);
void evweb_start_connect_server(EV_P, int port, evweb_server_settings* settings, evweb_connect_iface* iface);
void evweb_destroy_connect_iface(evweb_connect_iface* iface);

#endif

