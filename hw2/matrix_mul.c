#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>

int DIM, shmid;
int *shmaddr;

void gen_2d_matrix(int dim, unsigned int **matrix) {
    // printf("generating 2D matrix...\n");
    for (int i = 0; i < dim; i++) {
        for (int j = 0; j < dim; j++) {
            // printf("matrix[%d][%d] = %d\n", i, j, i * dim + j);
            matrix[i][j] = i * dim + j;
        }
    }
}

void matmul(unsigned int **matrixA, int rowA, int colA, unsigned int **matrixB, int rowB, int colB, unsigned int **matrixC) {
    if (colA != rowB) {
        printf("Incompatible matrix dimensions for multiplication.\n");
        return;
    }

    // printf("performing matrix multiplication...\n");
    for (int i=0;i<rowA;i++) {
        for (int j=0;j<colB;j++){
            matrixC[i][j] = 0;
            for (int k=0;k<colA;k++){
                matrixC[i][j] += matrixA[i][k] * matrixB[k][j];
            }
        }
    }
}

void matmul_parallel(unsigned int **matrixA, int rowA, int colA, unsigned int **matrixB, int rowB, int colB, unsigned int **matrixC, int num_processes) {
    pid_t pids[num_processes];
    
    for(int i = 0; i < num_processes; i++) {
        // printf("Forking process %d\n", i);
        pid_t pid = fork();
        pids[i] = pid;

        int rows = rowA / num_processes; // rows get process in each child process

        if (pid < 0){
            printf("fork error:%s\n", strerror(errno));
            return;
        }
        else if (pid == 0) { // Child process
            // "matrix" stored pointer to a row, so offset is measured in rows.

            if(i < num_processes -1){
                matmul(matrixA + i * rows, rows, colA, matrixB, rowB, colB, matrixC + i * rows);
            } else {
                // tail processing
                matmul(matrixA + i * rows, rowA - i * rows, colA, matrixB, rowB, colB, matrixC + i * rows);
            }
            
            shmdt(shmaddr);
            _exit(0);
        }
    }

    for (int i = 0; i < num_processes; i++) {
        waitpid(pids[i], NULL, 0);
    }
}

void print_2d_matrix(int dim, unsigned int **matrix) {
    // printf("printing 2D matrix...\n");
    for (int i = 0; i < dim; i++) {
        for (int j = 0; j < dim; j++) {
            printf("%d ", matrix[i][j]);
        }
        printf("\n");
    }
}

int check_sum(int dim, unsigned int **matrix) {
    int sum = 0;
    for (int i = 0; i < dim; i++) {
        for (int j = 0; j < dim; j++) {
            sum += matrix[i][j];
        }
    }
    return sum;
}

int main() {
    printf("Input the matrix dimension: ");
    scanf("%d", &DIM);

    // Create shared memory.
    shmid = shmget(IPC_PRIVATE, sizeof(int) * 3 * DIM * DIM, 0600);
    if(shmid < 0){
        printf("shmget error:%s\n", strerror(errno));
        return -1;
    }

    // Attach shared memory to the process's address space.
    shmaddr = (int *)shmat(shmid, NULL, 0);
    if (shmaddr == (int *)-1) {
        printf("shmat error:%s\n", strerror(errno));
        return -1;
    }

    unsigned int **matrixA, **matrixB, **matrixC;
    matrixA = (unsigned int **)malloc(DIM * sizeof(unsigned int *));
    matrixB = (unsigned int **)malloc(DIM * sizeof(unsigned int *));
    matrixC = (unsigned int **)malloc(DIM * sizeof(unsigned int *));
    for (int i = 0; i < DIM; i++) {
        matrixA[i] = shmaddr + 0 * DIM * DIM + i * DIM;
        matrixB[i] = shmaddr + 1 * DIM * DIM + i * DIM;
        matrixC[i] = shmaddr + 2 * DIM * DIM + i * DIM;
    }

    // Generate matrices
    gen_2d_matrix(DIM, matrixA);
    gen_2d_matrix(DIM, matrixB);

    // Perform matrix multiplication
    for(int i = 1; i <= 16; i++){
        printf("Multiplying matrices using %d process\n", i);

        struct timeval start, end;
        gettimeofday(&start, 0);

        matmul_parallel(matrixA, DIM, DIM, matrixB, DIM, DIM, matrixC, i);

        gettimeofday(&end, 0);
        int sec = end.tv_sec - start.tv_sec;
        int usec = end.tv_usec - start.tv_usec;

        // Check the sum of the resulting matrix.
        int sum = check_sum(DIM, matrixC);

        printf("Elapsed time: %f sec, Checksum: %d\n", sec + (usec / 1000000.0), sum);
    }

    // Perform matrix multiplication again in the main process.
    // matmul(matrixA, DIM, DIM, matrixB, DIM, DIM, matrixC);
    // printf("Checksum of matrixC after main process multiplication: %d\n", check_sum(DIM, matrixC));

    // Detach and remove shared memory.
    shmdt(shmaddr);
    shmctl(shmid, IPC_RMID, NULL);

    return 0;
}