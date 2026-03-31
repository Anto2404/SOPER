#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include "pow.h"

// Definiciones de nombres para que no haya errores
#define SEM_PIDS "/sem_pids"
#define FILE_PIDS "pids.pid"

// VARIABLES GLOBALES (Deben estar fuera del main para que limpiarysalir las vea)
sem_t *sem_pids; 
pid_t my_pid;

/* Función que limpia todo al salir (El Handler) */
void limpiarysalir(int sig) {
    printf("\nMiner %d exited system\n", my_pid); 
    
    sem_wait(sem_pids); // Sección crítica
    
    FILE *f = fopen(FILE_PIDS, "r");
    FILE *t = fopen("temp.pid", "w");
    int pid_read, contador = 0;

    if (f) {
        while (fscanf(f, "%d", &pid_read) != EOF) {
            if (pid_read != my_pid) {
                fprintf(t, "%d\n", pid_read);
                contador++;
            }
        }
        fclose(f);
    }
    fclose(t);
    rename("temp.pid", FILE_PIDS);

    if (contador == 0) {
        unlink(FILE_PIDS); // Borra el fichero si eres el último 
        sem_unlink(SEM_PIDS); // Borra el semáforo del sistema 
        printf("Last miner left. System cleaned.\n");
    }

    sem_post(sem_pids);
    sem_close(sem_pids);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <N_SECS> <N_THREADS>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int n_secs = atoi(argv[1]); 
    my_pid = getpid();         

    
    struct sigaction sa;
    sa.sa_handler = limpiarysalir; 
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL); 

    
    sem_pids = sem_open(SEM_PIDS, O_CREAT, 0644, 1);
    if (sem_pids == SEM_FAILED) { perror("sem_open"); exit(EXIT_FAILURE); }

   
    sem_wait(sem_pids); 
    FILE *f = fopen(FILE_PIDS, "a+"); 
    if (f) {
        fprintf(f, "%d\n", my_pid); 
        fclose(f);
    }
    printf("Miner %d added to system\n", my_pid); 
    sem_post(sem_pids); // Soltamos permiso

    
    alarm(n_secs); 

    while(1) {
        pause(); 
    }

    return 0;
}