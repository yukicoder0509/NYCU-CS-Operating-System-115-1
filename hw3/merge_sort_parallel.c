#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <sys/time.h>

// Task type for thread
const int SORT = 0;
const int MERGE = 1;

// Task
typedef struct Task {
    int type;
    int section; // the node id in merge sort tree
    int offset;
    int len;
    int mid; // Only used for merge tasks
} Task;

// Global variable
int arrSource[1000005], arr[1000005];

// Treat merge sort process as a tree, the doneSections[i] means whether the node i in the tree is done.
// This can indicate whether two sub sequences are ready to be merged.
int doneSections[16]={0}; 
/*
        1
      /   \
    2       3
   / \     / \
  4   5   6   7
 / \ / \ / \ / \
8  9 10 11 12 13 14 15

*/

Task jobQueue[100], jobList[16];
bool jobDispatched[16] = {0};

int jobCount = 0;
sem_t jobCount_sem, jobDone_sem;
pthread_mutex_t jobQueue_mutex;
int N;

bool DONE = false;

int bubble_sort(int arr[], int N) {
    for (int i = 0; i < N; i++){
        for(int j = 0; j < N - 1; j++){
            if(arr[j] > arr[j+1]){
                int tmp = arr[j];
                arr[j] = arr[j+1];
                arr[j+1] = tmp;
            }
        }
    }
}

int merge(int arr[], int left, int mid, int right) {
    int mergedArr[right - left + 1];
    int i = left, j = mid + 1;

    for(int k = 0; k < right - left + 1; k++){
        if(i <= mid && j <= right){
            if(arr[i] <= arr[j]){
                mergedArr[k] = arr[i];
                i++;
            } else {
                mergedArr[k] = arr[j];
                j++;
            }
        }
        else if(i <= mid){
            mergedArr[k] = arr[i];
            i++;
        }
        else if(j <= right){
            mergedArr[k] = arr[j];
            j++;
        }
    }

    for(int k = 0; k < right - left + 1; k++){
        arr[left + k] = mergedArr[k];
    }
}

void createMergeSortJobs(int idx){
    if(idx >= 8){
        Task sortTask;
        sortTask.type = SORT;
        sortTask.section = idx;
        sortTask.offset = (idx - 8) * (N / 8);
        if(idx == 15)
            sortTask.len = N - (N / 8) * 7;
        else
            sortTask.len = N / 8;
        jobList[idx] = sortTask;
        
        return;
    }

    createMergeSortJobs(idx * 2);
    createMergeSortJobs(idx * 2 + 1);

    Task mergeTask;
    mergeTask.type = MERGE;
    mergeTask.section = idx;
    mergeTask.offset = jobList[idx * 2].offset;
    mergeTask.mid = jobList[idx * 2].offset + jobList[idx * 2].len - 1;
    mergeTask.len = jobList[idx * 2].len + jobList[idx * 2 + 1].len;
    jobList[idx] = mergeTask;
}

// Execute task
void executeTask(Task* task){
    // printf("Executing task of type %d with offset %d and length %d\n", task->type, task->offset, task->len);
    if (task->type == SORT){
        bubble_sort(arr + task->offset, task->len);
        return;
    }
    else if (task->type == MERGE){
        merge(arr, task->offset, task->mid, task->offset + task->len - 1);
        return;
    }
    else{
        return;
    }
    // printf("Finished executing task of type %d with offset %d and length %d\n", task->type, task->offset, task->len);
}

// Get job loop
void* getJobFromQueue(void *args){
    // Implementation for getting a job from the job queue
    while (1) {
        if (DONE) {
            break;
        }

        sem_wait(&jobCount_sem); // wait for the job count to be greater than 0

        if (DONE) {
            break;
        }

        // printf("Getting job from queue\n");

        pthread_mutex_lock(&jobQueue_mutex);

        Task task = jobQueue[0];
        for(int i=0;i<jobCount-1;i++){
            jobQueue[i] = jobQueue[i+1];
        }
        jobCount--;

        pthread_mutex_unlock(&jobQueue_mutex);

        // printf("Got job from queue: type %d, offset %d, length %d\n", task.type, task.offset, task.len);

        executeTask(&task);

        // mark the section as done
        doneSections[task.section] = true;

        sem_post(&jobDone_sem);
    }
    return NULL;
}

// Submit job to queue
void submitJobToQueue(Task* task){
    // printf("Submitting job to queue: type %d, offset %d, length %d\n", task->type, task->offset, task->len);
    pthread_mutex_lock(&jobQueue_mutex);

    jobQueue[jobCount++] = *task;

    pthread_mutex_unlock(&jobQueue_mutex);

    sem_post(&jobCount_sem);
}

// Parallelized merge sort
void mergeSort_parallel(int n_threads){
    sem_init(&jobCount_sem, 0, 0);
    sem_init(&jobDone_sem, 0, 0);
    pthread_mutex_init(&jobQueue_mutex, NULL);

    // create worker threads
    int threadCount = n_threads;
    pthread_t threads[threadCount];

    for(int i = 0; i < threadCount; i++){
        pthread_create(&threads[i], NULL, getJobFromQueue, NULL);
    }

    // create sort tasks
    createMergeSortJobs(1);

    // devide array into 8 subarray to sort
    for (int i = 8; i < 16; i++){
        jobDispatched[i] = true;
        submitJobToQueue(&jobList[i]);
    }

    // total merge sort contain 15 tasks (8 initial sorts + 7 merges)
    // wake up main thread for each completed job
    for(int i = 0; i < 15; i++){
        sem_wait(&jobDone_sem);
        // check if there are section to merge
        for(int j=2; j<16; j+=2){
            if(doneSections[j] && doneSections[j+1] && !doneSections[j/2] && !jobDispatched[j/2]){
                // dispatch merge task
                jobDispatched[j/2] = true;
                submitJobToQueue(&jobList[j/2]);
            }
        }
    }

    // printf("All jobs done\n");

    DONE = true;
    for(int i = 0; i < threadCount; i++){
        sem_post(&jobCount_sem); // wake up any waiting threads so they can exit
    }
    
    for(int i = 0; i < threadCount; i++){
        pthread_join(threads[i], NULL);
    }
   
    sem_destroy(&jobCount_sem);
    sem_destroy(&jobDone_sem);
    pthread_mutex_destroy(&jobQueue_mutex);
}

void writeOutput(const char* filename){
    FILE *output;
    output = fopen(filename, "w");
    if (output ==  NULL){
        perror("Failed to create output file");
        return;
    }

    for (int i=0; i<N; i++){
        fprintf(output, "%d ", arr[i]);
    }

    fclose(output);
}


int main() {
    // Open file
    FILE *input;
    input = fopen("input.txt", "r");
    if (input == NULL) {
        perror("Failed to open input file");
        return 1;
    }

    // Read from file
    fscanf(input, "%d", &N);
    
    for (int i = 0; i < N; i++) {
        fscanf(input, "%d", &arrSource[i]);
        arr[i] = arrSource[i];
    }

    fclose(input);

    char output_filename[20];
    struct timeval start, end;

    for (int n_worker = 1; n_worker <= 8; n_worker++){
        // reset global variables
        for (int i = 0; i < N; i++) {
            arr[i] = arrSource[i];
        }

        DONE = false;
        for(int i = 0; i < 16; i++){
            doneSections[i] = false;
            jobDispatched[i] = false;
        }

        gettimeofday(&start, 0);

        mergeSort_parallel(n_worker); // You can change the number of threads as needed
        
        gettimeofday(&end, 0);

        long seconds = end.tv_sec - start.tv_sec;
        long microseconds = end.tv_usec - start.tv_usec;
        double elapsed = seconds + microseconds*1e-6;
        printf("worker thread #%d, elapsed %.6f ms\n", n_worker, elapsed*1000);

        // Write output file
        snprintf(output_filename, sizeof(output_filename), "output_%d.txt", n_worker);
        writeOutput(output_filename);
    }
}