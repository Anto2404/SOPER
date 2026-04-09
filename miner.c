#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 500
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include "pow.h"
#include <errno.h>

/*Ficheros del sistema*/
#define FILE_PIDS "pids.pid"     /**< Lista de PIDs de mineros activos */
#define FILE_TARGET "target.tgt" /**< Objetivo actual de la ronda (target) */
#define FILE_VOTES "voting.vot"  /**< Urna de votos de la ronda actual */
#define FILE_ROUND "round.rnd"   /**< Contador global de rondas */

/*Semaforos*/
#define SEM_PIDS "/sem_pids"     /**< Mutex para FILE_PIDS */
#define SEM_TARGET "/sem_target" /**< Mutex para FILE_TARGET y FILE_ROUND */
#define SEM_VOTES "/sem_votes"   /**< Mutex para FILE_VOTES */
#define SEM_WINNER "/sem_winner" /**< Semáforo de competición: vale 1 al inicio de cada ronda; el primer proceso que hace sem_trywait con éxito se proclama ganador */

/** PID del proceso actual */
static pid_t my_pid;
/** Número de hilos de minado configurado por parámetro */
static int n_threads;


static sem_t *sem_pids = NULL;
static sem_t *sem_target = NULL;
static sem_t *sem_votes = NULL;
static sem_t *sem_winner = NULL;

/* Flags de señal  escritos en handlers, leídos en main */
static volatile sig_atomic_t flag_start_round = 0;
static volatile sig_atomic_t flag_start_voting = 0;
static volatile sig_atomic_t flag_timeout = 0;



/* Estado de la ronda */
static volatile sig_atomic_t have_winner = 0; /**indica a los hilos de minado que ya hay ganador y deben parar.*/
static int i_am_winner = 0;/** Este proceso ganó la ronda actual */
static int my_coins = 0;/**monedas acumuladas por este proceso */
static int local_round = 0; /* rondas que ha participado este proceso */
static pid_t winner_pid = -1; /**Pid del proceso ganador */

/** Manejador de SIGUSR1: marca inicio de nueva ronda */
static void manejador_SIGUSR1(int sig)
{
    (void)sig;
    flag_start_round = 1;
}
/** Manejador de SIGUSR2: marca inicio de fase de votación */
static void manejador_SIGUSR2(int sig)
{
    (void)sig;
    flag_start_voting = 1;
    have_winner = 1;
}
/** Manejador de Sigalarm: marca fin del tiempo de vida del proceso */
static void manejador_SIGALRM(int sig)
{
    (void)sig;
    flag_timeout = 1;
    have_winner = 1;
}

/**
 * @brief Instala los manejadores de señal para SIGUSR1, SIGUSR2 y SIGALRM.
 *
 * Termina el proceso con error si alguna instalación falla.
 */
static void instalar_senales(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    /* Vaciamos la máscara (no bloqueamos ninguna señal extra al recibir esta) */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
/*Asignamos el manejador */
    sa.sa_handler = manejador_SIGUSR1;
    /*Registramos la señal en el nucleo*/
    if (sigaction(SIGUSR1, &sa, NULL) < 0)
    {
        perror("sigaction SIGUSR1");
        exit(EXIT_FAILURE);
    }
    sa.sa_handler = manejador_SIGUSR2;
    if (sigaction(SIGUSR2, &sa, NULL) < 0)
    {
        perror("sigaction SIGUSR2");
        exit(EXIT_FAILURE);
    }
    sa.sa_handler = manejador_SIGALRM;
    if (sigaction(SIGALRM, &sa, NULL) < 0)
    {
        perror("sigaction SIGALRM");
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Abre (o crea si no existen) los semáforos POSIX del sistema.
 *
 * Todos los semáforos se crean con valor inicial 1 (mutex).
 * sem_winner también empieza en 1: el primero en hacer sem_trywait gana.
 * Termina el proceso con error si algún semáforo no puede abrirse.
 */

static void abrir_semaforos(void)
{
    sem_pids = sem_open(SEM_PIDS, O_CREAT, 0644, 1);
    sem_target = sem_open(SEM_TARGET, O_CREAT, 0644, 1);
    sem_votes = sem_open(SEM_VOTES, O_CREAT, 0644, 1);
    sem_winner = sem_open(SEM_WINNER, O_CREAT, 0644, 1);

    if (sem_pids == SEM_FAILED || sem_target == SEM_FAILED ||
        sem_votes == SEM_FAILED || sem_winner == SEM_FAILED)
    {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }
}
/**
 * @brief Imprime por pantalla los PIDs actuales en FILE_PIDS.
 *
 * Debe llamarse con sem_pids ya cogido para evitar condiciones de carrera.
 */
static void imprimir_pids(void)
{
    FILE *f = fopen(FILE_PIDS, "r");
    if (!f)
        return;
    int p, first = 1;
    printf("  Miners in system: [");
    while (fscanf(f, "%d", &p) == 1)
    {
        if (!first)
            printf(", ");
        printf("%d", p);
        first = 0;
    }
    printf("]\n");
    fflush(stdout);
    fclose(f);
}

/**
 * @brief Añade el PID del proceso actual al fichero FILE_PIDS.
 *
 * Protegido por sem_pids. Imprime el estado del sistema tras la incorporación.
 */
static void add_mi_pid(void)
{
    sem_wait(sem_pids);
    FILE *f = fopen(FILE_PIDS, "a");
    if (f)
    {
        fprintf(f, "%d\n", my_pid);
        fclose(f);
    }
    printf("Miner %d added to system\n", my_pid);
    imprimir_pids();
    sem_post(sem_pids);
}

/**
 * @brief Elimina el PID del proceso actual del fichero FILE_PIDS.
 *
 * Si es el último minero, elimina todos los ficheros del sistema y
 * hace sem_unlink de todos los semáforos para liberar los recursos del sistema.
 * Protegido por sem_pids.
 */
static void eliminar_mi_pid(void)
{
    sem_wait(sem_pids);
    FILE *f = fopen(FILE_PIDS, "r");
    int pids[1024], n = 0;
    if (f)
    {
        int p;
        while (fscanf(f, "%d", &p) == 1)
            if (p != my_pid)
                pids[n++] = p;
        fclose(f);
    }
    if (n == 0)
    {
         /* Último minero: limpiar todo el sistema */
        unlink(FILE_PIDS);
        unlink(FILE_TARGET);
        unlink(FILE_VOTES);
        unlink(FILE_ROUND);
        sem_unlink(SEM_PIDS);
        sem_unlink(SEM_TARGET);
        sem_unlink(SEM_VOTES);
        sem_unlink(SEM_WINNER);
        printf("Last miner left. System cleaned.\n");
    }
    else
    {
          /* Reescribir el fichero sin el PID propio */
        FILE *t = fopen(FILE_PIDS, "w");
        if (t)
        {
            for (int i = 0; i < n; i++)
                fprintf(t, "%d\n", pids[i]);
            fclose(t);
        }
        printf("Miner %d exited system\n", my_pid);
        imprimir_pids();
    }
    sem_post(sem_pids);
}
/**
 * @brief Lee los PIDs activos del fichero FILE_PIDS.
 *
 * @param buf Array donde se almacenarán los PIDs leídos.
 * @param max Tamaño máximo del array buf.
 * @return Número de PIDs leídos.
 */
static int leer_pids(pid_t *buf, int max)
{
    int n = 0;
    sem_wait(sem_pids);
    FILE *f = fopen(FILE_PIDS, "r");
    if (f)
    {
        int p;
        while (n < max && fscanf(f, "%d", &p) == 1)
            buf[n++] = (pid_t)p;
        fclose(f);
    }
    sem_post(sem_pids);
    return n;
}
/**
 * @brief Lee el target actual del fichero FILE_TARGET.
 *
 * @return Valor del target leído, o 0 si el fichero no existe.
 */
static long int leer_target(void)
{
    long int t = 0;
    sem_wait(sem_target);
    FILE *f = fopen(FILE_TARGET, "r");
    if (f)
    {
        fscanf(f, "%ld", &t);
        fclose(f);
    }
    sem_post(sem_target);
    return t;
}
/**
 * @brief Escribe un nuevo valor de target en FILE_TARGET.
 *
 * @param t Nuevo valor del target a escribir.
 */
static void escribir_target(long int t)
{
    sem_wait(sem_target);
    FILE *f = fopen(FILE_TARGET, "w");
    if (f)
    {
        fprintf(f, "%ld\n", t);
        fclose(f);
    }
    sem_post(sem_target);
}

/**
 * @brief Incrementa y devuelve el contador global de rondas.
 *
 * Solo el ganador de cada ronda llama a esta función.
 * Protegido por sem_target .
 *
 * @return Número de ronda global tras el incremento.
 */
static int incrementar_ronda_global(void)
{
    int r = 0;
    sem_wait(sem_target);
    FILE *f = fopen(FILE_ROUND, "r");
    if (f)
    {
        fscanf(f, "%d", &r);
        fclose(f);
    }
    r++;
    f = fopen(FILE_ROUND, "w");
    if (f)
    {
        fprintf(f, "%d\n", r);
        fclose(f);
    }
    sem_post(sem_target);
    return r;
}

 
/**
 * @brief Registra una ronda ganada en el fichero personal <pid>.txt.
 *
 * Solo se llama cuando la ronda es aceptada, cuando los demas la rechazan no. El fichero acumula
 * todas las rondas ganadas por este proceso con su estado final.
 *
 * @param id        Número de ronda global.
 * @param target    Objetivo que había que resolver.
 * @param solution  Solución encontrada por este proceso.
 * @param votes_str Cadena con los votos recibidos .
 * @param yes       Número de votos yes.
 * @param total     Total de votos recibidos.
 */
static void registrar_ronda(int id, long int target, long int solution, const char *votes_str, int yes, int total)
{
    char fname[32];
    snprintf(fname, sizeof(fname), "%d.txt", my_pid);
    FILE *f = fopen(fname, "a");
    if (!f)
        return;
    fprintf(f, "Id: %d\n", id);
    fprintf(f, "Winner: %d\n", my_pid);
    fprintf(f, "Target: %ld\n", target);
    fprintf(f, "Solution: %ld (validated)\n", solution);
    fprintf(f, "Votes: %d/%d\n", yes, total);
    fprintf(f, "Wallets: %d:%d\n", my_pid, my_coins);
    fclose(f);
    (void)votes_str;
}
/**
 * @brief Espera no activa a una señal usando sigsuspend.
 *
 * @param flag   Puntero al flag que la señal pone a 1.
 * @param signum Número de la señal que se espera.
 */

static void esperar_signal(volatile sig_atomic_t *flag, int signum)
{
    sigset_t block_mask, oldmask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, signum);

    /* Bloquear la señal antes de comprobar el flag para evitar que llegue entre la comprobación y el sigsuspend */
    if (sigprocmask(SIG_BLOCK, &block_mask, &oldmask) == -1)
    {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }

    while (!(*flag) && !flag_timeout)
    {
        if (sigsuspend(&oldmask) == -1 && errno != EINTR)
        {
            perror("sigsuspend");
            exit(EXIT_FAILURE);
        }
    }
    *flag = 0;
 /* Restaurar la máscara original */
    if (sigprocmask(SIG_SETMASK, &oldmask, NULL) == -1)
    {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
}
/**
 * @brief Estructura de argumentos para cada hilo de minado.
 *
 * Cada hilo busca la solución en el rango [start, end).
 * Si la encuentra e intenta proclamarse ganador mediante sem_trywait,
 * escribe la solución en este struct.
 */
typedef struct
{
    long int start;  /**< Inicio del rango de búsqueda */
    long int end; /**< Fin del rango de búsqueda  */
    long int target;  /**< Hash objetivo a encontrar */
    long int solution; /**Solucion encontrada */
    int found; /**< 1 si este hilo encontró la solución */
} ThreadArg;

/**
 * @brief Función ejecutada por cada hilo de minado.
 *
 * Recorre el rango asignado buscando x tal que pow_hash(x) == target.
 * Si lo encuentra, intenta proclamarse ganador con sem_trywait sobre
 * sem_winner (. Si otro proceso ya ganó (sem_trywait
 * falla), pone have_winner=1 para que los demás hilos paren.
 *
 * @param arg Puntero a ThreadArg con el rango y target asignados.
 * @return NULL siempre.
 */

static void *minar_hilo(void *arg)
{
    ThreadArg *a = (ThreadArg *)arg;
    for (long int x = a->start; x < a->end && !have_winner; x++)
    {
        if (pow_hash(x) == a->target)
        {
            if (sem_trywait(sem_winner) == 0)
            {
                have_winner = 1;
                i_am_winner = 1;
                a->solution = x;
                winner_pid = my_pid;
                a->found = 1;
            }
            else
            {
                have_winner = 1;
            }
            break;
        }
    }
    return NULL;
}
/**
 * @brief Lanza n_threads hilos de minado y espera a que terminen.
 *
 * Divide el espacio de búsqueda [0, POW_LIMIT) entre los hilos.
 * Resetea have_winner y i_am_winner al inicio de cada llamada.
 *
 * @param target Hash objetivo que los hilos deben encontrar.
 * @return La solución encontrada, o 0 si ningún hilo de este proceso ganó.
 */
static long int ejecutar_minado(long int target)
{
    pthread_t *threads = malloc(n_threads * sizeof(pthread_t));
    ThreadArg *args = malloc(n_threads * sizeof(ThreadArg));
    if (!threads || !args)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
 /* Resetear estado de ganador para esta ronda */
    have_winner = 0;
    i_am_winner = 0;

    long int range = POW_LIMIT / n_threads;
    for (int i = 0; i < n_threads; i++)
    {
        args[i].start = i * range;
        args[i].end = (i == n_threads - 1) ? POW_LIMIT : (i + 1) * range;
        args[i].target = target;
        args[i].solution = 0;
        args[i].found = 0;
        pthread_create(&threads[i], NULL, minar_hilo, &args[i]);
    }

    long int solution = 0;
    for (int i = 0; i < n_threads; i++)
    {
        pthread_join(threads[i], NULL);
        if (args[i].found)
            solution = args[i].solution;
    }

    free(threads);
    free(args);
    return solution;
}
/**
 * @brief Emite el voto de este proceso en FILE_VOTES.
 *
 * Comprueba si pow_hash(solution) == target. Si es correcto vota 'Y',
 * si no vota 'N'. El ganador siempre llama con la solución real (Y),
 * los demás con la propuesta del fichero (N si el ganador mintió).
 *
 * @param solution Solución a verificar.
 * @param target   Target de la ronda actual.
 */

static void emitir_voto(long int solution, long int target)
{
    char vote = (pow_hash(solution) == target) ? 'Y' : 'N';
    sem_wait(sem_votes);
    FILE *f = fopen(FILE_VOTES, "a");
    if (f)
    {
        fprintf(f, "%d %c\n", my_pid, vote);
        fclose(f);
    }
    sem_post(sem_votes);
}
/**
 * @brief Lee y cuenta los votos del fichero FILE_VOTES.
 *
 * @param yes_out     Puntero donde se almacena el número de votos 'Y'.
 * @param no_out      Puntero donde se almacena el número de votos 'N'.
 * @param votes_str   Buffer donde se construye la cadena de votos (ej: "Y N Y ").
 * @param max_str     Tamaño máximo del buffer votes_str.
 * @return Total de votos leídos.
 */
static int contar_votos(int *yes_out, int *no_out, char *votes_str, int max_str)
{
    int yes = 0, no = 0, total = 0;
    votes_str[0] = '\0';
    sem_wait(sem_votes);
    FILE *f = fopen(FILE_VOTES, "r");
    if (f)
    {
        int pid_read;
        char v;
        while (fscanf(f, "%d %c\n", &pid_read, &v) == 2)
        {
            total++;
            if (v == 'Y')
                yes++;
            else
                no++;
            char tmp[4];
            snprintf(tmp, sizeof(tmp), "%c ", v);
            strncat(votes_str, tmp, max_str - strlen(votes_str) - 1);
        }
        fclose(f);
    }
    sem_post(sem_votes);
    *yes_out = yes;
    *no_out = no;
    return total;
}
 
/**
 * @brief Imprime las monedas finales y libera todos los recursos del proceso.
 *
 * Llama a eliminar_mi_pid() (que limpia ficheros si es el último),
 * cierra los semáforos y termina el proceso.
 */
static void limpiar_y_salir(void)
{
    printf("Miner %d finished with %d coins\n", my_pid, my_coins);
    fflush(stdout);
    eliminar_mi_pid();
    sem_close(sem_pids);
    sem_close(sem_target);
    sem_close(sem_votes);
    sem_close(sem_winner);
    exit(EXIT_SUCCESS);
}
 
/**
 * @brief Punto de entrada del programa.
 *
 * Inicializa el proceso, lo registra en el sistema y ejecuta el
 * bucle principal de rondas hasta que se agota el tiempo (SIGALRM).
 *
 *
 * @param argc Número de argumentos (debe ser 3).
 * @param argv argv[1] = N_SECS, argv[2] = N_THREADS.
 * @return 0 en éxito (en la práctica siempre sale por exit()).
 */
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <N_SECS> <N_THREADS>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int n_secs = atoi(argv[1]);
    n_threads = atoi(argv[2]);
    my_pid = getpid();
    long int solution;
    long int target;
    long int solution_to_write;
    int global_round;

    instalar_senales();
    abrir_semaforos();
    add_mi_pid();

    /* ── ¿Soy el primer minero? ── */
    int first_miner = 0;
    {
        sem_wait(sem_pids);
        FILE *f = fopen(FILE_PIDS, "r");
        int count = 0, p;
        if (f)
        {
            while (fscanf(f, "%d", &p) == 1)
                count++;
            fclose(f);
        }
        sem_post(sem_pids);
        first_miner = (count == 1);
    }

    alarm(n_secs);

    if (first_miner)
    {
        escribir_target(0);

        /* Inicializar contador global de rondas a 0 */
        sem_wait(sem_target);
        FILE *fr = fopen(FILE_ROUND, "w");
        if (fr)
        {
            fprintf(fr, "0\n");
            fclose(fr);
        }
        sem_post(sem_target);

        sem_wait(sem_votes);
        FILE *fv = fopen(FILE_VOTES, "w");
        if (fv)
            fclose(fv);
        sem_post(sem_votes);

        printf("Miner %d: waiting for another miner...\n", my_pid);
        fflush(stdout);
        while (!flag_timeout)
        {
            pid_t tmp[1024];
            int n = leer_pids(tmp, 1024);
            if (n >= 2)
                break;
            sleep(1);
        }
        if (flag_timeout)
        {
            limpiar_y_salir();
        }

        flag_start_round = 1;
        pid_t all[1024];
        int n_all;
        n_all = leer_pids(all, 1024);
        for (int i = 0; i < n_all; i++)
            if (all[i] != my_pid)
                kill(all[i], SIGUSR1);
    }

    /* ────────────────────────────────────────────
       BUCLE PRINCIPAL DE RONDAS
       ──────────────────────────────────────────── */
    while (!flag_timeout)
    {

        if (!flag_start_round)
            esperar_signal(&flag_start_round, SIGUSR1);
        if (flag_timeout)
            break;
        flag_start_round = 0;
        local_round++;

        pid_t all_pids[1024];
        int n_miners = leer_pids(all_pids, 1024);
        if (n_miners < 2)
        {
            printf("Miner %d: only 1 miner, waiting...\n", my_pid);
            fflush(stdout);
            continue;
        }

        target = leer_target();
        solution = ejecutar_minado(target);
        if (flag_timeout)
            break;

        /* ── SOY GANADOR ── */
        if (i_am_winner)
        {
            /* Incrementar ronda global y decidir si falsear la solución */
            global_round = incrementar_ronda_global();
            solution_to_write = solution;

            if (global_round % 10 == 0)
            {
                solution_to_write = solution + 1;
                printf("Miner %d: FORCING WRONG SOLUTION in round %d\n",
                       my_pid, global_round);
                fflush(stdout);
            }

            escribir_target(solution_to_write);

            sem_wait(sem_votes);
            FILE *fv = fopen(FILE_VOTES, "w");
            if (fv)
                fclose(fv);
            sem_post(sem_votes);

            n_miners = leer_pids(all_pids, 1024);
            for (int i = 0; i < n_miners; i++)
                if (all_pids[i] != my_pid)
                    kill(all_pids[i], SIGUSR2);

            flag_start_voting = 1;
        }

        /* ── FASE DE VOTACIÓN (todos) ── */
        if (!flag_start_voting)
            esperar_signal(&flag_start_voting, SIGUSR2);
        if (flag_timeout)
            break;
        flag_start_voting = 0;

        /* El ganador vota con la solución REAL (siempre Y).
           Los demás leen del fichero → N si el ganador mintió. */
        long int proposed = leer_target();
        if (i_am_winner)
        {
            emitir_voto(solution, target); /* solución real → Y */
        }
        else
        {
            emitir_voto(proposed, target); /* propuesta fichero */
        }

        /* ── RECUENTO (solo el ganador) ── */
        if (i_am_winner)
        {
            /* Releer n_miners justo antes de contar para pillar
        si alguno salió durante la votación */
            n_miners = leer_pids(all_pids, 1024);
            int attempts = 0, max_attempts = 10;
            int yes = 0, no = 0, total = 0;
            char votes_str[512] = "";

            do
            {
                usleep(100000);
                total = contar_votos(&yes, &no, votes_str, sizeof(votes_str));
                attempts++;
            } while (total < n_miners && attempts < max_attempts);

            int accepted = (yes > no);
            printf("Winner %d => [ %s] => %s\n",
                   my_pid, votes_str, accepted ? "Accepted" : "Rejected");
            fflush(stdout);

            if (accepted)
            {
                my_coins++;
                registrar_ronda(global_round, target, solution,
                                votes_str, yes, total);
            }

            /* El nuevo target es siempre la solución real */
            escribir_target(solution);
            sem_post(sem_winner);

            /* Pausa breve para que los que terminan actualicen pids */
            usleep(50000);
            n_miners = leer_pids(all_pids, 1024);
            int alive = 0;
            for (int i = 0; i < n_miners; i++)
                if (kill(all_pids[i], 0) == 0)
                    alive++;

            if (alive >= 2)
            {
                for (int i = 0; i < n_miners; i++)
                    if (all_pids[i] != my_pid && kill(all_pids[i], 0) == 0)
                        kill(all_pids[i], SIGUSR1);
                flag_start_round = 1;
            }

            i_am_winner = 0;
        }
    }

    limpiar_y_salir();
    return 0;
}