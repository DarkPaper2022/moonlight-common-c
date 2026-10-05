CC ?= gcc
CFLAGS ?= -O2 -Wall

all: ipad_audio_client pc_audio_sender

ipad_audio_client: ipad_audio_client.c
	$(CC) $(CFLAGS) -pthread $< -o $@

pc_audio_sender: pc_audio_sender.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f ipad_audio_client pc_audio_sender

.PHONY: all clean
