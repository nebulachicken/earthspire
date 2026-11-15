# Build the calculator client with the Ndless SDK on PATH
# (nspire-gcc, nspire-ld, genzehn, make-prg).
#
#   make            -> capeviewer.prg.tns
#   make clean

EXE      = capeviewer
COMMON   = ../common
CFLAGS   = -Wall -W -marm -I$(COMMON)
OBJS     = main.o svd_format.o svd_render.o
ZEHNFLAGS = --name "Cape Canaveral" --240x320-support true --uses-lcd-blit true

all: $(EXE).prg.tns

main.o: main.c
	nspire-gcc $(CFLAGS) -c main.c -o main.o

svd_format.o: $(COMMON)/svd_format.c
	nspire-gcc $(CFLAGS) -c $< -o $@

svd_render.o: $(COMMON)/svd_render.c
	nspire-gcc $(CFLAGS) -c $< -o $@

$(EXE).elf: $(OBJS)
	nspire-ld $(OBJS) -o $@

$(EXE).prg.tns: $(EXE).elf
	genzehn --input $< --output $(EXE).zehn $(ZEHNFLAGS)
	make-prg $(EXE).zehn $@
	rm -f $(EXE).zehn

clean:
	rm -f *.o *.elf *.zehn *.tns
