CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS ?= -lcurl -lpthread
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

HOST_OBJECTS = sirio.o sirio_provider.o sirio_core.o sirio_worker.o \
	sirio_help.o sirio_container.o linenoise.o
TEST_BINS = tests/sirio_core_test tests/sirio_provider_contract_test \
	tests/sirio_provider_terminal_test tests/sirio_provider_http_test \
	tests/sirio_container_test tests/sirio_tool_runner_test

.DEFAULT_GOAL := help

.PHONY: help install test sanitize clean

help:
	@echo "Sirio build targets:"
	@echo "  make                   Show this help"
	@echo "  make sirio             Build ./sirio"
	@echo "  make test              Build and run the test suite"
	@echo "  make sanitize          Run the tests with ASan and UBSan"
	@echo "  make clean             Remove generated files"
	@echo
	@echo "Install:"
	@echo "  make install           Install sirio in $(BINDIR)"

sirio: $(HOST_OBJECTS)
	$(CC) $(LDFLAGS) -o sirio $(HOST_OBJECTS) $(LDLIBS)


install:
	@if [ ! -x sirio ]; then \
		echo "sirio: ./sirio is missing; run 'make sirio' first" >&2; \
		exit 1; \
	fi
	@sudo install -d "$(BINDIR)"
	@sudo install -m 0755 sirio "$(BINDIR)/sirio"

sirio.o: sirio.c sirio_provider.h sirio_worker.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ sirio.c

sirio_provider.o: sirio_provider.c sirio_provider.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ sirio_provider.c

sirio_core.o: sirio_core.c sirio_core.h sirio_worker.h sirio_container.h linenoise.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ sirio_core.c

sirio_worker.o: sirio_worker.c sirio_worker.h sirio_core.h sirio_provider.h sirio_container.h linenoise.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ sirio_worker.c

sirio_container.o: sirio_container.c sirio_container.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ sirio_container.c

sirio_help.o: sirio_help.c sirio_core.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ sirio_help.c

linenoise.o: linenoise.c linenoise.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ linenoise.c

tests/sirio_core_test.o: tests/sirio_core_test.c sirio_core.c sirio_core.h sirio_worker.h sirio_worker.c sirio_container.h linenoise.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ tests/sirio_core_test.c

tests/sirio_core_test: tests/sirio_core_test.o sirio.c sirio_provider.o sirio_help.o sirio_container.o linenoise.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -DSIRIO_NO_MAIN -o $@ tests/sirio_core_test.o sirio.c sirio_provider.o sirio_help.o sirio_container.o linenoise.o $(LDLIBS)

tests/sirio_provider_contract_test.o: tests/sirio_provider_contract_test.c sirio_provider.c sirio_provider.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ tests/sirio_provider_contract_test.c

tests/sirio_provider_contract_test: tests/sirio_provider_contract_test.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/sirio_provider_terminal_test: tests/sirio_provider_terminal_test.c sirio_provider.c sirio_provider.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/sirio_provider_terminal_test.c $(LDLIBS)

tests/sirio_provider_http_test: tests/sirio_provider_http_test.c sirio_provider.c sirio_provider.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/sirio_provider_http_test.c $(LDLIBS)

tests/sirio_container_test: tests/sirio_container_test.c sirio_container.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/sirio_container_test.c sirio_container.o

tests/sirio_tool_runner_test: tests/sirio_tool_runner_test.c container/sirio_tool_runner.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/sirio_tool_runner_test.c -lpthread

test: sirio $(TEST_BINS)
	./tests/sirio_core_test
	./tests/sirio_provider_contract_test
	./tests/sirio_provider_terminal_test
	./tests/sirio_provider_http_test
	./tests/sirio_container_test
	./tests/sirio_tool_runner_test

sanitize: clean
	@status=0; \
	$(MAKE) -j2 test \
		CFLAGS='-std=c11 -O0 -g1 -Wall -Wextra -D_GNU_SOURCE -fno-omit-frame-pointer -fsanitize=address,undefined' \
		LDFLAGS='-fsanitize=address,undefined' || status=$$?; \
	$(MAKE) --no-print-directory clean; \
	exit $$status

clean:
	rm -f sirio $(HOST_OBJECTS) $(TEST_BINS)
	rm -f tests/sirio_core_test.o tests/sirio_provider_contract_test.o
