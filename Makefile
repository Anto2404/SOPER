TARGET = miner
CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -pthread
SRCS = miner.c pow.c
OBJS = $(SRCS:.c=.o)

# Parámetros de prueba para 'make all'
T_INI = 1000
ROUNDS = 10
THREADS = 4

# 1. Al escribir solo 'make', se ejecutará esta primera regla (SOLO COMPILA)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# 2. Al escribir 'make all', compila (si hace falta) y EJECUTA la prueba
all: $(TARGET)
	./$(TARGET) $(T_INI) $(ROUNDS) $(THREADS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

# Regla 'run' por si prefieres usarla en vez de 'all'
run: all

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	rm -f *.log

.PHONY: all clean distclean run