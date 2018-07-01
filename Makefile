CFLAGS += -W -Wall -Wextra

all: http_client

clean:
	rm -f http_client.o http_client

http_client: http_client.o

test: http_client
	chmod +x ./run_tests.sh
	./run_tests.sh
