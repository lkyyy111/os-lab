# kernel/mem/Makefile
CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb -gdwarf-2 \
         -mcmodel=medany -ffreestanding -fno-common -nostdlib -mno-relax \
         -I. -fno-stack-protector -fno-pie -no-pie -I ../../include

SRC = pmem.c vmem.c
OBJ = $(SRC:.c=.o)

build: $(OBJ)

%.o: %.c
	riscv64-linux-gnu-gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o
