#ifndef _SERVER_CENTER_H_
#define _SERVER_CENTER_H_

#include <ev.h>
#include <evn.h>
#include <bool.h>

#include "http_parser.h"

typedef struct evweb_header_line evweb_header_line;
typedef struct evweb_http_processer evweb_http_processer;
typedef struct evweb_request evweb_request;
typedef struct evweb_response evweb_response;
typedef struct evweb_server_settings evweb_server_settings;

struct evweb_header_line {
  char*  field;
  size_t field_len;
  char*  value;
  size_t value_len;
};

struct evweb_request {
  char*  url;
  size_t url_length;
  struct http_parser_url parsed_url_info;

  evweb_header_line* header_lines;
  int num_header_lines;
  int max_num_header_lines;
  bool last_was_value;
  enum http_method method;

  void* body;
  size_t body_length;
};

struct evweb_response {
  int status;
  char* status_message;

  evweb_header_line* header_lines;
  int num_header_lines;
  int max_num_header_lines;

  void* content;
  char* content_type;
  size_t content_length;

  struct evn_stream* connection;
};

struct evweb_http_processer {
  http_parser parser;
  evweb_request request;
  evweb_response response;
};

struct evweb_server_settings {
  int max_keep_alive;
};

typedef void (evweb_on_connection)(evweb_request* request, evweb_response* reponse);

// private
int interpret_header(evweb_http_processer* parser);
int finish_message(evweb_http_processer* parser);

// public
void evweb_start_server(EV_P, int port, evweb_server_settings* settings, evweb_on_connection callback);
void evweb_close_server();

int set_response_status(evweb_response* response, int status, char* message);
int add_response_header(evweb_response* response, char* field, char* value);
int clear_response_headers(evweb_response* response);

int set_response_body(evweb_response* response, void* body, size_t body_length, char* type);
int add_to_response_body(evweb_response* response, void* body, size_t body_length, char* type);
int clear_response_body(evweb_response* response);

bool send_response(evweb_response* response);
int end_response(evweb_response* response);

char* query_to_json(char* query_in, char* json_buffer, size_t json_size);

#endif

