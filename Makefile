CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lssl -lcrypto
TARGET = webserver
SRC = webserver.c account_service.c

.PHONY: all build monitor-mod manage-repos api tunnel premium watchdog employee-quarantine run s start serve packager deployer tools seed seed-data unseed-data clean help

all: build

build: $(TARGET)

monitor-mod: monitor-mod.c
	$(CC) $(CFLAGS) -o $@ $< $(GTK_CFLAGS) $(GTK_LIBS)

manage-repos:
	chmod +x manage-cadbay-repos.sh
	./manage-cadbay-repos.sh

seed: tools/seed_test_data
tools/seed_test_data: tools/seed_test_data.c
	$(CC) $(CFLAGS) -o $@ $<
seed-data: tools/seed_test_data
	./tools/seed_test_data -r 60 -a 24
unseed-data: tools/seed_test_data
	./tools/seed_test_data -c
api: api/create-rfp api/formRequest api/info

api/create-rfp: api/create-rfp.c
	$(CC) $(CFLAGS) -o $@ $<

api/formRequest: api/formRequest.c
	$(CC) $(CFLAGS) -o $@ $<

api/info: api/info.c
	$(CC) $(CFLAGS) -o $@ $<

tunnel: cadbay-tunnel

premium: add_premium_member renew_premium

watchdog: premium_watchdog

employee-quarantine: employee_quarantine

employee_quarantine: employee_quarantine.c
	$(CC) $(CFLAGS) -o $@ $<

# GTK3 packaging/deployment tooling
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)

packager: cadbay-packager

deployer: cadbay-deployer

tools: cadbay-packager cadbay-deployer

cadbay-packager: cadbay-packager.c cadbay_crypto.c cadbay_crypto.h
	$(CC) $(CFLAGS) -o $@ cadbay-packager.c cadbay_crypto.c $(GTK_CFLAGS) $(GTK_LIBS) -lcrypto

cadbay-deployer: cadbay-deployer.c cadbay_crypto.c cadbay_crypto.h
	$(CC) $(CFLAGS) -o $@ cadbay-deployer.c cadbay_crypto.c $(GTK_CFLAGS) $(GTK_LIBS) -lcrypto

$(TARGET): $(SRC) account_service.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

cadbay-tunnel: tunnel.c
	$(CC) $(CFLAGS) -o $@ $<

add_premium_member: add_premium_member.c premium_account_common.h
	$(CC) $(CFLAGS) -o $@ $<

renew_premium: renew_premium.c premium_account_common.h
	$(CC) $(CFLAGS) -o $@ $<

premium_watchdog: premium_watchdog.c premium_account_common.h
	$(CC) $(CFLAGS) -o $@ $<

run: build
	./$(TARGET)

s: build monitor-mod
	./monitor-mod -s

start: s

serve: s

clean:
	rm -f $(TARGET) cadbay-tunnel add_premium_member renew_premium premium_watchdog employee_quarantine cadbay-packager cadbay-deployer recovery_selftest crypto_selftest tools/seed_test_data
	rm -rf recovery_test_tmp

help:
	@echo "Available targets:"
	@echo "  make build      - Compile the webserver"
	@echo "  make api        - Compile the standalone API utilities"
	@echo "  make seed       - Compile the test data seeder"
	@echo "  make seed-data  - Create 60 test RFPs and 24 test ads"
	@echo "  make unseed-data - Delete the seeded test RFPs and ads"
	@echo "  make tunnel     - Compile the optional raw-IP TCP tunnel"
	@echo "  make premium    - Compile premium activation and renewal tools"
	@echo "  make watchdog   - Compile the premium expiry watchdog"
	@echo "  make employee-quarantine - Compile the employee pre-hosting quarantine tool"
	@echo "  make tools      - Compile the GTK3 packager and deployer GUIs"
	@echo "  make packager   - Compile the component packager GUI"
	@echo "  make deployer   - Compile the component deployer GUI"
	@echo "  make run        - Build and run the webserver"
	@echo "  make s          - Start the server via monitor-mod -s"
	@echo "  make start      - Alias for make s"
	@echo "  make clean      - Remove compiled files"
	@echo "  make help       - Show this help message"
