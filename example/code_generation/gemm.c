/*
 * Copyright 2003-2013 Jeffrey K. Hollingsworth
 *
 * This file is part of Active Harmony.
 *
 * Active Harmony is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Active Harmony is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Active Harmony.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This is an example of an application that uses the code-server
 * to search for code variants of a naive matrix multiplication
 * implementation for optimally performing configurations.
 *
 * The code variants are generated by the CHiLL framework.
 *   http://ctop.cs.utah.edu/ctop/?page_id=21
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <mpi.h>
#include <dlfcn.h>
#include <sys/time.h>

#include "hsession.h"
#include "hclient.h"
#include "defaults.h"

#define SESSION_NAME      "gemm"
#define SHLIB_SYMBOL_NAME "gemm_"
#define DEFAULT_SO        "./gemm_default.so"
#define SEARCH_MAX 400
#define N 500

/* ISO C forbids conversion of object pointer to function pointer.  We
 * get around this by first casting to a word-length integer.  (LP64
 * compilers assumed).
 */
#define HACK_CAST(x) (code_t)(long)(x)

/* Function signature of the tuning target function produced by CHiLL. */
typedef void (*code_t)(void *, void *, void *, void *);

/*
 * Function Prototypes
 */
int    fetch_configuration(void);
int    check_convergence(hdesc_t *hdesc);
char * construct_so_filename(void);
int    update_so(const char *filename);
void   initialize_matrices(void);
int    check_code_correctness(void);
int    penalty_factor(void);
double calculate_performance(double raw_perf);
double timer(void);
int    dprint(const char *fmt, ...);
int    errprint(const char *fmt, ...);

/*
 * Global variable declarations.
 */
int debug = 1;
int rank = -1;
hdesc_t *hdesc = NULL;
int matrix_size = N;

/* Pointers to data loaded from shared libraries of generated code. */
void *flib_eval;
code_t code_so;

/* 
 * Matrix definitions.
 */
double A[N][N];
double B[N][N];
double C[N][N];
double C_TRUTH[N][N];

/* Parameters that will be modified by the Harmony server. */
long TI, TJ, TK, UI, UJ;

double time_start, time_end;
const char *new_code_path;

int main(int argc, char *argv[])
{
    hsession_t sess;
    const char *retval;
    char *ptr;
    char numbuf[12];
    char hostname[64];
    int harmony_connected;
    int node_count;
    double time_initial, time_current, time;
    double raw_perf, perf;
    int i, found_iter, harmonized;

    /* MPI Initialization */
    MPI_Init(&argc, &argv);
    time_initial = MPI_Wtime();

    /* Set the location where new code will arrive. */
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <new_code_dir>"
                " [KEY_1=VAL_1] .. [KEY_N=VAL_N]\n\n", argv[0]);
        MPI_Finalize();
        return -1;
    }
    new_code_path = argv[1];

    /* Get the rank and size of this MPI application. */
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &node_count);

    /* Retrieve for the host name */
    gethostname(hostname, sizeof(hostname));
    harmony_connected = 0;

    /* Initialize Harmony API. */
    if (rank == 0) {
        /* We are the master rank.  Establish a new Harmony tuning session. */
        hsession_init(&sess);
        snprintf(numbuf, sizeof(numbuf), "%d", node_count);

        if (hsession_name(&sess, SESSION_NAME)                        < 0 ||
            hsession_cfg(&sess, CFGKEY_CLIENT_COUNT, numbuf)          < 0 ||
            hsession_cfg(&sess, CFGKEY_SESSION_STRATEGY, "pro.so")    < 0 ||
            hsession_cfg(&sess, CFGKEY_SESSION_PLUGINS, "codegen.so") < 0)
        {
            errprint("Error during session configuration.\n");
            MPI_Abort(MPI_COMM_WORLD, -1);
        }

        if (hsession_int(&sess, "TI", 2, 500, 2) < 0 ||
            hsession_int(&sess, "TJ", 2, 500, 2) < 0 ||
            hsession_int(&sess, "TK", 2, 500, 2) < 0 ||
            hsession_int(&sess, "UI", 1,   8, 1) < 0 ||
            hsession_int(&sess, "UJ", 1,   8, 1) < 0)
        {
            errprint("Failed to define tuning session\n");
            MPI_Abort(MPI_COMM_WORLD, -1);
        }

        /* Process arguments last to give command-line parameters the
         * highest priority.
         */
        for (i = 2; i < argc; ++i) {
            ptr = strchr(argv[i], '=');
            if (!ptr) {
                fprintf(stderr, "Invalid parameter '%s'\n", argv[i]);
                MPI_Abort(MPI_COMM_WORLD, -1);
            }

            *(ptr++) = '\0';
            if (hsession_cfg(&sess, argv[i], ptr) < 0) {
                fprintf(stderr, "Failed to set config var %s\n", argv[i]);
                MPI_Abort(MPI_COMM_WORLD, -1);
            }
        }

        retval = hsession_launch(&sess, NULL, 0);
        if (retval) {
            errprint("Could not launch tuning session: %s\n", retval);
            MPI_Abort(MPI_COMM_WORLD, -1);
        }
    }

    /* Everybody should wait until a tuning session is created. */
    MPI_Barrier(MPI_COMM_WORLD);

    /* Tuning session has been established.  All nodes may now connect. */
    hdesc = harmony_init();
    if (hdesc == NULL) {
        errprint("Failed to initialize a Harmony session.\n");
        goto cleanup;
    }

    /* Associate local memory to the session's runtime tunable
     * parameters.  For example, these represent loop tiling and
     * unrolling factors.
     */
    if (harmony_bind_int(hdesc, "TI", &TI) < 0 ||
        harmony_bind_int(hdesc, "TJ", &TJ) < 0 ||
        harmony_bind_int(hdesc, "TK", &TK) < 0 ||
        harmony_bind_int(hdesc, "UI", &UI) < 0 ||
        harmony_bind_int(hdesc, "UJ", &UJ) < 0)
    {
        errprint("Error binding local memory to Harmony variables.\n");
        goto cleanup;
    }

    /* Connect to Harmony server and register ourselves as a client. */
    if (harmony_join(hdesc, NULL, 0, SESSION_NAME) < 0) {
        errprint("Could not join Harmony tuning session.\n");
        goto cleanup;
    }
    harmony_connected = 1;

    dprint("MPI node ready and searching for new shared objects in %s:%s\n",
           hostname, new_code_path);

    initialize_matrices();
    if (update_so(DEFAULT_SO) < 0) {
        errprint("Error loading default shared object.\n");
        goto cleanup;
    }
    code_so(&matrix_size, A, B, C_TRUTH);

    dprint("Entering main loop.\n");
    harmonized = 0;
    for (i = 1; i < SEARCH_MAX; ++i) {
        dprint("Begin iteration #%d\n", i);

        /* Retrieve a new point to test from the tuning session. */
        if (fetch_configuration() < 0)
            goto cleanup;

        initialize_matrices();

        /* Perform and measure the client application code. */
        time_start = timer();
        code_so(&matrix_size, A, B, C);
        time_end = timer();

        raw_perf = time_end - time_start;
        dprint("Application code took %lf seconds\n", raw_perf);

        check_code_correctness();

        perf = calculate_performance(raw_perf);
        dprint("TI:%ld, TJ:%ld, TK:%ld, UI:%ld, UJ:%ld = %lf\n",
               TI, TJ, TK, UI, UJ, perf);

        /* update the performance result */
        if (harmony_report(hdesc, perf) < 0) {
            errprint("Error reporting performance to server.\n");
            goto cleanup;
        }

        if (!harmonized) {
            harmonized = check_convergence(hdesc);
            if (harmonized < 0) {
                errprint("Error checking harmony convergence status.\n");
                goto cleanup;
            }

            if (harmonized) {
                /*
                 * Harmony server has converged, so make one final fetch
                 * to load the harmonized values, and disconnect from
                 * server.
                 */
                if (fetch_configuration() < 0)
                    goto cleanup;

                if (harmony_leave(hdesc) < 0) {
                    errprint("Error leaving tuning session.");
                    goto cleanup;
                }

                /* At this point, the application may continue its
                 * execution using the harmonized values without
                 * interference from the Harmony server.
                 */
                harmony_connected = 0;
                found_iter = i;
            }
        }
        MPI_Barrier(MPI_COMM_WORLD);
    }

    /* Print final book-keeping messages. */
    time_current = MPI_Wtime();
    time = time_current - time_initial;
    dprint("%.3f: machine=%s [# node_count=%i]\n",
           time, hostname, node_count);

    /* Leave the Harmony session, if needed. */
    if (harmony_connected) {
        if (harmony_leave(hdesc) < 0) {
            errprint("Error leaving Harmony session.\n");
            harmony_connected = 0;
            goto cleanup;
        }
    }
    else {
        dprint("Reached final harmonized values (TI:%ld, TJ:%ld, TK:%ld,"
               " UI:%ld, UJ:%ld) at iteration %d of %d\n",
               TI, TJ, TK, UI, UJ, found_iter, SEARCH_MAX);
    }
    MPI_Finalize();
    return 0;

  cleanup:
    if (harmony_connected) {
        if (harmony_leave(hdesc) < 0)
            errprint("Error disconnecting from Harmony server.\n");
    }
    harmony_fini(hdesc);
    return MPI_Abort(MPI_COMM_WORLD, -1);
}

double timer()
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) < 0)
        errprint("Error during gettimeofday()\n");

    return (tv.tv_sec + 1.0e-6 * tv.tv_usec);
}

/*
 * Contact tuning session to see if a new configuration is available.
 * If so, update the underlying shared object and function pointer.
 */
int fetch_configuration(void)
{
    int changed;

    changed = harmony_fetch(hdesc);
    if (changed == 1) {
        /* Harmony updated variable values.  Load a new shared object. */
        if (update_so(construct_so_filename()) < 0) {
            errprint("Could not load new code object.");
            return -1;
        }
    }
    else if (changed == 0) {
        /* Harmony was not yet ready with a point to test.  Use the default.*/
        if (update_so(DEFAULT_SO) < 0) {
            errprint("Error loading default shared object.\n");
            return -1;
        }
    }
    else {
        errprint("Error fetching new values from Harmony server.\n");
        return -1;
    }
    return 0;
}

/*
 * Check if the parameter space search has converged.
 * Only rank 0 communicates directly with the Harmony server.
 */
int check_convergence(hdesc_t *hdesc)
{
    int status;

    if (rank == 0) {
        dprint("Checking Harmony search status.\n");
        status = harmony_converged(hdesc);
    }
    else {
        dprint("Waiting to hear search status from rank 0.\n");
    }

    /* Broadcast the result of harmony_converged(). */
    MPI_Bcast(&status, 1, MPI_INT, 0, MPI_COMM_WORLD);
    return status;
}

/*
 * Construct the full pathname for the new code variant.
 */
char *construct_so_filename()
{
    static char fullpath[1024];

    snprintf(fullpath, sizeof(fullpath), "%s/gemm_%ld_%ld_%ld_%ld_%ld.so",
             new_code_path, TI, TJ, TK, UI, UJ);
    return fullpath;
}

/* Update the code we wish to test.
 * Loads function <symbol_name> from shared object <filename>,
 * and stores that address in code_ptr.
 */
int update_so(const char *filename)
{
    char *err_str;

    flib_eval = dlopen(filename, RTLD_LAZY);
    err_str = dlerror();
    if (err_str) {
        errprint("Error opening %s: %s\n", filename, err_str);
        return -1;
    }

    code_so = HACK_CAST(dlsym(flib_eval, SHLIB_SYMBOL_NAME));
    err_str = dlerror();
    if (err_str) {
        errprint("Error finding symbol " SHLIB_SYMBOL_NAME " in %s: %s\n",
                 filename, err_str);
        return -1;
    }

    dprint("Evaluation candidate updated.\n");
    return 0;
}

void initialize_matrices()
{
    int i,j;
    for (i=0; i<N; i++) {
        for (j=0; j<N; j++) {
            A[i][j] = i;
            B[i][j] = j;
            C[i][j] = 0.0;
        }
    }
}

int check_code_correctness()
{
    int i, j;
    for (i=0; i<N; i++) {
        for (j=0; j<N; j++) {
            if(C[i][j] != C_TRUTH[i][j]) {
                errprint("C_TRUTH[%d][%d]=%lf, C[%d][%d]=%lf don't match \n",
                         i, j, C_TRUTH[i][j], i, j, C[i][j]);
                return -1;
            }
        }
    }
    return 0;
}

/* Illustrates the use of a local penalization technique. */
int penalty_factor()
{
    int increment = 100;
    int return_val = 0;

    if (TI < TJ)
        return_val += increment;

    if (UI*UJ > 16)
        return_val = return_val + (3 * increment);

    return return_val;
}

double calculate_performance(double raw_perf)
{
    int result = (int)(raw_perf * 1000);
    return (double)(result + penalty_factor());
}

int dprint(const char *fmt, ...)
{
    va_list ap;
    int count;

    if (!debug)
        return 0;

    va_start(ap, fmt);
    fprintf(stderr, "[r%d] ", rank);
    count = vfprintf(stderr, fmt, ap);
    va_end(ap);

    return count;
}

int errprint(const char *fmt, ...)
{
    va_list ap;
    int count;

    va_start(ap, fmt);
    fprintf(stderr, "[r%d] ", rank);
    count = vfprintf(stderr, fmt, ap);
    va_end(ap);

    return count;
}
