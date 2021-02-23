gcc `pkg-config --cflags --libs libpipewire-0.3` -lm ./rnnoise/*.c ringbuf.c dsp-filter.c -o nt-pw-poc
