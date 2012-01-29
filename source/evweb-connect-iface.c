#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "evweb-connect-iface.h"

#ifndef DEBUG_CONNECT_IFACE
  #ifdef DEBUG
    #define DEBUG_CONNECT_IFACE 1
  #else
    #define DEBUG_CONNECT_IFACE 0
  #endif
#endif

#if DEBUG_CONNECT_IFACE
  #define print_debug(...) printf("[connect-iface] " __VA_ARGS__)
#else
  #define print_debug(...)
#endif
#define print_status(...) printf("[connect-iface] " __VA_ARGS__)
#define print_err(...) fprintf(stderr, "[connect-iface] " __VA_ARGS__)

#define EVWEB_CNCT_GENERAL  10
#define EVWEB_CNCT_ROUTER   11
#define EVWEB_CNCT_STATIC   12

struct priv_connect_cb {
  int cb_type;
  enum http_method method;
  char* resource;
  evweb_connect_cb* cb;
};

static evweb_connect_iface* static_iface;

static void request_handler(evweb_request* request, evweb_response* response);
static void serve_static_file(evweb_request* request, evweb_response* response, bool* next, char* directory);
static char* guess_content_type(char* extension);

void evweb_init_connect_iface(evweb_connect_iface* iface) {
  iface->cb_count = 0;
  iface->max_cb_count = 8;
  iface->cbs = calloc(iface->max_cb_count, sizeof (struct priv_connect_cb));
}

static struct priv_connect_cb* next_unused_cb(evweb_connect_iface* iface) {
  iface->cb_count += 1;
  if (iface->cb_count > iface->max_cb_count)
  {
    iface->max_cb_count *= 2;
    iface->cbs = realloc(iface->cbs, iface->max_cb_count * sizeof (struct priv_connect_cb));
    if (NULL == iface->cbs)
    {
      print_err("failed to allocate memory for the connect callbacks: %s\n", strerror(errno));
      return NULL;
    }
  }

  return ((struct priv_connect_cb*)iface->cbs) + (iface->cb_count - 1);
}

int evweb_connect_add_function(evweb_connect_iface* iface, evweb_connect_cb cb) {
  struct priv_connect_cb* new_cb;
  new_cb = next_unused_cb(iface);

  if (NULL == new_cb)
  {
    return errno;
  }

  new_cb->cb_type = EVWEB_CNCT_GENERAL;
  new_cb->cb = cb;

  return 0;
}

int evweb_connect_add_router(evweb_connect_iface* iface, enum http_method method, char* resource, evweb_connect_cb cb) {
  struct priv_connect_cb* new_cb;

  new_cb = next_unused_cb(iface);

  if (NULL == new_cb)
  {
    return errno;
  }

  new_cb->cb_type = EVWEB_CNCT_ROUTER;
  new_cb->method = method;
  new_cb->cb = cb;
  new_cb->resource = malloc(strlen(resource) + 1);
  if (NULL == new_cb->resource)
  {
    print_err("failed to allocate memory for the router resource: %s\n", strerror(errno));
    return errno;
  }
  strncpy(new_cb->resource, resource, strlen(resource) + 1);

  return 0;
}

int evweb_connect_add_static(evweb_connect_iface* iface, char* directory) {
  struct priv_connect_cb* new_cb;

  new_cb = next_unused_cb(iface);

  if (NULL == new_cb)
  {
    return errno;
  }

  new_cb->cb_type = EVWEB_CNCT_STATIC;
  new_cb->resource = malloc(strlen(directory) + 1);
  if (NULL == new_cb->resource)
  {
    print_err("failed to allocate memory for the static connect directory: %s\n", strerror(errno));
    return errno;
  }
  strncpy(new_cb->resource, directory, strlen(directory) + 1);

  return 0;
}

void evweb_destroy_connect_iface(evweb_connect_iface* iface) {
  int i;
  struct priv_connect_cb* cur_cb;

  cur_cb = (struct priv_connect_cb*)iface->cbs;
  for (i = 0; i < iface->cb_count; i += 1)
  {
    free(cur_cb->resource);
    cur_cb += 1;
  }

  iface->cb_count = 0;
  iface->max_cb_count = 0;
  free(iface->cbs);
  iface->cbs = NULL;
}

void evweb_start_connect_server(EV_P, int port, evweb_server_settings* settings, evweb_connect_iface* iface) {
  static_iface = iface;
  evweb_start_server(EV_A, port, settings, request_handler);
}

static void request_handler(evweb_request* request, evweb_response* response) {
  int i;
  bool next = true;
  struct priv_connect_cb* cur_cb;
  char*     path_start;
  u_int16_t path_length;

  if (!(request->parsed_url_info.field_set & 1 << UF_PATH)) {
    print_err("url parser didn't find path in %s, don't know what to do with request\n", request->url);
    return;
  }
  path_start  = request->url + request->parsed_url_info.field_data[UF_PATH].off;
  path_length = request->parsed_url_info.field_data[UF_PATH].len;

  print_debug("received a request for resource %.*s, running through %d callbacks\n", path_length, path_start, static_iface->cb_count);

  cur_cb = (struct priv_connect_cb*)static_iface->cbs;
  for(i = 0; i < static_iface->cb_count; i += 1)
  {
    if (EVWEB_CNCT_GENERAL == cur_cb->cb_type)
    {
      print_debug("callback %d is a general type\n", i);
      next = false;
      cur_cb->cb(request, response, &next);
    }
    if (EVWEB_CNCT_ROUTER == cur_cb->cb_type)
    {
      print_debug("callback %d is a router type\n", i);
      if (cur_cb->method != request->method)
      {
        print_debug("HTTP request method (%d) does not match this routers method (%d)\n", request->method, cur_cb->method);
      }
      else if (strlen(cur_cb->resource) == path_length && 0 == strncmp(path_start, cur_cb->resource, path_length))
      {
        next = false;
        print_debug("calling router callback for resource %s\n", cur_cb->resource);
        cur_cb->cb(request, response, &next);
      }
    }
    else if (EVWEB_CNCT_STATIC == cur_cb->cb_type)
    {
      print_debug("callback %d is a static type\n", i);
      if ( (HTTP_GET != request->method) && (HTTP_HEAD != request->method) )
      {
        print_debug("received HTTP request method (%d) incompatible with a static server (need %d or %d)\n", request->method, HTTP_GET, HTTP_HEAD);
      }
      else
      {
        next = false;
        print_debug("checking directory %s for resource %.*s\n", cur_cb->resource, path_length, path_start);
        serve_static_file(request, response, &next, cur_cb->resource);
        print_debug("next = %d following the serve static call\n", next);
      }
    }

    if (false == next)
    {
      print_debug("the request has been handle by one of the callbacks\n");
      break;
    }
    cur_cb += 1;
  }

  if (true == next)
  {
    print_debug("don't know what to do with resource %.*s, sending 404\n", path_length, path_start);
    set_response_status(response, 404, NULL);
    end_response(response);
  }
}

static void serve_static_file(evweb_request* request, evweb_response* response, bool* next, char* directory) {
  char full_path[256];
  struct stat sb;
  int fd;
  size_t file_size;
  char* file_buffer;
  int err_check;

  int i;
  char* extension;
  char* type;

  char*     path_start;
  u_int16_t path_length;

  if (!(request->parsed_url_info.field_set & 1 << UF_PATH)) {
    print_err("somehow got past previous error checks with invalid parsed url info\n");
    return;
  }
  path_start  = request->url + request->parsed_url_info.field_data[UF_PATH].off;
  path_length = request->parsed_url_info.field_data[UF_PATH].len;

  // the path should always start with a "/", but if that's all it is and the file index.html exists we should serve that
  if (1 == path_length)
  {
    snprintf(full_path, sizeof full_path, "%s/index.html", directory);
  }
  else
  {
    snprintf(full_path, sizeof full_path, "%s%.*s", directory, path_length, path_start);
  }
  print_debug("checking to see if %s exists\n", full_path);

  fd = open(full_path, O_RDONLY);
  if(-1 == fd)
  {
    print_debug("could not open a file descriptor for %s: %s\n", full_path, strerror(errno));
    *next = true;
    return;
  }

  set_response_status(response, 200, NULL);
  if (HTTP_HEAD == request->method)
  {
    send_response(response);
    return;
  }

  err_check = fstat(fd, &sb);
  if(-1 == err_check)
  {
    perror("failed to stat the requested file");
    exit(EXIT_FAILURE);
  }
  file_size = (size_t)sb.st_size;

  file_buffer = (char*)calloc(1, file_size + 100);
  err_check = (int)read(fd, file_buffer, file_size + 100); // try to read more just to see if it's there
  if (err_check != (int)file_size)
  {
    fprintf(stderr, "read %d bytes instead of the expected %zu for %s: %s\n", err_check, file_size, full_path, strerror(errno));
    set_response_status(response, 503, "File found, but buffer failed to load");
    free(file_buffer);
    close(fd);
    return;
  }

  i = strlen(full_path) - 1;
  for (; i > 0; i -= 1)
  {
    if ('.' == full_path[i])
    {
      extension = full_path + i + 1;
      break;
    }
  }
  if (0 == i)
  {
    extension = "octet-stream";
  }
  type = guess_content_type(extension);

  print_debug("ending response with a file of size %zu, and type %s\n", file_size, type);

  set_response_body(response, file_buffer, file_size, type);
  end_response(response);
  print_debug("served file %s (%zu bytes)\n", full_path, file_size);

  free(file_buffer);
  close(fd);
}

static char* guess_content_type(char* extension) {
  if (0 == strcmp(extension, "octet-stream")) { return "application/octet-stream"; }
  if (0 == strcmp(extension, "png"))  { return "image/png"; }
  if (0 == strcmp(extension, "jpeg")) { return "image/jpeg"; }
  if (0 == strcmp(extension, "json")) { return "application/json"; }
  if (0 == strcmp(extension, "html")) { return "text/html"; }
  if (0 == strcmp(extension, "xml"))  { return "text/xml"; }
  if (0 == strcmp(extension, "txt"))  { return "text/plain"; }
  else { return "application/octet-stream"; }
}
