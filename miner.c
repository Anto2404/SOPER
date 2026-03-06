
/**
 * @file miner.c
 * @brief Implementación de un minero PoW multihilo con sistema de registro mediante tuberías.
 * Este programa simula el minado de bloques utilizando hilos para dividir el espacio
 * de búsqueda de solucion. El proceso padre coordina los hilos y el proceso hijo actúa como
 * un registrador (logger) que guarda los resultados en un archivo de log.
 * * @author Daniel GOnzalez Ureta y Antonino Albarran Peñas
 * @date 05 de Marzo, 2026
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "pow.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
/* Variables globales para la comunicación entre hilos */
volatile int solucion_found = 0; /**< Flag indicador de si algún hilo encontró la solución. Esta a 0 a menos que la solucion haya sido encontrada */
long solucion;                   /**< Almacena el valor de la solución encontrada */
/**
 * @struct thread_data
 * @brief Estructura para pasar argumentos e informacion a los hilos de minado.
 */
typedef struct
{
    long start;
    long end;
    long target; /*Numero de hilos*/
} thread_data;
/**
 * @brief Función que ejecuta cada hilo para realizar la búsqueda (minado).
 * Cada hilo recorre su rango asignado. Si encuentra un valor cuyo hash coincide
 * con el target, actualiza la solución global y activa el flag para que los
 * demás hilos se detengan.
 *  @param arg Puntero a estructura thread_data con los límites del rango.
 * @return NULL al finalizar la ejecución del hilo.
 */
void *mine(void *arg)
{
    thread_data *data = (thread_data *)arg;
    int i;
    for (i = data->start; i < data->end && !solucion_found; i++)
    {
        if (pow_hash(i) == data->target)
        {
            solucion = i;
            solucion_found = 1;
        }
    }
    pthread_exit(NULL); /*salimos del hilo*/
}
/**
 * @brief Función principal que gestiona el flujo del programa.
 * Crea la tubería, crea el proceso hijo para registro y ejecuta las
 * rondas de minado distribuyendo el trabajo entre hilos.
 * @param argc Número de argumentos de línea de comandos.
 * @param argv Array de strings: [1] Target inicial, [2] Rondas, [3] Nº hilos.
 * @return int Código de salida (0 éxito, 1 error).
 */
int main(int argc, char *argv[])
{

    int target;
    int n_rounds;
    int n_threads;
    int tamaño;
    int i, j, k;
    int fd[2];
    pid_t pid;
    char filename[32];

    if (argc != 4)
    {
        fprintf(stderr, " Use: %s <TARGET_INI> <ROUNDS><N_THREADS>\n", argv[0]);
    }
    target = atoi(argv[1]);
    n_rounds = atoi(argv[2]);
    n_threads = atoi(argv[3]);

    if (n_rounds <= 0)
    {
        printf("Miner: no hay rondas que ejecutar\n");
        exit(EXIT_SUCCESS);
    }

    if (n_threads <= 0)
    {
        fprintf(stderr, "Error: el numero de hilos debe ser mayor que 0\n");
        exit(EXIT_FAILURE);
    }

    /*Cremos la tuberia antes de fork para que amos procesos puedan usar la misma tuberia */
    if (pipe(fd) == -1)
    {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    pid = fork();
    if (pid == -1)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    /*Evaluamos el proceso padre*/
    if (pid > 0)
    {
        close(fd[0]); /*El padre no lee*/

        for (i = 0; i < n_rounds; i++)
        {
            pthread_t threads[n_threads];       /*Creamos _thereads hilos*/
            thread_data thread_data[n_threads]; /*Creamos threads estructuras de datos para cada hilo*/
            tamaño = POW_LIMIT / n_threads;     /* El numero de veces */
            solucion = -1;
            solucion_found = 0;
            int validada;
            for (j = 0; j < n_threads; j++)
            {
                thread_data[j].start = j * tamaño;
                if (j == n_threads - 1)
                {
                    thread_data[j].end = POW_LIMIT;
                }
                else
                {
                    thread_data[j].end = (j + 1) * tamaño;
                }
                thread_data[j].target = target;

                if (pthread_create(&threads[j], NULL, mine, &thread_data[j]) != 0) /*cREAMOS LOS HILOS Y LES DECIMOS QUE USE MINE QUE ES LA FUNCION QUE SE USA PARA ENCONTRAR LA SOLUCION*/
                {
                    perror("pthread_create");
                    exit(EXIT_FAILURE);
                }
            }
            /*Esperamos a que todos los hilos terminen*/
            for (k = 0; k < n_threads; k++)
            {
                if (pthread_join(threads[k], NULL) != 0) /*Bloqueamos el padre hasta que todos los hijos acaban */
                {
                    perror("pthread_join");
                    exit(EXIT_FAILURE);
                }
            }

            if (solucion != -1)
            {
                if ((i + 1) % 15 == 0)
                { /*Cada 15 vees se rechaza*/
                    validada = 0;
                }
                else
                {
                    validada = 1;
                }
                if (validada)
                {
                    printf("Solution accepted: %08d --> %08ld\n", target, solucion);
                }
                else
                {
                    printf("Solution rejected: %08d --> %08ld\n", target, solucion);
                }

                /*Ahora que hemos encontrado la solucion lo escribimos en la tuberia para enviarselo al hijo*/
                write(fd[1], &target, sizeof(int));
                write(fd[1], &solucion, sizeof(long));
                write(fd[1], &validada, sizeof(int));

                target = solucion;
            }
            else
            {
                // Si alguna ronda falla, paramos
                printf("Miner: No se encontró solución en la ronda %d\n", i);
            }
        }
        close(fd[1]);
        wait(NULL); /*Espero a que el hijo finalice de escribir su proceso*/
        printf("Miner exited with status 0\n");
        exit(EXIT_SUCCESS);
    }
    else /*Proceso registrador*/
    {
        int r_target;
        long r_solucion;
        int round = 0;
        int validated;
        close(fd[1]);                                 /*No va a escribir solo va a leer*/
        sprintf(filename, "miner_%d.log", getppid()); /*Creamos un fichero con ese nombre*/
        FILE *f = fopen(filename, "w");               /*Lo abrimos para escribir*/
        while (read(fd[0], &r_target, sizeof(int)) > 0)
        {
            read(fd[0], &r_solucion, sizeof(long));
            read(fd[0], &validated, sizeof(int));

            fprintf(f, "Id:       %d\n", round);
            fprintf(f, "Winner:    %d\n", getppid());
            fprintf(f, "Target:    %d\n", r_target);
            fprintf(f, "Solution:  %08ld (%s)\n", r_solucion, validated ? "validated" : "rejected");
            fprintf(f, "Votes:    %d/%d\n", round, round);
            fprintf(f, "Wallets:  %d:%d\n", getppid(), round);
            fprintf(f, "------------------------------\n");
            fflush(f);
            round++;
        }
        fclose(f);
        close(fd[0]);
        printf("Logger exited with status 0\n");
        exit(EXIT_SUCCESS);
    }
}
