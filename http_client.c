/*
	Basic http client.
	Copyright (C) 2013-2015 Edward Chernenko.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
*/

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

const unsigned request_timeout = 60; // in seconds
const unsigned max_redirects = 7;
const char *appname = "http_client";
const char *appversion = "0.1";
#define MAX_HTTP_HEADERS_LENGTH 4096 // maximum sum length of all HTTP headers
#define MAX_HTTP_HEADERS_COUNT 100

struct http_header {
	char *key;
	char *val;

	int val_must_be_freed; // 0 if 'val' points to a static buffer, 1 if it was malloc()-ed
};
int redirect_nr = 0;

void free_headers(struct http_header *HEADERS, int HEADERS_count)
{
	int i;
	for(i = 0; i < HEADERS_count; i ++)
	{
		if(HEADERS[i].val_must_be_freed)
			free(HEADERS[i].val);
	}
}

int compare_headers_cb(const void *a, const void *b)
{
	return strcmp(
		((struct http_header *) a)->key,
		((struct http_header *) b)->key
	);
}


char *find_header(
	struct http_header *HEADERS,
	int HEADERS_count,

	// "header_name_normalized" must be completely in lowercase
	char *header_name_normalized)
{
	struct http_header query;
	query.key = header_name_normalized;

	struct http_header *h = (struct http_header *) bsearch(&query, HEADERS, HEADERS_count, sizeof(HEADERS[0]), compare_headers_cb);
	return h ? h->val : NULL;
}
void print_usage()
{
	fprintf(stderr, "Usage: %s URL\n", appname);
	exit(1);
}
void timeout_handler(int unused)
{
	fprintf(stderr, "[error] Timeout\n");
	exit(1);
}

/* separate_CRLF
	- looks at "string" and detects "\n", "\r" or
		their combination ("\r\n" or "\n\r")
	- replaces first of those detected symbols with \0
	- returns the pointer to the first byte after CRLF
	- returns NULL if neither \r nor \n were found
*/
char *separate_CRLF(char *string)
{
	char *p1, *p2;
	p1 = strchr(string, '\n');
	p2 = strchr(string, '\r');

	if(!p1 && !p2) return NULL;

	/* Switch "p1" and "p2", so that "p1" would be closer to the beginning
		of the string (and so that "p1" wouldn't be NULL).
		"p2" can be NULL */
	if(!p1)
	{
		p1 = p2;
		p2 = NULL;
	}
	else if(p2 && p1 > p2)
	{
		char *t = p1;
		p1 = p2;
		p2 = t;
	}

	*p1 = '\0'; // mark the end of the string

	p1 ++;
	if(p1 == p2) // if \r is followed by \n (or vice verse), then ignore the second symbol
		p1 ++;

	return p1;
}

/* If "string" starts with CRLF, returns a pointer to whatever is after CRLF.
	Otherwise returns "string".
	No more than one CRLF is skipped (because CRLF can be followed
	by binary data).
*/

char *strip_first_CRLF(char *string)
{
	if(*string == '\r')
	{
		string ++;
		if(*string == '\n') string ++;
	}
	else if(*string == '\n')
	{
		string ++;
		if(*string == '\r') string ++;
	}
	return string;
}

/*
	Helper method to read the response body.
	Unlike in the usual sendfile(), "in_fd" here can be a socket.

	Return the number of NOT YET READ bytes (i.e. 0 if "count" bytes
	have been read completely).
*/
size_t sendfile_from_socket(int out_fd, int in_fd, size_t count)
{
#define READ_BUFFER_SIZE 4096
	char buffer[READ_BUFFER_SIZE];

	while(count)
	{
		size_t todo;
		ssize_t bytes, written;

		todo = count > READ_BUFFER_SIZE ? READ_BUFFER_SIZE : count;
		bytes = read(in_fd, buffer, todo);

		if(bytes < 0)
		{
			fprintf(stderr, "read() failed: %s\n", strerror(errno));
			exit(1);
		}

		written = write(out_fd, buffer, bytes);
		if(written < bytes)
		{
			fprintf(stderr, "write() failed: %s\n", strerror(errno));
			exit(1);
		}

		if(bytes == 0)
			break;

		count -= bytes;
	}
	return count;
}

void perform_http_request(char *URL)
{
	struct http_header HEADERS[MAX_HTTP_HEADERS_COUNT];
	int HEADERS_count = 0;

	memset(&HEADERS, 0, sizeof(HEADERS));

	int i, j; // temporary variables for loops
	char *p; // temporary pointer used when parsing URLs
	char *begin = URL;

	p = strchr(begin, ':');
	if(!p)
	{
		fprintf(stderr, "[warn] No schema in URL, assuming HTTP.\n");
		p = begin;
	}
	else
	{
		*p = '\0'; p ++;
		if(strncmp(begin, "http", 4))
		{
bad_schema:
			fprintf(stderr, "[error] Unsupported schema: '%s' in URL.\n", begin);
			exit(EINVAL);
		}
		else if(begin[4] != '\0')
		{
			if(begin[4] == 's' && begin[5] == '\0')
			{
				fprintf(stderr, "[error] HTTPS is not yet implemented.\n");
				exit(ENOSYS);
			}
			else goto bad_schema;
		}

		if(!strcmp(p, "//"))
		{
			fprintf(stderr, "[error] Malformed URL (no http://).\n");
			exit(EINVAL);
		}
		p += 2;
	}


	begin = p; // "being" points to the beginning of "example.com/some/path"
	p = strchr(begin, '/'); // separate the host
	*p = '\0';

	char *host = begin; // points to "example.com" or "example.com:1234"
	char *path = p + 1; // points to "/some/path"

	// Convert the port from string to integer
	p = strchr(host, ':');
	const char *port;
	if(p)
	{
		port = p + 1;
		*p = '\0'; // port is separated from the "host" string
	}
	else port = "80";

	/* Convert "host" into IP address (unless it is already an address) */

	struct addrinfo hints;
	struct addrinfo *ai;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC; /* Both IPv4 and IPv6 are acceptable */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	int ret = getaddrinfo(host, port, &hints, &ai);
	if(ret != 0)
	{
		fprintf(stderr, "[error] Bad hostname or address: \"%s\": %s\n", host, gai_strerror(ret));
		exit(1);
	}

	// "ai" is a linked list, but we don't need all results,
	// we can just use the first
	int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if(sock < 0)
	{
		fprintf(stderr, "[error] socket() failed: %s\n", strerror(errno));
		exit(1);
	}

	/* Timeout control */
	signal(SIGALRM, timeout_handler);
	alarm(request_timeout);

	/* Debug code: how much time does each step take? */
	struct timeval start;
	gettimeofday(&start, NULL);

#define SPENT() ({ \
	struct timeval now; \
	gettimeofday(&now, NULL); \
	fprintf(stderr, "[info] Started %.4f seconds ago...\n", \
		now.tv_sec - start.tv_sec + 0.000001 * (now.tv_usec - start.tv_usec) ); \
})

	fprintf(stderr, "[info] Connecting to %s:%s...\n", host, port);
	if(connect(sock, ai->ai_addr, ai->ai_addrlen) < 0)
	{
		fprintf(stderr, "[error] connect(%s:%s) failed: %s\n", host, port, strerror(errno));
 		exit(1);
	}
	SPENT();
	freeaddrinfo(ai);

	fprintf(stderr, "[info] Connected to %s:%s OK\n", host, port);

	char *request;
	int request_length = asprintf(&request,
		"GET /%s HTTP/1.1\n"
		"Host: %s\n"
		"Connection: close\n"
		"User-Agent: %s/%s\n"
		"\n", path, host, appname, appversion);
	if(request_length < 0)
	{
		fprintf(stderr, "[error] asprintf: memory allocation failed\n");
		exit(1);
	}

	fprintf(stderr, "[info] Sending request to server...\n");

	write(sock, request, request_length);
	free(request);

	fprintf(stderr, "[info] Request sent OK.\n");
	SPENT();

	/* Read the reply. Here we must put the socket into non-blocking mode,
		because when we're reading headers, we can try to read more
		than exists in the response, if the response is small enough
		(less than MAX_HTTP_HEADERS_LENGTH). Which would cause timeout.
	*/
	if(fcntl(sock, F_SETFL, O_NONBLOCK) < 0)
	{
		fprintf(stderr, "[error] fcntl() failed: %s\n", strerror(errno));
		exit(1);
	}

	// We'll read into buffer[] until the first newline,
	// then parse with simple strchr()-s
	char buffer[MAX_HTTP_HEADERS_LENGTH + 1];
	int lineno = 0;

	// index of HTTP header we're currently reading in the HEADERS[] array
	int header_idx = 0;

	struct pollfd fds;
	fds.fd = sock;
	fds.events = POLLIN;

	char *line = buffer; // Start of the current string (inside "buffer").

	// Pointer to the byte in "buffer", into which we're going to read with
	// the next read() call. Changes with each read().
	char *buffer_offset = buffer;

	unsigned short code; // HTTP response code

	// Number of bytes of HTTP response body which we read prematurely
	// (while reading HTTP headers). "line" pointer will point
	// to the first byte of this data.
	int prefetched_body_length;
	char *p1;

	while(1)
	{
		if(poll(&fds, 1, -1) < 0) // no timeout needed, we did alarm()
		{
			fprintf(stderr, "[error] poll() failed: %s\n", strerror(errno));
			exit(1);
		}

		int space_left_in_buffer = MAX_HTTP_HEADERS_LENGTH - (buffer_offset - buffer);
		ssize_t bytes_received = read(sock, buffer_offset, space_left_in_buffer);
		if(bytes_received < 0)
		{
			fprintf(stderr, "[error] read(sock) failed: %s\n", strerror(errno));
			exit(1);
		}
		if(bytes_received == 0) continue;


#if 0 /* Debugging code. Here you can replace HTTP response with any text
	and check how it is parsed */

#warning "Debug code: real response substituted"
		bytes_received = snprintf(buffer, MAX_HTTP_HEADERS_LENGTH, "HTTP/1.1 200 OK\nContent-Type: text/html; charset=UTF-8\nSome-Header: text123\n appendix to text123\nContent-Length: 112\nTransfer-Encoding: chunked\nTransfer-Encoding: something-else\nTransfer-Encoding: yet-another-line\nX-another: first line\nX-another: and yet another line\n\nINTERESTING CONTENT");

		/* WARNING: debug */
#endif

		char *new_offset = buffer_offset + bytes_received;
		*new_offset = '\0';

		// 1) There're no newlines in whatever was in buffer[] before
		// read() call, so we search from buffer_offset.
		// 2) Line break can be \r, \n or their combination, so we
		// check everything with separate_CRLF().

check_buffer_for_newlines:
		p1 = separate_CRLF(buffer_offset); /* points to the next line */
		if(!p1) // no line breaks found
		{
			if(bytes_received == space_left_in_buffer)
			{
				// Server is doing something wrong:
				// it sent a lot of stuff already,
				// but HTTP headers still haven't ended yet

				fprintf(stderr, "[error] HTTP response headers returned by server are too long (> %i bytes). Aborting (just in case).\n", MAX_HTTP_HEADERS_LENGTH);
				exit(1);
			}

			// return to poll(), server should sent the remainder of this string
			buffer_offset = new_offset;
			continue;
		}

		/* Handle the line */
		fprintf(stderr, "[debug] Received line: \"%s\"\n", line);
		if(lineno == 0)
		{
			char proto[10];
			char status[256];

			sscanf(line, "%9s %3hu %255[^\n]", proto, &code, status);

			/* Catch the "wrong" status codes */
			if(code >= 400)
			{
				fprintf(stderr, "[error] Server returned HTTP error %i: %s\n", code, status);
				exit(1);
			}
			if(code < 100)
			{
				fprintf(stderr, "[error] Server has returned code %i. What?\n", code);
				exit(1);
			}
			if(code < 200)
			{
				fprintf(stderr, "[error] Server has returned code %i, which is quite strange (we didn't send the Upgrade header and our HTTP request had no body). Anyway, responses with 1xx codes can't have content. There is nothing to save. Exiting.\n", code);
				exit(1);
			}

			if(code == 204)
			{
				fprintf(stderr, "[notice] Server has returned 204 No Content. There is nothing to save. Exiting.\n");
				exit(0);
			}

		}
		else
		{
			if(line[0] == '\0')
			{
				// Start of the response body.
				// NOTE: part of the body has already been read
				// and is saved at (line+1).
				fprintf(stderr, "[info] All HTTP response headers have been received\n");
				line ++;

				/* Strip one newline (no more than one, as response body can have binary data) */
				line = strip_first_CRLF(line);

				prefetched_body_length = new_offset - line;
				goto read_response_body;
			}

			// If the string starts with space or tabulation, then
			// it is a continuation of the previous HTTP header.
			if(line[0] == ' ' || line[0] == '\t')
			{
				do
				{
					line ++; // Skip all spaces/tabs
				}
				while(*line == ' ' || *line == '\t');
				line --; // Leave one space

				if(!HEADERS[header_idx].val)
				{
					fprintf(stderr, "[error] Server has sent a malformed _first_ HTTP header (starts with space or tabulation)\n");
					exit(1);
				}

				fprintf(stderr, "[debug] Appending \"%s\" to \"%s\" in \"%s\" header.\n", line, HEADERS[header_idx].val, HEADERS[header_idx].key);

				int oldlen = strlen(HEADERS[header_idx].val);
				int applen = strlen(line);

				//fprintf(stderr, "[debug] Appending \"%p\" to \"%p\" (diff %i) (len %i) header.\n", line, HEADERS[header_idx].val, line - HEADERS[header_idx].val, len);

				// We do have enough space for memmove(): we're copying
				// to the area which contained the very same text (plus newlines).
				memmove(HEADERS[header_idx].val + oldlen, line, applen);
				HEADERS[header_idx].val[oldlen + applen] = '\0';
			}
			else // New HTTP header found
			{
				char *val = strchr(line, ':');
				if(!val)
				{
					fprintf(stderr, "[error] Server has sent a malformed HTTP header (no colon).\n");
					exit(1);
				}

				*val = '\0';
				val ++;

				// Remove the spaces after ':'
				while(isspace(*val))
					val ++;

				if(HEADERS[header_idx].val)
					header_idx ++;

				fprintf(stderr, "[debug] Found header '%s': '%s' -> goes into HEADERS[%i]\n", line, val, header_idx);
				HEADERS[header_idx].key = line;
				HEADERS[header_idx].val = val;

				// Normalize header names (they are case-insensitive)
				char *ptr;
				for(ptr = HEADERS[header_idx].key; *ptr != '\0'; ptr ++)
					*ptr = tolower(*ptr);
			}
		}

		line = p1;
		buffer_offset = p1;
		lineno ++;

		// We need to check if the last read() got more than one line.
		goto check_buffer_for_newlines;
	}

read_response_body:
	/* Let's study the HTTP response headers */
	HEADERS_count = header_idx;
	if(HEADERS[header_idx].val)
		HEADERS_count ++;

	SPENT();

	/* Sort the HEADERS[] array for faster search */
	qsort(HEADERS, HEADERS_count, sizeof(HEADERS[0]), compare_headers_cb);

	/* Merge duplicate headers (their values are joined using commas) */
	for(i = 0; i < HEADERS_count - 1; i ++)
	{
test_another_dup:
		if(!strcmp(HEADERS[i].key, HEADERS[i + 1].key))
		{
			int oldlen = strlen(HEADERS[i].val);

			char *newval;
			if(HEADERS[i].val_must_be_freed)
			{
				newval = realloc(HEADERS[i].val, oldlen + strlen(HEADERS[i + 1].val) + 3 /* 3 bytes = comma + space + zero byte */);
				if(!newval)
				{
					fprintf(stderr, "[error] realloc: memory allocation failed\n");
					exit(1);
				}

				newval[oldlen] = ',';
				newval[oldlen + 1] = ' ';

				strcpy(newval + oldlen + 2, HEADERS[i + 1].val);
			}
			else
			{
				if(asprintf(&newval, "%s, %s", HEADERS[i].val, HEADERS[i + 1].val) < 0)
				{
					fprintf(stderr, "[error] asprintf: memory allocation failed\n");
					exit(1);
				}
				HEADERS[i].val_must_be_freed = 1;
			}
			HEADERS[i].val = newval;

			/* Move the remaining elements of HEADERS[] to the beginning of the array */
			HEADERS_count --;
			for(j = i + 1; j < HEADERS_count; j ++)
				HEADERS[j] = HEADERS[j + 1];

			if(i != HEADERS_count - 1)
				goto test_another_dup;
		}
	}

	fprintf(stderr, "[debug] Total %i headers found:\n", HEADERS_count);
	for(i = 0; i < HEADERS_count; i ++)
	{
		fprintf(stderr, "[debug] Header[%i] '%s' is '%s'.\n", i, HEADERS[i].key, HEADERS[i].val);
	}

	/* OK, we've parsed the headers. Is it a redirect? */
	if(code >= 300) /* codes >= 400 have already been filtered before */
	{
		char *location = find_header(HEADERS, HEADERS_count, "location");

		fprintf(stderr, "[notice] Server returned redirect: %s\n", location);

		if(++ redirect_nr > max_redirects)
		{
			fprintf(stderr, "[error] Redirects depth limit reached: maximum %u are allowed\n", max_redirects);
			exit(1);
		}

		location = strdup(location);
		if(!location)
		{
			fprintf(stderr, "[error] strdup: memory allocation failed\n");
			exit(1);
		}
		free_headers(HEADERS, HEADERS_count);

		perform_http_request(location);
		free(location);

		return; /* Done. */
	}

	/*
		We don't need to support all headers.
		Transfer-Encoding and Content-Length are enough.

		Content-Encoding header can't be here because
		we never sent Accept-Encoding. But we'll check
		in case the server is misbehaving.
	*/
	if(find_header(HEADERS, HEADERS_count, "content-encoding"))
	{
		fprintf(stderr, "[error] Server has returned Content-Encoding header, but we support none of them. Exiting.\n");
		exit(1);
	}

	unsigned long len = 0;
	int is_chunked = 0;
	int no_length = 0; // 1 if there is neither "Transfer-Encoding: chunked" nor "Content-Length"

	char *transfer_encoding = find_header(HEADERS, HEADERS_count, "transfer-encoding");
	char *content_length = find_header(HEADERS, HEADERS_count, "content-length");
	if(!transfer_encoding) /* When Transfer-Encoding exists, we must ignore Content-Length */
	{
		if(content_length)
		{
			errno = 0;
			len = strtol(content_length, NULL, 10);

			if(errno)
			{
				fprintf(stderr, "[error] Malformed Content-Length response header: not a number.\n");
				exit(1);
			}
		}
		else
		{
			fprintf(stderr, "[warn] Server has responded without both Content-Length and Transfer-Encoding headers.\n");

			no_length = 1;
			len = (unsigned long) -1; // = very-very long
		}
	}
	else
	{
		/* Per HTTP/1.1, the client must support chunked */
		const char chunked[] = "chunked";

		if(content_length)
			fprintf(stderr, "[warn] Received both Transfer-Encoding and Content-Length. Ignoring the latter per RFC2616.\n");

		char *p = strstr(transfer_encoding, chunked);
		if(p)
		{
			fprintf(stderr, "[info] Server is using chunked transfer-encoding\n");
			is_chunked = 1;

			/* Is there something else in the Transfer-Encoding?
				We only support chunked. */
			memset(p, ' ', sizeof(chunked) - 1);

			char *q;
			for(q = transfer_encoding; *q != '\0'; q ++)
				if(!isspace(*q) && *q != ',')
				{
					// Restore 'transfer_encoding' for the error message below.
					memcpy(p, chunked, sizeof(chunked) - 1);

					fprintf(stderr, "[error] Server has requested transfer encoding \"%s\", we can't use that. Only 'chunked' transfer encoding is supported.\n", transfer_encoding);
					exit(1);
				}
		}
	}
	free_headers(HEADERS, HEADERS_count);

	SPENT();

	/* Read the response body. Note: part of it has already been read into "line" */
	const char *filename = "http.out"; // write response into this file
	int fout = open(filename, O_WRONLY | O_CREAT, 0600);
	if(fout < 0)
	{
		fprintf(stderr, "[error] open(\"%s\") failed: %s\n", filename, strerror(errno));
		exit(1);
	}
	if(ftruncate(fout, 0) < 0)
	{
		fprintf(stderr, "[error] ftruncate() failed: %s\n", strerror(errno));
		exit(1);
	}
	fprintf(stderr, "[info] Opened \"%s\" for writing.\n", filename);

	fprintf(stderr, "[info] Reading response body...\n");

	/* Put the socket back into the blocking mode */
	if(fcntl(sock, F_SETFL, 0) < 0)
	{
		fprintf(stderr, "[error] fcntl() failed: %s\n", strerror(errno));
		exit(1);
	}

	ssize_t bytes; int prefetched_bytes_needed;
	if(!is_chunked)
	{
		prefetched_bytes_needed = prefetched_body_length;
		if(len && len < prefetched_body_length)
		{
			prefetched_bytes_needed = len;

			fprintf(stderr, "[warn] Detecting (and ignoring) extra data in HTTP response (beyond the length specified by server).\n");
		}

		bytes = write(fout, line, prefetched_bytes_needed);
		if(bytes != prefetched_bytes_needed)
		{
			fprintf(stderr, "write() failed: %s\n", strerror(errno));
			exit(1);
		}

		len -= prefetched_bytes_needed;
		if(len == 0) // Everything read OK.
			goto close_file;

		size_t left = sendfile_from_socket(fout, sock, len);

		if(!no_length && left > 0)
			fprintf(stderr, "[warn] Response has ended prematurely (either the server has transmitted wrong length or the response body we received is incomplete)\n");
	}
	else
	{
		/* chunked method.
			For convenience we're moving the text which exists in
			"line" into the beginning of "buffer" (otherwise we'd
			have two different versions of code - for "line"
			and for "buffer").
		*/
		memmove(buffer, line, prefetched_body_length);
		buffer[prefetched_body_length] = '\0';

		buffer_offset = buffer + prefetched_body_length; // points to the end of what we have read

		char *start;
get_next_chunk:
		/* If buffer[] has only 1-2 symbols and they are CRLF,
			separate_CRLF() would return a correct pointer (to the
			string ""). We use strip_first_CRLF() to avoid that */
		start = strip_first_CRLF(buffer);
		p1 = separate_CRLF(start); /* points to the beginning of the next chunk */

		if(!p1)
		{
			// We've got an incomplete string (it must have length
			// and newline symbol in the end). Continue reading it.

			ssize_t ret = read(sock, buffer_offset, sizeof(buffer) - 1 - (buffer_offset - buffer));
			if(ret < 0)
			{
				fprintf(stderr, "[warn] read(sock) failed: %s\n", strerror(errno));
				exit(1);
			}

			if(ret == 0)
			{
				// We just read the entire buffer[], but haven't found any newlines.
				// This is an incorrect chunked from the server.

response_ended_prematurely:
				fprintf(stderr, "[warn] Response has ended prematurely (while waiting for another chunk). It might be incomplete\n");
				goto close_file;
			}

			buffer_offset += ret;
			*buffer_offset = '\0';

			goto get_next_chunk;
		}

		errno = 0;
		size_t chunk_len = strtol(start, 0, 16);
		if(errno)
		{
			fprintf(stderr, "[error] Malformed chunk length: not a number.\n");
			exit(1);
		}
		if(chunk_len == 0)
		{
			fprintf(stderr, "[debug] Last chunk received.\n");
			goto close_file;
		}

		fprintf(stderr, "[debug] Chunk length: %zu\n", chunk_len);

		/*
			Either the chunk is completely within buffer
			(already read), or the chunk is longer
		*/
		if(buffer_offset - p1 > chunk_len)
		{
			/* FIXME: this part of code wasn't really tested */

			/* The chunk is completely within the buffer.
				Just write it into the file. */
			if(write(fout, p1, chunk_len) < chunk_len)
			{
				fprintf(stderr, "write() failed: %s\n", strerror(errno));
				exit(1);
			}

			/* Move the rest into the beginning of buffer[] */
			int newlen = buffer_offset - p1 - chunk_len;
			memmove(buffer, p1 + chunk_len, newlen);
			buffer[newlen] = '\0';
			buffer_offset = buffer + newlen;

			goto get_next_chunk;
		}
		else
		{
			/* Write what we already have */

			size_t todo = buffer_offset - p1;
			if(write(fout, p1, todo) < todo)
			{
				fprintf(stderr, "write() failed: %s\n", strerror(errno));
				exit(1);
			}
			chunk_len -= todo;

			//fprintf(stderr, "p1[%i]:\n-------------\n%s\n-------------\n\n", todo, p1);

			/* Read into the beginning of buffer[] */

			while(chunk_len)
			{
				todo = chunk_len > MAX_HTTP_HEADERS_LENGTH ? MAX_HTTP_HEADERS_LENGTH : chunk_len;
				ssize_t bytes = read(sock, buffer, todo);

				if(bytes < 0)
				{
					fprintf(stderr, "[error] read() failed: %s\n", strerror(errno));
					exit(1);
				}

				//fprintf(stderr, "buffer[%i]:\n-------------\n%s\n-------------\n\n", bytes, buffer);

				if(write(fout, buffer, bytes) < bytes)
				{
					fprintf(stderr, "[error] write() failed: %s\n", strerror(errno));
					exit(1);
				}

				if(bytes == 0)
					break;

				chunk_len -= bytes;
			}

			if(chunk_len > 0) goto response_ended_prematurely;

			/* Go to the next chunk. Here buffer[] is completely
				blank (we didn't read more than we needed) */
			buffer[0] = '\0';
			buffer_offset = buffer;
			goto get_next_chunk;
		}
	}

close_file:
	alarm(0); /* Disable the timeout */
	SPENT();
	fprintf(stderr, "[notice] File received (saved to %s)\n", filename);

	struct stat st;
	if(fstat(fout, &st) < 0)
	{
		fprintf(stderr, "[error] fstat() failed: %s\n", strerror(errno));
		exit(1);
	}

	fprintf(stderr, "[info] %s is %i bytes long\n", filename, st.st_size);

	if(close(fout) < 0)
	{
		fprintf(stderr, "[error] close() failed: %s\n", strerror(errno));
		exit(1);
	}
}

int main( int argc, char **argv )
{
	if(argc != 2)
		print_usage();

	perform_http_request(argv[1]);
	return 0;
}
