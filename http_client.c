/*
	Basic http client.
	Copyright (C) 2013 Edward Chernenko.

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

const unsigned request_timeout = 60; // в секундах
const unsigned max_redirects = 7;
const char *appname = "http_client";
const char *appversion = "0.1";
#define MAX_HTTP_HEADERS_LENGTH 4096 // максимальная общая длина всех HTTP-заголовков
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

char *find_header(struct http_header *HEADERS, int HEADERS_count, char *header_name_normalized) // header_name_normalized должно быть полностью в lowercase
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
	- обнаруживает в строке string перевод строки \n, либо \r, либо их комбинацию (\r\n или \n\r),
	- заменяет первый из этих попавшихся символов на \0
	- возвращает указатель на первый байт после CRLF.
	- возвращает NULL, если ни \r, ни \n в строке нет.
*/
char *separate_CRLF(char *string)
{
	char *p1, *p2;
	p1 = strchr(string, '\n');
	p2 = strchr(string, '\r');
		
	if(!p1 && !p2) return NULL;
		
	/* Поменяем p1 и p2 местами так, чтобы p1 находился ближе к началу строки (и не был равен NULL). p2 может быть NULL */
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
		
	*p1 = '\0'; // откусываем конец строки (её начало - begin)

	p1 ++;
	if(p1 == p2) // если за \r сразу идёт \n (или наоборот), то игнорируем второй символ
		p1 ++;
		
	return p1;
}

/* Если string начинается с CRLF, то возвращает указатель на следующее за ним. Иначе возвращает string.
	Отбрасывается не более одного CRLF (т.к. следующее за ним может быть бинарными данными)
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
	Метод для упрощения чтения тела ответа.
	По семантике похож на sendfile, но in_fd может быть сокетом.
	
	Возвращает количество НЕДОЧИТАННЫХ байт (т.е. 0, если count байт прочитаны целиком).
*/
size_t sendfile_from_socket(int out_fd, int in_fd, size_t count)
{
#define READ_BUFFER_SIZE 4096
	char buffer[READ_BUFFER_SIZE];
	size_t todo;
	ssize_t bytes;
	ssize_t written;
	
	while(count)
	{
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

	int i, j; // временные переменные для циклов
	char *p; // используется как временная переменная во время парсинга URL
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
				fprintf(stderr, "[error] HTTPS is not yet implemented.\n", begin);
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
	
	
	begin = p; // begin указывает на начало "example.com/some/path"
	p = strchr(begin, '/'); // отцепляем хост
	*p = '\0';
	
	char *host = begin; // указывает на "example.com" или "example.com:1234"
	char *path = p + 1; // указывает на "/some/path"
	
	// Переводим номер порта из текста в число
	p = strchr(host, ':');
	const char *port;
	if(p)
	{
		port = p + 1;
		*p = '\0'; // порт откусывается от строки host
	}
	else port = "80";
	
	/* Превращаем хост в IP-адрес (если там уже сейчас не IP-адрес) */
	
	struct addrinfo hints;
	struct addrinfo *ai;
	
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC; /* устроит и IPv4, и IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	int ret = getaddrinfo(host, port, &hints, &ai); // ai - односвязный список адресов, но все нам не нужны, возьмём первый
	if(ret != 0)
	{
		fprintf(stderr, "[error] Bad hostname or address: \"%s\": %s\n", host, gai_strerror(ret));
		exit(1);
	}

	int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if(sock < 0)
	{
		fprintf(stderr, "[error] socket() failed: %s\n", strerror(errno));
		exit(1);
	}
	
	/* Отслеживаем таймаут */
	signal(SIGALRM, timeout_handler);
	alarm(request_timeout);
	
	/* Хотим отследить, сколько времени занимал каждый этап */
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
	
	/* Читаем ответ. Здесь нужно перевести сокет в неблокирующий режим, т.к. когда мы читаем заголовки, мы можем попытаться
		прочитать больше, чем есть в ответе, если сам ответ мал (меньше максимальной длины заголовка). Что привело бы к таймауту */
	if(fcntl(sock, F_SETFL, O_NONBLOCK) < 0)
	{
		fprintf(stderr, "[error] fcntl() failed: %s\n", strerror(errno));
		exit(1);
	}
	
	char buffer[MAX_HTTP_HEADERS_LENGTH + 1]; // Будем читать до перевода строки, далее парсить простыми strchr
	int lineno = 0;
	int header_idx = 0; // номер http-заголовка, который мы читаем прямо сейчас, в массиве HEADERS[]
	
	struct pollfd fds;
	fds.fd = sock;
	fds.events = POLLIN;
	
	char *line = buffer; // Начало текущей строки (внутри buffer).
	char *buffer_offset = buffer; // указатель на первый байт массива buffer, куда мы читаем следующим вызовом read. Сдвигается с каждым read

	unsigned short code; // код ответа
	int prefetched_body_length; // количество байт тела HTTP-ответа, зачитанных в буфер на этапе чтения HTTP-заголовков (указатель line будет ссылаться на первый байт этих данных)

	char *p1;
	
	while(1)
	{
		if(poll(&fds, 1, -1) < 0) // таймаут к poll не нужен, у нас и так установлен alarm
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
		
		
#if 0 /* Код для отладки. Здесь можно подменить HTTP-ответ на любой текст и проверить, как он парсится */
		
#warning "Debug code: real response substituted"
		bytes_received = snprintf(buffer, MAX_HTTP_HEADERS_LENGTH, "HTTP/1.1 200 OK\nContent-Type: text/html; charset=UTF-8\nSome-Header: text123\n appendix to text123\nContent-Length: 112\nTransfer-Encoding: chunked\nTransfer-Encoding: something-else\nTransfer-Encoding: yet-another-line\nX-another: first line\nX-another: and yet another line\n\nINTERESTING CONTENT");
		
		/* WARNING: debug */
#endif
		
		char *new_offset = buffer_offset + bytes_received;
		*new_offset = '\0';
		
		// 1) В том, что уже было внутри buffer (ещё до вызова read), точно нет перевода строки. Ищем с buffer_offset.
		// 2) Перевод строки: стандарт допускает и \r, и \n, так и их комбинацию. Проверяем всё при помощи separate_CRLF.
		
check_buffer_for_newlines:
		p1 = separate_CRLF(buffer_offset); /* указывает на начало следующей строки */
		if(!p1) // перевода строки нет
		{
			if(bytes_received == space_left_in_buffer) // сервер, по-видимому, неисправен: прислал уже уйму всего, а HTTP-заголовки всё не заканчиваются
			{
				fprintf(stderr, "[error] HTTP response headers returned by server are too long (> %i bytes). Aborting (just in case).\n", MAX_HTTP_HEADERS_LENGTH);
				exit(1);
			}
			
			buffer_offset = new_offset;
			continue; // возвращаемся к poll и ждём, пока сервер пришлёт оставшуюся часть этой строки
		}
		
		/* обрабатываем строку */
		fprintf(stderr, "[debug] Received line: \"%s\"\n", line);
		if(lineno == 0)
		{
			char proto[10];
			char status[256];
			sscanf(line, "%9s %i %255s", proto, &code, status);
			
			/* Отловим "неподходящие" коды ответа */
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
				// Начало тела документа.
				// Важно: часть тела уже прочитана и находится по адресу line + 1.
				fprintf(stderr, "[info] All HTTP response headers have been received\n");
				line ++;
				
				/* Отбрасываем перевод строки (только один, т.к. тело ответа может быть бинарными данными) */
				line = strip_first_CRLF(line);
				
				prefetched_body_length = new_offset - line;
				goto read_response_body;
			}
			
			if(line[0] == ' ' || line[0] == '\t') /* если строка начинается с пробела/табуляции, то это продолжение предыдущего HTTP-заголовка */
			{
				do
				{
					line ++; // Пропускаем все пробелы/табуляции
				}
				while(*line == ' ' || *line == '\t');
				line --; // Оставить один пробел
				
				if(!HEADERS[header_idx].val)
				{
					fprintf(stderr, "[error] Server has sent a malformed _first_ HTTP header (starts with space or tabulation)\n");
					exit(1);
				}
				
				fprintf(stderr, "[debug] Appending \"%s\" to \"%s\" in \"%s\" header.\n", line, HEADERS[header_idx].val, HEADERS[header_idx].key);
				
				int oldlen = strlen(HEADERS[header_idx].val);
				int applen = strlen(line);
				
				//fprintf(stderr, "[debug] Appending \"%p\" to \"%p\" (diff %i) (len %i) header.\n", line, HEADERS[header_idx].val, line - HEADERS[header_idx].val, len);
				memmove(HEADERS[header_idx].val + oldlen, line, applen); // Места заведомо хватает: переносится на места, где раньше были переводы строки + тот же текст
				HEADERS[header_idx].val[oldlen + applen] = '\0';
			}
			else // Новый HTTP-заголовок ответа
			{
				char *val = strchr(line, ':');
				if(!val)
				{
					fprintf(stderr, "[error] Server has sent a malformed HTTP header (no colon).\n");
					exit(1);
				}
				
				*val = '\0';
				val ++;
				
				// убираем пробелы после двоеточия
				while(isspace(*val))
					val ++;
					
				if(HEADERS[header_idx].val)
					header_idx ++;
			
				fprintf(stderr, "[debug] Found header '%s': '%s' -> goes into HEADERS[%i]\n", line, val, header_idx);
				HEADERS[header_idx].key = line;
				HEADERS[header_idx].val = val;
				
				char *ptr;
				for(ptr = HEADERS[header_idx].key; *ptr != '\0'; ptr ++)
					*ptr = tolower(*ptr); // нормализация (названия заголовков - case-insensitive)
			}
		}
		
		line = p1;
		buffer_offset = p1;
		lineno ++;
		
		// Прочёл ли последний read более одной строки? Надо проверить.
		goto check_buffer_for_newlines;
	}
	
read_response_body:
	/* Изучим HTTP-заголовки ответа */
	HEADERS_count = header_idx;
	if(HEADERS[header_idx].val)
		HEADERS_count ++; 

	SPENT();
	
	/* Сперва разберёмся с массивом HEADERS, дабы в нём было проще искать */
	qsort(HEADERS, HEADERS_count, sizeof(HEADERS[0]), compare_headers_cb);
	
	/* Уберём дубликаты (значения дублирующих заголовков склеиваются через запятую) */
	for(i = 0; i < HEADERS_count - 1; i ++)
	{
test_another_dup:
		if(!strcmp(HEADERS[i].key, HEADERS[i + 1].key))
		{
			int oldlen = strlen(HEADERS[i].val);
		
			char *newval;
			if(HEADERS[i].val_must_be_freed)
			{
				newval = realloc(HEADERS[i].val, oldlen + strlen(HEADERS[i + 1].val) + 3 /* 3 байта = запятая + пробел + нулевой байт */);
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
			
			/* Сдвигаем оставшиеся элементы HEADER[] к началу массива */
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
	
	/* Отлично, с заголовками разобрались. А не редирект ли это? */
	if(code >= 300) /* коды >= 400 уже были отфильтрованы ранее */
	{
		char *location = find_header(HEADERS, HEADERS_count, "location");
	
		fprintf(stderr, "[notice] Server returned redirect: %s\n", location);
		
		if(++ redirect_nr > max_redirects)
		{
			fprintf(stderr, "[error] Redirects depth limit reached: maximum %i are allowed\n", max_redirects);
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
		
		return; /* Готово. */
	}

	/* Нам не надо разбираться во всех полях. Достаточно Transfer-Encoding и Content-Length.
		Заголовка Content-Encoding тут не может быть, т.к. мы не посылали Accept-Encoding. Но на случай битых серверов проверим.
	*/
	if(find_header(HEADERS, HEADERS_count, "content-encoding"))
	{
		fprintf(stderr, "[error] Server has returned Content-Encoding header, but we support none of them. Exiting.\n");
		exit(1);
	}
	
	unsigned long len = 0;
	int is_chunked = 0;
	int no_length = 0; // 1, если нет ни Transfer-Encoding: chunked, ни Content-Length
	
	char *transfer_encoding = find_header(HEADERS, HEADERS_count, "transfer-encoding");
	char *content_length = find_header(HEADERS, HEADERS_count, "content-length");
	if(!transfer_encoding) /* Если бы Transfer-Encoding был, мы были бы обязаны проигнорировать Content-Length */
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
			len = (unsigned long) -1; // = очень много
		}
	}
	else
	{
		/* Согласно стандарту HTTP/1.1, клиент обязан поддерживать chunked */
		const char chunked[] = "chunked";
		
		if(content_length)
			fprintf(stderr, "[warn] Received both Transfer-Encoding and Content-Length. Ignoring the latter per RFC2616.\n");

		char *p = strstr(transfer_encoding, chunked);
		if(p)
		{
			fprintf(stderr, "[info] Server is using chunked transfer-encoding\n");
			is_chunked = 1;
			
			/* Нет ли там ещё каких-нибудь Transfer-Encoding, которых мы не поддерживаем? */
			memset(p, ' ', sizeof(chunked) - 1);
			
			char *q;
			for(q = transfer_encoding; *q != '\0'; q ++)
				if(!isspace(*q) && *q != ',')
				{
					memcpy(p, chunked, sizeof(chunked) - 1); // Только для более корректного сообщения об ошибке
					fprintf(stderr, "[error] Server has requested transfer encoding \"%s\", we can't use that. Only 'chunked' transfer encoding is supported.\n", transfer_encoding);
					exit(1);					
				}
		}
	}
	free_headers(HEADERS, HEADERS_count);
	
	SPENT();
	
	/* Читаем тело документа. Замечание: часть тела документа нами уже прочитана (и находится в line) */
	const char *filename = "http.out";
	int fout = open(filename, O_WRONLY | O_CREAT); // записываем результат в этот файл
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

	/* Возвращаем сокет обратно в блокирующий режим */
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
		if(len == 0) // Дочитали.
			goto close_file;
		
		size_t left = sendfile_from_socket(fout, sock, len);
		
		if(!no_length && left > 0)
			fprintf(stderr, "[warn] Response has ended prematurely (either the server has transmitted wrong length or the response body we received is incomplete)\n");
	}
	else
	{
		/* chunked-метод.
			Тут мешает кусок из line, который пришлось бы обрабатывать по-другому, нежели читаемое из сокета.
			Поэтому сделаем так: читать будем в buffer[], а line переместим к его началу. 
		*/
		memmove(buffer, line, prefetched_body_length);
		buffer[prefetched_body_length] = '\0';
		
		buffer_offset = buffer + prefetched_body_length; // указывает на конец прочитанного
		
		char *start;
get_next_chunk:
		start = strip_first_CRLF(buffer); /* Во избежание ситуации, когда в buffer[] находятся всего один-два символа, и это CRLF (вызов separate_CRLF не понял бы подвоха и вернул бы корректный указатель) */
		p1 = separate_CRLF(start); /* указывает на начало следующего chunk */
		
		if(!p1) // имеем неполную строку (она должна содержать длину куска и перевод строки в конце). Её надо дочитать
		{
			ssize_t ret = read(sock, buffer_offset, sizeof(buffer) - 1 - (buffer_offset - buffer));
			if(ret < 0)
			{
				fprintf(stderr, "[warn] read(sock) failed: %s\n", strerror(errno));
				exit(1);
			}
			
			if(ret == 0) // Сюда же попадём, если мы так и не встретили перевода строки (зачитали весь buffer, а его там нет). Это неверный chunked со стороны сервера
			{
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
		
		fprintf(stderr, "[debug] Chunk length: %lu\n", chunk_len);

		/* Возможно два варианта: 1. кусок находится полностью внутри buffer (уже зачитан), 2. кусок длиннее */
		
		if(buffer_offset - p1 > chunk_len)
		{
			/* FIXME: этот кусок не протестирован */
		
			/* Кусок полностью внутри buffer. Просто записываем его в файл. */
			if(write(fout, p1, chunk_len) < chunk_len)
			{
				fprintf(stderr, "write() failed: %s\n", strerror(errno));
				exit(1);
			}
			
			/* Перемещаем остаток в начало buffer[] */
			int newlen = buffer_offset - p1 - chunk_len;
			memmove(buffer, p1 + chunk_len, newlen);
			buffer[newlen] = '\0';
			buffer_offset = buffer + newlen;
			
			goto get_next_chunk;
		}
		else
		{
			/* Записываем то, что у нас уже есть */
			
			size_t todo = buffer_offset - p1;
			if(write(fout, p1, todo) < todo)
			{
				fprintf(stderr, "write() failed: %s\n", strerror(errno));
				exit(1);
			}
			chunk_len -= todo;
			
			//fprintf(stderr, "p1[%i]:\n-------------\n%s\n-------------\n\n", todo, p1);
			
			/* читаем в начало buffer[] */
			
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
			
			/* А дальше у нас идёт следующий chunk. При этом buffer[] пуст (мы не читали более, чем нужно) */
			buffer[0] = '\0';
			buffer_offset = buffer;
			goto get_next_chunk;
		}
	}
	
close_file:
	alarm(0); /* Отключаем таймаут */
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
