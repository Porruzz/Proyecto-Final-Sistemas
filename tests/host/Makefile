# Pruebas del nucleo en el PC (sin hardware).  Uso:  make test
CORE   = ../../firmware/components/complexnet/core
CC     = gcc
CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Werror -I$(CORE)
SRC    = $(CORE)/cn_core.c $(CORE)/cn_proto.c $(CORE)/cn_scheduler.c
BIN    = build

all: $(BIN)/test_proto $(BIN)/test_core $(BIN)/test_integration

$(BIN):
	mkdir -p $(BIN)

$(BIN)/test_proto: test_proto.c $(SRC) test_util.h | $(BIN)
	$(CC) $(CFLAGS) $< $(SRC) -lm -o $@

$(BIN)/test_core: test_core.c sim_net.c sim_net.h $(SRC) test_util.h | $(BIN)
	$(CC) $(CFLAGS) $< sim_net.c $(SRC) -lm -o $@

$(BIN)/test_integration: test_integration.c sim_net.c sim_net.h $(SRC) test_util.h | $(BIN)
	$(CC) $(CFLAGS) $< sim_net.c $(SRC) -lm -o $@

$(BIN)/demo_failure: demo_failure.c sim_net.c sim_net.h $(SRC) | $(BIN)
	$(CC) $(CFLAGS) $< sim_net.c $(SRC) -lm -o $@

demo: $(BIN)/demo_failure
	./$(BIN)/demo_failure

$(BIN)/escenario: escenario.c sim_net.c sim_net.h $(SRC) | $(BIN)
	$(CC) $(CFLAGS) $< sim_net.c $(SRC) -lm -o $@

escenario: $(BIN)/escenario

test: all
	./$(BIN)/test_proto
	./$(BIN)/test_core
	mkdir -p ../../results
	./$(BIN)/test_integration ../../results/metrics.json

# Mismas pruebas con AddressSanitizer + UndefinedBehaviorSanitizer
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all" test

clean:
	rm -rf $(BIN)

.PHONY: all test sanitize clean demo escenario
