/****************************************************************************/
/****************************************************************************/
/**                                                                        **/
/**                 TU München - Institut für Informatik                   **/
/**                                                                        **/
/** Copyright: Prof. Dr. Thomas Ludwig                                     **/
/**            Andreas C. Schmidt                                          **/
/**                                                                        **/
/** File:      partdiff-seq.c                                              **/
/**                                                                        **/
/** Purpose:   Partial differential equation solver for Gauß-Seidel and   **/
/**            Jacobi method.                                              **/
/**                                                                        **/
/****************************************************************************/
/****************************************************************************/

/* ************************************************************************ */
/* Include standard header file.                                            */
/* ************************************************************************ */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <malloc.h>
#include <sys/time.h>
#include <omp.h>
#include <pthread.h>

#include "partdiff-posix.h"

struct calculation_arguments
{
	uint64_t  N;              /* number of spaces between lines (lines=N+1)     */
	uint64_t  num_matrices;   /* number of matrices                             */
	double    h;              /* length of a space between two lines            */
	double    ***Matrix;      /* index matrix used for addressing M             */
	double    *M;             /* two matrices with real values                  */
};

struct calculation_results
{
	uint64_t  m;
	uint64_t  stat_iteration; /* number of current iteration                    */
	double    stat_precision; /* actual precision of all slaves in iteration    */
};

struct posstruct
{
	double** matrix_in;
	double** matrix_out;
	int chunk_start;
	int chunk_end;
	int term_iteration;
	int N;
	uint64_t inf_func;
	uint64_t termination;
	double fpisin;
	double pih;
	double* maxresiduum;
};

pthread_mutex_t maxres;
pthread_mutex_t mainready;

/* ************************************************************************ */
/* Global variables                                                         */
/* ************************************************************************ */

/* time measurement variables */
struct timeval start_time;       /* time when program started                      */
struct timeval comp_time;        /* time when calculation completed                */


/* ************************************************************************ */
/* initVariables: Initializes some global variables                         */
/* ************************************************************************ */
static
void
initVariables (struct calculation_arguments* arguments, struct calculation_results* results, struct options const* options)
{
	arguments->N = (options->interlines * 8) + 9 - 1;
	arguments->num_matrices = (options->method == METH_JACOBI) ? 2 : 1;
	arguments->h = 1.0 / arguments->N;

	results->m = 0;
	results->stat_iteration = 0;
	results->stat_precision = 0;
}

/* ************************************************************************ */
/* freeMatrices: frees memory for matrices                                  */
/* ************************************************************************ */
static
void
freeMatrices (struct calculation_arguments* arguments)
{
	uint64_t i;

	for (i = 0; i < arguments->num_matrices; i++)
	{
		free(arguments->Matrix[i]);
	}

	free(arguments->Matrix);
	free(arguments->M);
}

/* ************************************************************************ */
/* allocateMemory ()                                                        */
/* allocates memory and quits if there was a memory allocation problem      */
/* ************************************************************************ */
static
void*
allocateMemory (size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
	{
		printf("Speicherprobleme! (%" PRIu64 " Bytes angefordert)\n", size);
		exit(1);
	}

	return p;
}

/* ************************************************************************ */
/* allocateMatrices: allocates memory for matrices                          */
/* ************************************************************************ */
static
void
allocateMatrices (struct calculation_arguments* arguments)
{
	uint64_t i, j;

	uint64_t const N = arguments->N;

	arguments->M = allocateMemory(arguments->num_matrices * (N + 1) * (N + 1) * sizeof(double));
	arguments->Matrix = allocateMemory(arguments->num_matrices * sizeof(double**));

	for (i = 0; i < arguments->num_matrices; i++)
	{
		arguments->Matrix[i] = allocateMemory((N + 1) * sizeof(double*));

		for (j = 0; j <= N; j++)
		{
			arguments->Matrix[i][j] = arguments->M + (i * (N + 1) * (N + 1)) + (j * (N + 1));
		}
	}
}

/* ************************************************************************ */
/* initMatrices: Initialize matrix/matrices and some global variables       */
/* ************************************************************************ */
static
void
initMatrices (struct calculation_arguments* arguments, struct options const* options)
{
	uint64_t g, i, j;                                /*  local variables for loops   */

	uint64_t const N = arguments->N;
	double const h = arguments->h;
	double*** Matrix = arguments->Matrix;

	/* initialize matrix/matrices with zeros */
	for (g = 0; g < arguments->num_matrices; g++)
	{
		for (i = 0; i <= N; i++)
		{
			for (j = 0; j <= N; j++)
			{
				Matrix[g][i][j] = 0.0;
			}
		}
	}

	/* initialize borders, depending on function (function 2: nothing to do) */
	if (options->inf_func == FUNC_F0)
	{
		for (g = 0; g < arguments->num_matrices; g++)
		{
			for (i = 0; i <= N; i++)
			{
				Matrix[g][i][0] = 1.0 - (h * i);
				Matrix[g][i][N] = h * i;
				Matrix[g][0][i] = 1.0 - (h * i);
				Matrix[g][N][i] = h * i;
			}

			Matrix[g][N][0] = 0.0;
			Matrix[g][0][N] = 0.0;
		}
	}
}

void *posixcalc(void *arg)
{
	struct posstruct argstruct = *(struct posstruct *)arg;

	int const N = argstruct.N;

	int i, j;                                   /* local variables for loops */
	double residuum;                            /* residuum of current iteration */
	double star;                                /* four times center value minus 4 neigh.b values */
	double maxresiduum = *argstruct.maxresiduum;

	/* over all rows */
	//  for (i = 1; i < N; i++)
	for (i = argstruct.chunk_start; i < argstruct.chunk_end; i++)
	{
		double fpisin_i = 0.0;

		if (argstruct.inf_func == FUNC_FPISIN)
		{
			fpisin_i = argstruct.fpisin * sin(argstruct.pih * (double)i);
		}
		/* over all columns */
		for (j = 1; j < N; j++)
		{
			star = 0.25 * (argstruct.matrix_in[i+1][j] + argstruct.matrix_in[i][j-1] + argstruct.matrix_in[i][j+1] + argstruct.matrix_in[i-1][j]);

			if (argstruct.inf_func == FUNC_FPISIN)
			{
				star += fpisin_i * sin(argstruct.pih * (double)j);
			}

			if (argstruct.termination == TERM_PREC || argstruct.term_iteration == 1)
			{
				residuum = argstruct.matrix_in[i][j] - star;
				residuum = (residuum < 0) ? -residuum : residuum;
				pthread_mutex_lock(&maxres);
				maxresiduum = (residuum < maxresiduum) ? maxresiduum : residuum;
				pthread_mutex_unlock(&maxres);
			}

			argstruct.matrix_out[i][j] = star;
		}
	}
	pthread_mutex_lock(&mainready);
	pthread_mutex_unlock(&mainready);
	//Das hier ist eine hässliche Möglichkeit, dass kein Thread fertig wird, bevor ein anderer beginnt.
	pthread_exit((void*) arg);
}

/* ************************************************************************ */
/* calculate: solves the equation                                           */
/* ************************************************************************ */
static
void
calculate (struct calculation_arguments const* arguments, struct calculation_results* results, struct options const* options)
{

	int m1, m2;                                 /* used as indices for old and new matrices */
	                         										/* maximum residuum value of a slave in iteration */
	pthread_mutex_init(&maxres,NULL);
	pthread_mutex_init(&mainready,NULL);
	double const h = arguments->h;
	int const N = arguments->N;

	double pih = 0.0;
	double fpisin = 0.0;

	int term_iteration = options->term_iteration;

	/* initialize m1 and m2 depending on algorithm */
	if (options->method == METH_JACOBI)
	{
		m1 = 0;
		m2 = 1;
	}
	else
	{
		m1 = 0;
		m2 = 0;
	}

	if (options->inf_func == FUNC_FPISIN)
	{
		pih = PI * h;
		fpisin = 0.25 * TWO_PI_SQUARE * h * h;
	}

	while (term_iteration > 0)
	{
		double *maxresiduum = malloc(sizeof(double));
		*maxresiduum = 0;

		pthread_t thread[options->number];
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

		int k, rc;
		void* status;
		int chunk_start = 1;
		int chunk_end;

		pthread_mutex_lock(&mainready);
		for(k = 0; k < options->number; k++)
		{
			struct posstruct *argstructptr = malloc(sizeof(struct posstruct));
			struct posstruct argstruct = *argstructptr;
			argstruct.matrix_in = arguments->Matrix[m2];
			argstruct.matrix_out = arguments->Matrix[m1];
			argstruct.N = arguments->N;
			argstruct.inf_func = options->inf_func;
			argstruct.termination = options->termination;
			argstruct.term_iteration = term_iteration;
			argstruct.pih = pih;
			argstruct.fpisin = fpisin;
			argstruct.maxresiduum = maxresiduum;

			argstruct.chunk_start = chunk_start;
			int chunksize = N / options->number;
			int rest = N - N * chunksize;
			if (k < rest)
			{
				chunksize++;
			}
			chunk_start = chunk_start + chunksize;
			chunk_end = chunk_start - 1;

			argstruct.chunk_end = chunk_end;
			rc = pthread_create(&thread[k], &attr, posixcalc, &argstruct);
			//folgendes Problem entsteht. Wenn die einzelnen Threads lesend auf gleiche Matrixfelder zugreifen wollen,
			//gibt es Speicherzugriffsfehler.
			if(rc)
			{
				printf("ERROR; return code from pthread_create is %d\n", rc);
				exit(-1);
			}
		}
		pthread_mutex_unlock(&mainready);

		for(k = 0; k < options->number; k++)
		{
			rc = pthread_join(thread[k],&status);
			if(rc)
			{
				printf("ERROR; return code from pthread_create is %d\n", rc);
				exit(-1);
			}
		}
		pthread_attr_destroy(&attr);


		results->stat_iteration++;
		results->stat_precision = *maxresiduum;

		/* exchange m1 and m2 */
		int i = m1;
		m1 = m2;
		m2 = i;

		/* check for stopping calculation depending on termination method */
		if (options->termination == TERM_PREC)
		{
			if (*maxresiduum < options->term_precision)
			{
				term_iteration = 0;
			}
		}
		else if (options->termination == TERM_ITER)
		{
			term_iteration--;
		}
	}
	results->m = m2;
}

/* ************************************************************************ */
/*  displayStatistics: displays some statistics about the calculation       */
/* ************************************************************************ */
static
void
displayStatistics (struct calculation_arguments const* arguments, struct calculation_results const* results, struct options const* options)
{
	int N = arguments->N;
	double time = (comp_time.tv_sec - start_time.tv_sec) + (comp_time.tv_usec - start_time.tv_usec) * 1e-6;

	printf("Berechnungszeit:    %f s \n", time);
	printf("Speicherbedarf:     %f MiB\n", (N + 1) * (N + 1) * sizeof(double) * arguments->num_matrices / 1024.0 / 1024.0);
	printf("Berechnungsmethode: ");

	if (options->method == METH_GAUSS_SEIDEL)
	{
		printf("Gauß-Seidel");
	}
	else if (options->method == METH_JACOBI)
	{
		printf("Jacobi");
	}

	printf("\n");
	printf("Interlines:         %" PRIu64 "\n",options->interlines);
	printf("Stoerfunktion:      ");

	if (options->inf_func == FUNC_F0)
	{
		printf("f(x,y) = 0");
	}
	else if (options->inf_func == FUNC_FPISIN)
	{
		printf("f(x,y) = 2pi^2*sin(pi*x)sin(pi*y)");
	}

	printf("\n");
	printf("Terminierung:       ");

	if (options->termination == TERM_PREC)
	{
		printf("Hinreichende Genaugkeit");
	}
	else if (options->termination == TERM_ITER)
	{
		printf("Anzahl der Iterationen");
	}

	printf("\n");
	printf("Anzahl Iterationen: %" PRIu64 "\n", results->stat_iteration);
	printf("Norm des Fehlers:   %e\n", results->stat_precision);
	printf("\n");
}

/****************************************************************************/
/** Beschreibung der Funktion DisplayMatrix:                               **/
/**                                                                        **/
/** Die Funktion DisplayMatrix gibt eine Matrix                            **/
/** in einer "ubersichtlichen Art und Weise auf die Standardausgabe aus.   **/
/**                                                                        **/
/** Die "Ubersichtlichkeit wird erreicht, indem nur ein Teil der Matrix    **/
/** ausgegeben wird. Aus der Matrix werden die Randzeilen/-spalten sowie   **/
/** sieben Zwischenzeilen ausgegeben.                                      **/
/****************************************************************************/
static
void
DisplayMatrix (struct calculation_arguments* arguments, struct calculation_results* results, struct options* options)
{
	int x, y;

	double** Matrix = arguments->Matrix[results->m];

	int const interlines = options->interlines;

	printf("Matrix:\n");

	for (y = 0; y < 9; y++)
	{
		for (x = 0; x < 9; x++)
		{
			printf ("%7.4f", Matrix[y * (interlines + 1)][x * (interlines + 1)]);
		}

		printf ("\n");
	}

	fflush (stdout);
}

/* ************************************************************************ */
/*  main                                                                    */
/* ************************************************************************ */
int
main (int argc, char** argv)
{
	struct options options;
	struct calculation_arguments arguments;
	struct calculation_results results;

	AskParams(&options, argc, argv);

	initVariables(&arguments, &results, &options);

	allocateMatrices(&arguments);
	initMatrices(&arguments, &options);

	gettimeofday(&start_time, NULL);
	calculate(&arguments, &results, &options);
	gettimeofday(&comp_time, NULL);

	displayStatistics(&arguments, &results, &options);
	DisplayMatrix(&arguments, &results, &options);

	freeMatrices(&arguments);

	pthread_exit(NULL);
	return 0;
}
