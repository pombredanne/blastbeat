#include "../blastbeat.h"

extern struct blastbeat_server blastbeat;
http_parser_settings bb_http_parser_settings;

int bb_http_func(struct bb_connection *bbc, char *buf, size_t len) {
	// in HTTP connections, only one session is allowed
	if (!bbc->sessions_head) {
		bbc->sessions_head = bb_session_new(bbc);
	}
	struct bb_session *bbs = bbc->sessions_head;
	if (!bbs) return -1;

	if (!bbs->request.initialized)
		bb_initialize_request(bbs);

	int res = http_parser_execute(&bbs->request.parser, &bb_http_parser_settings, buf, len);
	if (res != len) return -1;
	return 0;
}

int bb_http_send_headers(struct bb_session *bbs, char *buf, size_t len) {
	return bb_wq_push_copy(bbs, buf, len, BB_WQ_FREE);
}

int bb_http_send_end(struct bb_session *bbs) {
	return bb_wq_push_close(bbs);
}

int bb_http_send_body(struct bb_session *bbs, char *buf, size_t len) {
	if (bb_wq_push_copy(bbs, buf, len, BB_WQ_FREE))
		return -1;
	bbs->response.written_bytes += len;
	if (bbs->response.content_length != ULLONG_MAX && bbs->response.content_length > 0 &&
		bbs->response.written_bytes >= bbs->response.content_length &&
		bbs->response.close)
        	return bb_wq_push_close(bbs);
	return 0;
}

int bb_http_cache_send_headers(struct bb_session *bbs, struct bb_cache_item *bbci) {
	char *first_line = bb_alloc(bbci->http_first_line_len+2);
	if (!first_line) {
		return -1;
	}
	memcpy(first_line, bbci->http_first_line, bbci->http_first_line_len);
	first_line[bbci->http_first_line_len] = '\r';
	first_line[bbci->http_first_line_len+1] = '\n';

	if (bb_wq_push(bbs, first_line, bbci->http_first_line_len+2, BB_WQ_FREE))
                return -1;

	size_t i;
	for(i=0;i<bbci->headers_count;i++) {
		char *header = bb_alloc(bbci->headers[i].keylen+2+bbci->headers[i].vallen+2);
		if (!header) {
			return -1;
		}
		memcpy(header, bbci->headers[i].key, bbci->headers[i].keylen);
		header[bbci->headers[i].keylen] = ':';
		header[bbci->headers[i].keylen+1] = ' ';
		memcpy(header+bbci->headers[i].keylen+2, bbci->headers[i].value, bbci->headers[i].vallen);
		header[bbci->headers[i].keylen+2+bbci->headers[i].vallen] = '\r';
		header[bbci->headers[i].keylen+2+bbci->headers[i].vallen+1] = '\n';
		if (bb_wq_push(bbs, header, bbci->headers[i].keylen+2+bbci->headers[i].vallen+2, BB_WQ_FREE))
                	return -1;
	}

	if (bb_wq_push(bbs, "\r\n", 2, 0))
                return -1;

	return 0;
}

int bb_http_cache_send_body(struct bb_session *bbs, struct bb_cache_item *bbci) {
	if (bb_wq_push_copy(bbs, bbci->body, bbci->body_len, BB_WQ_FREE))
                return -1;
	// close the connection if the cached response has no valid EOS
	if (!bbci->valid) {
        	return bb_wq_push_close(bbs);
	}
	return 0;
}

ssize_t bb_http_read(struct bb_connection *bbc, char *buf, size_t len) {
        return read(bbc->fd, buf, len);
}

ssize_t bb_http_write(struct bb_connection *bbc, char *buf, size_t len) {
        return write(bbc->fd, buf, len);
}


struct bb_http_header *bb_http_req_header(struct bb_session *bbs, char *key, size_t keylen) {
        off_t i;
        for(i=0;i<bbs->request.headers_count;i++) {
                if (!bb_stricmp(key, keylen, bbs->request.headers[i].key, bbs->request.headers[i].keylen)) {
                        return &bbs->request.headers[i];
                }
        }

        return NULL;
}


int bb_manage_chunk(struct bb_session *bbs, char *buf, size_t len) {

	// fallback to normal body if chunked encoding is not allowed

	if (!bbs->request.can_chunk) {
		if (bb_wq_push_copy(bbs, buf, len, BB_WQ_FREE)) return -1;
		return 0;
	}

	char *chunk = bb_alloc(MAX_CHUNK_STORAGE);
        if (!chunk) {
		return -1;
        }
        int chunk_len = snprintf(chunk, MAX_CHUNK_STORAGE, "%X\r\n", (unsigned int) len);

        if (bb_wq_push_copy(bbs, chunk, chunk_len, BB_WQ_FREE)) {
		bb_free(chunk, MAX_CHUNK_STORAGE);
		return -1;
	}
	bb_free(chunk, MAX_CHUNK_STORAGE);
        if (bb_wq_push_copy(bbs, buf, len, BB_WQ_FREE)) return -1;
        if (bb_wq_push(bbs, "\r\n", 2, 0)) return -1;
        if (len == 0 && bbs->response.close) {
        	if (bb_wq_push_close(bbs)) return -1;
	}

	return 0;
}

static int url_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session *bbs = (struct bb_session *) parser->data;
        if (!bbs->request.url) {
                bbs->request.url = bb_alloc(len);
		if (!bbs->request.url) {
			return -1;
		}
                memcpy(bbs->request.url, buf, len);
                bbs->request.url_len = len;
        }
        else {
                bbs->request.url = bb_realloc(bbs->request.url, bbs->request.url_len, len);
                memcpy(bbs->request.url + bbs->request.url_len, buf, len);
                bbs->request.url_len += len;
        }
        return 0;
}


int null_cb(http_parser *parser) {
        return 0;
}

int null_b_cb(http_parser *parser, const char *buf, size_t len) {
        return 0;
}

static int header_field_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session *bbs = (struct bb_session *) parser->data;
	struct bb_request *bbr = &bbs->request;

        if (bbr->last_was_value) {
		if (bbr->headers_count + 1 > blastbeat.max_headers) return -1;
		if (bbr->headers_len + 2 + len > MAX_HEADERS_SIZE) return -1;	
                struct bb_http_header *bbhh = bb_realloc(bbr->headers, sizeof(struct bb_http_header)*bbr->headers_count, sizeof(struct bb_http_header));
                if (!bbhh) {
                        return -1;
                }
                bbr->headers = bbhh;
                int pos = bbr->headers_count;
                bbr->headers_count++;

		bbr->headers[pos].value = NULL;
		bbr->headers[pos].vallen = 0;
		bbr->headers[pos].keylen = 0;

                bbr->headers[pos].key = bb_alloc(len);
                if (!bbr->headers[pos].key) {
                        return -1;
                }
                memcpy(bbr->headers[pos].key, buf, len);
                bbr->headers[pos].keylen = len;
		bbr->headers_len += 2+len;
        }
        else {
		if (bbr->headers_len + len > MAX_HEADERS_SIZE) return -1;
                int pos = bbr->headers_count-1;
                char *tmp_buf = bb_realloc(bbr->headers[pos].key, bbr->headers[pos].keylen, len);
                if (!tmp_buf) {
                        return -1;
                }
                bbr->headers[pos].key = tmp_buf;
                memcpy(bbr->headers[pos].key + bbr->headers[pos].keylen, buf, len);
                bbr->headers[pos].keylen += len;
		bbr->headers_len += len;
        }
        bbr->last_was_value = 0;
        return 0;
}

static int header_value_cb(http_parser *parser, const char *buf, size_t len) {
	struct bb_session *bbs = (struct bb_session *) parser->data;
        struct bb_request *bbr = &bbs->request;

        int pos = bbr->headers_count-1;
        if (!bbr->last_was_value) {
		if (bbr->headers_len + 2 + len > MAX_HEADERS_SIZE) return -1;
                bbr->headers[pos].value = bb_alloc(len);
                if (!bbr->headers[pos].value) {
                        return -1;
                }
                memcpy(bbr->headers[pos].value, buf, len);
                bbr->headers[pos].vallen = len;
                bbr->headers_len+= 2+len;
        }
        else {
		if (bbr->headers_len + len > MAX_HEADERS_SIZE) return -1;
                char *tmp_buf = bb_realloc(bbr->headers[pos].value, bbr->headers[pos].vallen, len);
                if (!tmp_buf) {
                        return -1;
                }
                bbr->headers[pos].value = tmp_buf;
                memcpy(bbr->headers[pos].value + bbr->headers[pos].vallen, buf, len);
                bbr->headers[pos].vallen += len;
                bbr->headers_len+=len;
        }
        bbr->last_was_value = 1;
        return 0;
}


static int header_ptr_field_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session *bbs = (struct bb_session *) parser->data;
        if (bbs->response.last_was_value) {
		if (bbs->response.headers_count + 1 > blastbeat.max_headers) return -1;
		struct bb_http_header *bbhh = bb_realloc(bbs->response.headers, sizeof(struct bb_http_header)*bbs->response.headers_count, sizeof(struct bb_http_header));
                if (!bbhh) {
                        return -1;
                }
                bbs->response.headers = bbhh;
                bbs->response.headers[bbs->response.headers_count].key = (char *) buf;
                bbs->response.headers[bbs->response.headers_count].keylen = len;
		bbs->response.headers[bbs->response.headers_count].value = NULL;
		bbs->response.headers[bbs->response.headers_count].vallen = 0;
                bbs->response.headers_count++;
        }
        else {
                bbs->response.headers[bbs->response.headers_count-1].keylen += len;
        }
        bbs->response.last_was_value = 0;
        return 0;
}

static int header_ptr_value_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session *bbs = (struct bb_session *) parser->data;
        if (!bbs->response.last_was_value) {
                bbs->response.headers[bbs->response.headers_count-1].value = (char *) buf;
                bbs->response.headers[bbs->response.headers_count-1].vallen = len;
        }
        else {
                bbs->response.headers[bbs->response.headers_count-1].vallen += len;
        }
        bbs->response.last_was_value = 1;
        return 0;
}

int bb_http_recv_body(struct bb_session *bbs, char *buf, size_t len) {
	bb_zmq_send_msg(bbs->dealer, bbs, (char *) &bbs->uuid_part1, BB_UUID_LEN, "body", 4, (char *) buf, len);
	return 0;
}

static int body_cb(http_parser *parser, const char *buf, size_t len) {
        struct bb_session *bbs = (struct bb_session *) parser->data;
	return bbs->recv_body(bbs, (char *) buf, len);
}

static int response_headers_complete(http_parser *parser) {
        struct bb_session *bbs = (struct bb_session *) parser->data;
	if (parser->content_length != ULLONG_MAX) {
                bbs->response.content_length = parser->content_length;
        }
        if (!http_should_keep_alive(parser)) {
                bbs->response.close = 1;
        }
        return 0;
}

static int bb_session_headers_complete(http_parser *parser) {
        struct bb_session *bbs = (struct bb_session *) parser->data;

        // ok get the Host header
        struct bb_http_header *bbhh = bb_http_req_header(bbs, "Host", 4);
        if (!bbhh) {
                return -1;
        }

        if (!bbs->dealer) {
                if (bb_set_dealer(bbs, bbhh->value, bbhh->vallen)) {
                	return -1;
        	}
        }

	if (parser->http_major == 1 && parser->http_minor == 1) {
		bbs->request.can_chunk = 1;
	}

	// check for mountpoint...
	// check for socket.io
	if (!bb_startswith(bbs->request.url, bbs->request.url_len, "/socket.io/1/", 13)) {
		char *method = (char *) http_method_str(bbs->request.parser.method);
		if (bb_manage_socketio(bbs,  method, strlen(method), bbs->request.url, bbs->request.url_len)) {
			return -1;
		}
		goto msg;
	}

        if (parser->upgrade) {
                struct bb_http_header *bbhh = bb_http_req_header(bbs, "Upgrade", 7);
                if (bbhh) {
                        if (!bb_stricmp("websocket", 9, bbhh->value, bbhh->vallen)) {
				bbs->connection->func = bb_websocket_func;
                                bb_send_websocket_handshake(bbs);
				goto msg;
                        }
                }
        }


        if (!http_should_keep_alive(parser)) {
                //printf("NO KEEP ALIVE !!!\n");
                bbs->response.close = 1;
        }

	// ok now check if the virtualhost as a cache store associated
	if (bbs->vhost->cache_size > 0 && bbs->request.parser.method == HTTP_GET) {
		int ret = bb_manage_cache(bbs, bbs->request.url, bbs->request.url_len);
		if (ret == BLASTBEAT_CACHE_MISS) goto msg;
		if (ret == BLASTBEAT_CACHE_FOUND) return 0;
		if (ret == BLASTBEAT_CACHE_ERROR) return -1;
	}

msg:
	if (bbs->request.no_uwsgi) return 0;
        // now encode headers in a uwsgi packet and send it as "headers" message
	if (bb_uwsgi(bbs)) {
		return -1;
	}
        bb_zmq_send_msg(bbs->dealer, bbs, (char *) &bbs->uuid_part1, BB_UUID_LEN, "uwsgi", 5, bbs->request.uwsgi_buf, bbs->request.uwsgi_pos);
        return 0;
}

static int bb_session_request_complete(http_parser *parser) {
        if (parser->upgrade) return 0;
	struct bb_session *bbs = (struct bb_session *) parser->data;
	if (bbs->recv_complete) {
		if (bbs->recv_complete(bbs))
			return -1;
	}
        if (http_should_keep_alive(parser)) {
                // prepare for a new request
		bb_initialize_request(bbs);
        }
        return 0;
}


http_parser_settings bb_http_parser_settings = {
        .on_message_begin = null_cb,
        .on_message_complete = bb_session_request_complete,
        .on_headers_complete = bb_session_headers_complete,
        .on_header_field = header_field_cb,
        .on_header_value = header_value_cb,
        .on_url = url_cb,
        .on_body = body_cb,
};

http_parser_settings bb_http_response_parser_settings = {
        .on_message_begin = null_cb,
        .on_message_complete = null_cb,
        .on_headers_complete = response_headers_complete,
        .on_header_field = header_ptr_field_cb,
        .on_header_value = header_ptr_value_cb,
        .on_url = null_b_cb,
        .on_body = null_b_cb,
};
