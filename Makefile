CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c99
OBJS = main.o jed.o device.o decode.o

xc7300dump: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) xc7300dump

.PHONY: clean

# Windows build (cross-compiled with mingw-w64, statically linked so it needs
# no DLLs). On Windows just run xc7300dump.exe from a Command Prompt.
windows:
	x86_64-w64-mingw32-gcc -O2 -Wall -static -o xc7300dump.exe \
	    main.c jed.c device.c decode.c
