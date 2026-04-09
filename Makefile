TARGET = miner
CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99 -pthread

SRCS = miner.c pow.c
OBJS = miner.o pow.o

# Parámetros de prueba
N_SECS    = 30
N_THREADS = 4

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

miner.o: miner.c
	$(CC) $(CFLAGS) -c miner.c

pow.o: pow.c
	$(CC) $(CFLAGS) -c pow.c

# Caso normal: 3 mineros con tiempos distintos
# - Minero 1: 15s → sale primero, rondas con 3 mineros [ Y Y Y ]
# - Minero 2: 20s → sale segundo, rondas con 2 mineros [ Y Y ]
# - Minero 3: 30s → último, limpia el sistema
# Cada 10 rondas globales el ganador fuerza solución incorrecta → Rejected
run: $(TARGET)
	rm -f pids.pid target.tgt voting.vot round.rnd *.txt /dev/shm/sem.sem_*
	./$(TARGET) 15 $(N_THREADS) & ./$(TARGET) 20 $(N_THREADS) & ./$(TARGET) $(N_SECS) $(N_THREADS) & wait

# Caso con 5 mineros: se ve cómo van saliendo uno a uno
# [ Y Y Y Y Y ] → [ Y Y Y Y ] → [ Y Y Y ] → [ Y Y ] → solo
run-5miners: $(TARGET)
	rm -f pids.pid target.tgt voting.vot round.rnd *.txt /dev/shm/sem.sem_*
	./$(TARGET) 10 $(N_THREADS) & ./$(TARGET) 15 $(N_THREADS) & ./$(TARGET) 20 $(N_THREADS) & ./$(TARGET) 25 $(N_THREADS) & ./$(TARGET) $(N_SECS) $(N_THREADS) & wait

clean:
	rm -f $(OBJS) $(TARGET) pids.pid target.tgt voting.vot round.rnd *.txt
	rm -f /dev/shm/sem.sem_*

distclean: clean

.PHONY: run run-5miners clean distclean