all: http_client

clean:
	rm -f http_client.o http_client

http_client: http_client.o
