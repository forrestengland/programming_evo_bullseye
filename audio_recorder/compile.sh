gcc -Wall -Wextra -O2 audio_recorder.c -o audio_recorder \
    $(pkg-config --cflags --libs jack sndfile) -pthread
