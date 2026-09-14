# PS4 Fan Control PayLoad - SDK build
# Requires ps4-payload-dev/sdk (v0.9 or newer) installed.

PS4_HOST ?= ps4
PS4_PORT ?= 9021

ifndef PS4_PAYLOAD_SDK
$(error PS4_PAYLOAD_SDK is undefined. Example: export PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk)
endif

include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk

PAYLOAD := fan_control.elf
SRCS    := main.c

CFLAGS  := -Wall -Wextra -Werror -std=c11 -O1 -g

.PHONY: all clean test

all: $(PAYLOAD)

$(PAYLOAD): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(PAYLOAD) *.o

test: $(PAYLOAD)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $(PAYLOAD)
