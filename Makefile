TARGET = miner
CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -pthread

SRCS = miner.c pow.c
OBJS = $(SRCS:.c=.o)

# Parámetros de prueba
N_SECS   = 10
N_THREADS = 4

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# Lanza dos mineros como en el enunciado: uno de 5s y otro de 10s
all: $(TARGET)
	./$(TARGET) 5 $(N_THREADS) & ./$(TARGET) $(N_SECS) $(N_THREADS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

# Línea 22 y 23 (por si acaso, aunque parece que usas variables)
run: $(TARGET)
	./$(TARGET) $(N_SECS) $(N_THREADS)

# Línea 25 (Clean)
# Asegúrate de que $(OBJS) y $(TARGET) estén bien definidos arriba
clean:
	rm -f $(OBJS) $(TARGET) pids.pid target.tgt voting.vot *.txt
distclean: clean
	rm -f *.log

.PHONY: all clean distclean run