#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <evn.h>

#include "http_parser.h"
#include "evweb.h"

#ifndef DEBUG_HTTP_PARSER_CBS
  #ifdef DEBUG
    #define DEBUG_HTTP_PARSER_CBS 1
  #else
    #define DEBUG_HTTP_PARSER_CBS 0
  #endif
#endif

#if DEBUG_HTTP_PARSER_CBS
  #define print_debug(...) printf("[http-parser-cbs] " __VA_ARGS__)
#else
  #define print_debug(...)
#endif
#define print_status(...) printf("[http-parser-cbs] " __VA_ARGS__)
#define print_err(...) fprintf(stderr, "[http-parser-cbs] " __VA_ARGS__)

static http_parser_settings parser_settings;
static bool cbs_initialized = false;

static int on_message_begin(http_parser* parser);
static int on_url(http_parser* parser, const char* at, size_t length);
static int on_header_field(http_parser* parser, const char* at, size_t length);
static int on_header_value(http_parser* parser, const char* at, size_t length);
static int on_headers_complete(http_parser* parser);
static int on_body(http_parser* parser, const char* at, size_t length);
static int on_message_complete(http_parser* parser);

http_parser_settings* get_http_parser_settings() {
  if (true != cbs_initialized)
  {
    parser_settings.on_message_begin = on_message_begin;
    parser_settings.on_url = on_url;
    parser_settings.on_header_field = on_header_field;
    parser_settings.on_header_value = on_header_value;
    parser_settings.on_headers_complete = on_headers_complete;
    parser_settings.on_body = on_body;
    parser_settings.on_message_complete = on_message_complete;
    cbs_initialized = true;
  }

  return &parser_settings;
}

static int on_message_begin(http_parser* parser) {
  int i;
  evweb_header_line* current_line;
  evweb_http_processer* processer = (evweb_http_processer*)parser;
  print_debug("message begun\n");

  // set last_was_value true so we will properly read in the first header
  processer->request.last_was_value = true;

  // these will be calloced, so it is safe to free pointer we didn't set (they will be NULL)
  // we still want to reinitialize them incase we get another message on the same connection
  current_line = processer->request.header_lines;
  for (i = 0; i < processer->request.num_header_lines; i += 1)
  {
    free(current_line->field);
    free(current_line->value);
    current_line += 1;
  }
  free(processer->request.header_lines);

  processer->request.num_header_lines = 0;
  processer->request.max_num_header_lines = 10;
  processer->request.header_lines = calloc(processer->request.max_num_header_lines, sizeof(evweb_header_line));

  free(processer->request.url);
  processer->request.url = NULL;
  processer->request.url_length = 0;

  free(processer->request.body);
  processer->request.body = NULL;
  processer->request.body_length = 0;

  // Now initialize the response and it's headers
  processer->response.status = -1;
  free(processer->response.status_message);
  processer->response.status_message = NULL;

  current_line = processer->response.header_lines;
  for (i = 0; i < processer->response.num_header_lines; i += 1)
  {
    free(current_line->field);
    free(current_line->value);
    current_line += 1;
  }
  free(processer->response.header_lines);
  processer->response.num_header_lines = 0;
  processer->response.max_num_header_lines = 5;
  processer->response.header_lines = calloc(processer->response.max_num_header_lines, sizeof(evweb_header_line));

  free(processer->response.content);
  processer->response.content = NULL;
  free(processer->response.content_type);
  processer->response.content_type = NULL;
  processer->response.content_length = 0;
  processer->response.connection = (struct evn_stream*)parser->data;

  print_debug("request struct initialized\n");

  return 0;
}

static int on_url(http_parser* parser, const char* at, size_t length) {
  evweb_http_processer* processer = (evweb_http_processer*)parser;
  size_t current_len = processer->request.url_length;

  print_debug("url received: %.*s\n", (int)length, at);

  processer->request.url_length += length;
  processer->request.url = realloc(processer->request.url, processer->request.url_length + 1);
  if (NULL == processer->request.url)
  {
    print_err("failed to realloc memory for the url: %s\n", strerror(errno));
    return 1;
  }
  memcpy(processer->request.url + current_len, at, length);
  processer->request.url[processer->request.url_length] = '\0';

  return http_parser_parse_url(processer->request.url, processer->request.url_length, 0, &(processer->request.parsed_url_info));
}

static int on_header_field(http_parser* parser, const char* at, size_t length) {
  evweb_header_line* current_line;
  size_t current_length;
  evweb_request* request = &(((evweb_http_processer*)parser)->request);

  print_debug("header field received: %.*s\n", (int)length, at);

  current_line = request->header_lines + request->num_header_lines - 1;
  if (true == request->last_was_value)
  {
    request->num_header_lines += 1;
    if (request->num_header_lines > request->max_num_header_lines)
    {
      request->max_num_header_lines *= 2;
      print_debug("expanding the number of headers we can store to %d\n", request->max_num_header_lines);
      request->header_lines = realloc(request->header_lines, request->max_num_header_lines * sizeof (evweb_header_line));
      if (NULL == request->header_lines)
      {
        print_err("failed to allocate memory to expand the number of headers we can store: %s\n", strerror(errno));
        return 1;
      }
    }
    current_line = request->header_lines + request->num_header_lines - 1;
    memset(current_line, 0, sizeof (evweb_header_line));
  }
  else
  {
    print_debug("adding %zu to the previous header field of length %zu\n", length, current_line->field_len);
    print_debug("previous (incomplete) header field = %s\n", current_line->field);
  }

  current_length = current_line->field_len;
  current_line->field_len += length;
  current_line->field = realloc(current_line->field, current_line->field_len+1);
  memcpy(current_line->field + current_length, at, length);
  current_line->field[current_line->field_len] = '\0';

  request->last_was_value = false;
  return 0;
}

static int on_header_value(http_parser* parser, const char* at, size_t length) {
  evweb_header_line* current_line;
  size_t current_length;
  evweb_http_processer* processer = (evweb_http_processer*)parser;

  print_debug("header value received: %.*s\n", (int)length, at);

  current_line = processer->request.header_lines + processer->request.num_header_lines - 1;

  if (true == processer->request.last_was_value)
  {
    print_debug("adding %zu to the header value buffer currently size %zu\n", length, current_line->value_len);
    print_debug("previous (incomplete) header value = %s\n", current_line->value);
  }

  current_length = current_line->value_len;
  current_line->value_len += length;
  current_line->value = realloc(current_line->value, current_line->value_len+1);
  memcpy(current_line->value + current_length, at, length);
  current_line->value[current_line->value_len] = '\0';

  processer->request.last_was_value = true;
  return 0;
}

static int on_headers_complete(http_parser* parser) {
  print_debug("headers complete\n");
  return interpret_header((evweb_http_processer*)parser);
}

static int on_body(http_parser* parser, const char* at, size_t length) {
  size_t current_length;
  evweb_request* request = &(((evweb_http_processer*)parser)->request);

  print_debug("body (fragment?) received (%zu bytes)\n", length);

  current_length = request->body_length;
  request->body_length += length;
  request->body = realloc(request->body, request->body_length + 1);
  if (NULL == request->body)
  {
    print_err("failed to realloc memory for the body: %s\n", strerror(errno));
    return 1;
  }
  memcpy(request->body + current_length, at, length);
  ((char*)request->body)[request->body_length] = '\0';

  return 0;
}

static int on_message_complete(http_parser* parser) {
  print_debug("message complete\n");
  return finish_message((evweb_http_processer*)parser);
}

