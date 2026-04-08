#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
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
#define _POSIX_C_SOURCE 200809L
/* ────────────────────────────────────────────────────────────
   FICHEROS Y SEMÁFOROS DEL SISTEMA
   ──────────────────────────────────────────────────────────── */
#define FILE_PIDS "pids.pid"
#define FILE_TARGET "target.tgt"
#define FILE_VOTES "voting.vot"

#define SEM_PIDS "/sem_pids"
#define SEM_TARGET "/sem_target"
#define SEM_VOTES "/sem_votes"
#define SEM_WINNER "/sem_winner"

/* ────────────────────────────────────────────────────────────
   VARIABLES GLOBALES
   ──────────────────────────────────────────────────────────── */
static pid_t my_pid;
static int n_threads;

/* Semáforos globales para que el handler los pueda usar */
static sem_t *sem_pids = NULL;
static sem_t *sem_target = NULL;
static sem_t *sem_votes = NULL;
static sem_t *sem_winner = NULL;

/* Flags de señal — solo se escriben en handlers, se leen en main */
static volatile sig_atomic_t flag_start_round = 0;
static volatile sig_atomic_t flag_start_voting = 0;
static volatile sig_atomic_t flag_timeout = 0;

/* Estado de la ronda */
static volatile sig_atomic_t have_winner = 0; /* hay ganador en esta ronda */
static pid_t winner_pid = -1;
static int i_am_winner = 0;
static int my_coins = 0;
static int round_id = 0;

/* ────────────────────────────────────────────────────────────
   PROTOTIPOS
   ──────────────────────────────────────────────────────────── */
static void print_pids(void);
static void remove_my_pid(void);
static void cleanup_and_exit(int coins);

/* ────────────────────────────────────────────────────────────
   HANDLERS DE SEÑALES  (mínimos — solo ponen flags)
   ──────────────────────────────────────────────────────────── */
static void handler_SIGUSR1(int sig)
{
    (void)sig;
    flag_start_round = 1;
}
static void handler_SIGUSR2(int sig)
{
    (void)sig;
    flag_start_voting = 1; /*Comienza la fase de votacion*/
    have_winner = 1;
}
static void handler_SIGALRM(int sig)
{
    (void)sig;
    flag_timeout = 1;
    have_winner = 1;
}

/* ────────────────────────────────────────────────────────────
   INSTALACIÓN DE SEÑALES
   ──────────────────────────────────────────────────────────── */
static void setup_signals(void)
{
    struct sigaction sa = {0};

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sa.sa_handler = handler_SIGUSR1;
    if (sigaction(SIGUSR1, &sa, NULL) < 0)
    {
        perror("sigaction SIGUSR1");
        exit(EXIT_FAILURE);
    }

    sa.sa_handler = handler_SIGUSR2;
    if (sigaction(SIGUSR2, &sa, NULL) < 0)
    {
        perror("sigaction SIGUSR2");
        exit(EXIT_FAILURE);
    }

    sa.sa_handler = handler_SIGALRM;
    if (sigaction(SIGALRM, &sa, NULL) < 0)
    {
        perror("sigaction SIGALRM");
        exit(EXIT_FAILURE);
    }
}

static void open_semaphores(void)
{

    sem_pids = sem_open(SEM_PIDS, O_CREAT, 0644, 1);
    sem_target = sem_open(SEM_TARGET, O_CREAT, 0644, 1);
    sem_votes = sem_open(SEM_VOTES, O_CREAT, 0644, 1);
    /* sem_winner valor inicial 1 (para que el primero lo coja) */
    sem_winner = sem_open(SEM_WINNER, O_CREAT, 0644, 1);

    if (sem_pids == SEM_FAILED ||
        sem_target == SEM_FAILED ||
        sem_votes == SEM_FAILED ||
        sem_winner == SEM_FAILED)
    {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }
}

/* ────────────────────────────────────────────────────────────
   GESTIÓN DEL FICHERO DE PIDS
   ──────────────────────────────────────────────────────────── */

/* Imprime todos los PIDs que hay en el fichero (llamar con sem cogido) */
static void print_pids(void)
{
    FILE *f = fopen(FILE_PIDS, "r");
    if (!f)
        return;
    int p;
    printf("  Miners in system: [");
    int first = 1;
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

/* Añade my_pid al fichero de PIDs */
static void add_my_pid(void)
{
    sem_wait(sem_pids);
    FILE *f = fopen(FILE_PIDS, "a");
    if (f)
    {
        fprintf(f, "%d\n", my_pid);
        fclose(f);
    }
    printf("Miner %d added to system\n", my_pid);
    print_pids();
    sem_post(sem_pids);
}

/* Elimina my_pid del fichero; si es el último borra el fichero */
static void remove_my_pid(void)
{
    sem_wait(sem_pids);

    /* Leer todos los PIDs excepto el nuestro */
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
        /* Somos el último → borramos ficheros y semáforos del sistema */
        unlink(FILE_PIDS);
        unlink(FILE_TARGET);
        unlink(FILE_VOTES);
        sem_unlink(SEM_PIDS);
        sem_unlink(SEM_TARGET);
        sem_unlink(SEM_VOTES);
        sem_unlink(SEM_WINNER);
        printf("Last miner left. System cleaned.\n");
    }
    else
    {
        /* Reescribimos el fichero sin nuestro PID */
        FILE *t = fopen(FILE_PIDS, "w");
        if (t)
        {
            for (int i = 0; i < n; i++)
                fprintf(t, "%d\n", pids[i]);
            fclose(t);
        }
        printf("Miner %d exited system\n", my_pid);
        print_pids();
    }

    sem_post(sem_pids);
}

/* Lee todos los PIDs en un array; devuelve el número de PIDs leídos */
static int read_pids(pid_t *buf, int max)
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

/* ────────────────────────────────────────────────────────────
   GESTIÓN DEL TARGET
   ──────────────────────────────────────────────────────────── */
static long int read_target(void)
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

static void write_target(long int t)
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

/* ────────────────────────────────────────────────────────────
   FICHERO DE REGISTRO PERSONAL  <pid>.txt
   ──────────────────────────────────────────────────────────── */
static void log_round(int id, long int target, long int solution,
                      const char *votes_str, int yes, int total)
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

/* ────────────────────────────────────────────────────────────
   ESPERA NO ACTIVA CON sigsuspend
   Desbloquea solo la señal indicada y espera.
   ──────────────────────────────────────────────────────────── */
static void wait_for_SIGUSR1(void)
{
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);

    /* Bloqueamos SIGUSR1 ANTES de comprobar el flag */
    if (sigprocmask(SIG_BLOCK, &mask, &oldmask) == -1)
    {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }

    while (!flag_start_round)
    {
        /* sigsuspend restaura oldmask atómicamente → SIGUSR1 desbloqueada */
        if (sigsuspend(&oldmask) == -1 && errno != EINTR)
        {
            perror("sigsuspend");
            exit(EXIT_FAILURE);
        }
    }
    flag_start_round = 0;

    if (sigprocmask(SIG_SETMASK, &oldmask, NULL) == -1)
    {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
}

static void wait_for_SIGUSR2(void)
{
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);

    if (sigprocmask(SIG_BLOCK, &mask, &oldmask) == -1)
    {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }

    while (!flag_start_voting)
    {
        if (sigsuspend(&oldmask) == -1 && errno != EINTR)
        {
            perror("sigsuspend");
            exit(EXIT_FAILURE);
        }
    }
    flag_start_voting = 0;

    if (sigprocmask(SIG_SETMASK, &oldmask, NULL) == -1)
    {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
}

/* ────────────────────────────────────────────────────────────
   MINADO — estructura compartida entre hilos
   ──────────────────────────────────────────────────────────── */
typedef struct
{
    long int start;    /* inicio del rango de búsqueda */
    long int end;      /* fin del rango de búsqueda */
    long int target;   /* objetivo: buscar x tal que pow_hash(x) == target */
    long int solution; /* rellenado por el hilo que encuentra la solución */
    int found;         /* 1 si este hilo encontró solución */
} ThreadArg;

static void *mine_thread(void *arg)
{
    ThreadArg *a = (ThreadArg *)arg;

    for (long int x = a->start; x < a->end && !have_winner; x++)
    {
        if (pow_hash(x) == a->target)
        {
            /* Intentamos ser el ganador de este proceso de forma atómica. Si encontramos la solucion del hash intentamos pill */
            if (sem_trywait(sem_winner) == 0) /*Si semtrywait es 0 es que lo hemos pillado nosotros, */
            {
                have_winner = 1;
                winner_pid = my_pid; // Guarda quién ha ganado
                i_am_winner = 1;
                a->solution = x; // Guarda el número que es la solucion.
                a->found = 1;
            }
            /* Tanto si se nos declara ganador como si no, si no hemos podido entrar en el semaforo es que ya habia otro ganador */
            break;
        }
    }
    return NULL;
}

/* ────────────────────────────────────────────────────────────
   LANZAR HILOS DE MINADO
   Devuelve la solución encontrada (0 si no fuimos el ganador)
   ──────────────────────────────────────────────────────────── */
static long int run_mining(long int target)
{
    pthread_t *threads = malloc(n_threads * sizeof(pthread_t));
    ThreadArg *args = malloc(n_threads * sizeof(ThreadArg));
    if (!threads || !args)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    have_winner = 0;
    i_am_winner = 0;
    winner_pid = -1;

    /* Dividimos el espacio de búsqueda [0, POW_LIMIT) entre los hilos */
    long int range = POW_LIMIT / n_threads;
    for (int i = 0; i < n_threads; i++)
    {
        args[i].start = i * range;
        args[i].end = (i == n_threads - 1) ? POW_LIMIT : (i + 1) * range;
        args[i].target = target;
        args[i].solution = 0;
        args[i].found = 0;
        pthread_create(&threads[i], NULL, mine_thread, &args[i]);
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

/* ────────────────────────────────────────────────────────────
   ENVIAR SEÑAL A TODOS LOS MINEROS DEL FICHERO
   ──────────────────────────────────────────────────────────── */
static int broadcast_signal(int sig, pid_t *out_pids, int *out_n)
{
    pid_t pids[1024];
    int n = read_pids(pids, 1024);
    for (int i = 0; i < n; i++)
    {
        if (pids[i] != my_pid)
            kill(pids[i], sig);
    }
    if (out_pids && out_n)
    {
        memcpy(out_pids, pids, n * sizeof(pid_t));
        *out_n = n;
    }
    return n;
}

/* ────────────────────────────────────────────────────────────
   VOTACIÓN
   ──────────────────────────────────────────────────────────── */
static int check_solution(long int solution, long int target)
{
    return pow_hash(solution) == target;
}

static void cast_vote(long int solution, long int target)
{
    char vote = check_solution(solution, target) ? 'Y' : 'N';
    sem_wait(sem_votes);
    FILE *f = fopen(FILE_VOTES, "a");
    if (f)
    {
        fprintf(f, "%d %c\n", my_pid, vote);
        fclose(f);
    }
    sem_post(sem_votes);
}

/* Lee los votos; devuelve total, y escribe yes/no */
static int contar_votos(int *yes_out, int *no_out, char *cadena_votos, int max_cadena)
{
    int si = 0, no = 0, total = 0;
    cadena_votos[0] = '\0'; // Inicializamos la cadena vacía

    // 1. Bloqueamos el semáforo para leer la "urna" sin que nadie escriba
    sem_wait(sem_votes);

    FILE *fichero = fopen(FILE_VOTES, "r");
    if (fichero)
    {
        int pid_leido;
        char voto_leido;

        // 2. Leemos el fichero: cada línea tiene "PID VOTO" (ej: 1234 Y)
        while (fscanf(fichero, "%d %c\n", &pid_leido, &voto_leido) == 2)
        {
            total++;
            if (voto_leido == 'Y')
            {
                si++;
            }
            else
            {
                no++;
            }

            /*  Vamos montando el texto que se imprimira en consola con el resumen de los votos */
            char temporal[8];
            snprintf(temporal, sizeof(temporal), "%c ", voto_leido);

            // Concatenamos el voto a la cadena final sin pasarnos del tamaño
            strncat(cadena_votos, temporal, max_cadena - strlen(cadena_votos) - 1);
        }
        fclose(fichero);
    }

   
    sem_post(sem_votes);
    *yes_out = si;
    *no_out = no;

    return total;
}

/* ────────────────────────────────────────────────────────────
   LIMPIEZA Y SALIDA
   ──────────────────────────────────────────────────────────── */
static void cleanup_and_exit(int coins)
{
    (void)coins;
    remove_my_pid();
    sem_close(sem_pids);
    sem_close(sem_target);
    sem_close(sem_votes);
    sem_close(sem_winner);
    exit(EXIT_SUCCESS);
}

/* ────────────────────────────────────────────────────────────
   MAIN
   ──────────────────────────────────────────────────────────── */
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

    /*  Instalar manejadores */
    setup_signals();

    /*  Abrir semáforos */
    open_semaphores();

    /* Registrar en pids.pid */
    add_my_pid();

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

    /* ── PRIMER MINERO: inicializa el sistema ── */
    if (first_miner)
    {
        write_target(0);

        sem_wait(sem_votes);
        fclose(fopen(FILE_VOTES, "w"));
        sem_post(sem_votes);
        /* Enviar SIGUSR1 a todos (en este momento solo estamos nosotros,
           pero ponemos el flag para arrancarnos a nosotros mismos) */
        flag_start_round = 1;
        /* pequeña espera para que otros mineros que arranquen casi a la vez
           puedan registrarse antes de la primera ronda */
        sleep(1);
        broadcast_signal(SIGUSR1, NULL, NULL);
    }

    /* ────────────────────────────────────────────
       BUCLE PRINCIPAL DE RONDAS
       ──────────────────────────────────────────── */
    while (!flag_timeout)
    {

        /* Esperar SIGUSR1 (inicio de ronda) */
        if (!flag_start_round)
            wait_for_SIGUSR1();
        if (flag_timeout)
            break;
        flag_start_round = 0;
        round_id++;

        /* Comprobar que hay al menos 2 mineros */
        pid_t all_pids[1024];
        int n_miners;
        n_miners = read_pids(all_pids, 1024);
        if (n_miners < 2)
        {
            /* Solo hay un minero: esperamos a que llegue otro */
            printf("Miner %d: only 1 miner, waiting...\n", my_pid);
            fflush(stdout);
            wait_for_SIGUSR1();
            if (flag_timeout)
                break;
            flag_start_round = 0;
            n_miners = read_pids(all_pids, 1024);
        }

        /* Leer objetivo */
        long int target = read_target();

        /* ── FASE DE MINADO ── */
        long int solution = run_mining(target);

        if (flag_timeout)
            break;

        /* ── SOY GANADOR ── */
        if (i_am_winner)
        {
            /* Escribir solución en target.tgt */
            write_target(solution);

            /* Vaciar fichero de votos */
            sem_wait(sem_votes);
            fclose(fopen(FILE_VOTES, "w"));
            sem_post(sem_votes);

            /* Enviar SIGUSR2 a todos para arrancar votación */
            n_miners = read_pids(all_pids, 1024); /*abre el archivo pids.pid (donde todos se apuntaron al principio) y copia todos los números de proceso (PIDs) en un array llamado all_pids*/
                                                  /*De esa manera se hace la cuenta de cuantos mineros hay.*/
            for (int i = 0; i < n_miners; i++)
                if (all_pids[i] != my_pid)
                    kill(all_pids[i], SIGUSR2); /*Le manda la señal a los demos de que empeiza la votacion*/

            flag_start_voting = 1;
        }

        /* ── FASE DE VOTACIÓN (todos) ── */
        /*Si yo no soy el proceso ganador, el que encontró la solucion en la parte anterior me tengo que esperar hasta que marquen la señal.*/
        if (!flag_start_voting)
            wait_for_SIGUSR2();
        if (flag_timeout)
            break;
        flag_start_voting = 0;

        /* Leer la solución propuesta y votar */
        long int proposed;
        sem_wait(sem_target);
        FILE *ft = fopen(FILE_TARGET, "r"); /*Abrimos el archivo donde el ganador guarda la solucion */
        if (ft)
        {
            fscanf(ft, "%ld", &proposed); // lee la solucion
            fclose(ft);
        }
        sem_post(sem_target);

        cast_vote(proposed, target);

        /* ── RECUENTO (solo el ganador) ── */
        if (i_am_winner)
        {
            /* Esperar a que voten todos (con límite de intentos) */
            int v_esperados = n_miners;
            int intentos = 0, intentos_maximos = 50;
            int yes = 0, no = 0, total = 0;
            char votes_str[256] = "";
            
            do
            {
                usleep(100000); /* Espera 100 ms para dar tiempo a que los demás voten */
                total = contar_votos(&yes, &no, votes_str, sizeof(votes_str));
                intentos++;
            } while (total < v_esperados && intentos < intentos_maximos);

            int accepted = (yes >= no);
            printf("Winner %d => [ %s] => %s\n",
                   my_pid, votes_str,
                   accepted ? "Accepted" : "Rejected");
            fflush(stdout);

            if (accepted)
            {
                my_coins++;
                log_round(round_id, target, solution, votes_str, yes, total);
            }

            /* Preparar siguiente ronda */
            write_target(solution); /* el nuevo target es la solución actual */

            /* Restaurar sem_winner para la siguiente ronda */
            sem_post(sem_winner);

            /* Enviar SIGUSR1 a todos para la siguiente ronda */
            n_miners = read_pids(all_pids, 1024);
            if (n_miners >= 2)
            {
                for (int i = 0; i < n_miners; i++)
                    if (all_pids[i] != my_pid)
                        kill(all_pids[i], SIGUSR1);
                flag_start_round = 1; /* me incluyo a mí mismo */
            }

            i_am_winner = 0;
        }
        /* Los no-ganadores vuelven al inicio del bucle y esperan SIGUSR1 */
    }

    /* ── SALIDA ── */
    cleanup_and_exit(my_coins);
    return 0;
}