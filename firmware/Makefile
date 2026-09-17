DEVICE  = attiny85
F_CPU   = 16500000
COMPILE = avr-gcc -Wall -Os -DF_CPU=$(F_CPU) -mmcu=$(DEVICE) -I.

OBJECTS = main.o usbdrv.o usbdrvasm.o oddebug.o osccal.o

all: main.hex

.SUFFIXES: .c .S .o

%.o: %.c
	$(COMPILE) -c $< -o $@

%.o: %.S
	$(COMPILE) -x assembler-with-cpp -c $< -o $@

main.elf: $(OBJECTS)
	$(COMPILE) -o main.elf $(OBJECTS)
	avr-size main.elf

main.hex: main.elf
	rm -f main.hex
	avr-objcopy -j .text -j .data -O ihex main.elf main.hex

clean:
	rm -f main.hex main.elf $(OBJECTS)

flash: main.hex
	../micronucleus-2.0a4/micronucleus --run main.hex
