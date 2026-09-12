#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define DEBUG

// args is passed by reference because we are assigning a new memory block to it within the function
// line is passed by reference because strtok modifies the original string
// argumentCount will be 1 more than actually argument. The last one will be filled with NULL.
void parseArgs(char** *args, char* *line, int *argumentCount){
    char *ptr = NULL, *arg = NULL;
    *argumentCount = 0;

    *args = (char **)malloc(sizeof(char *));
    arg = strtok_r(*line, " \t\n", &ptr);
    
    while(arg != NULL){
        *args = (char **)realloc(*args, ++(*argumentCount) * sizeof(char *));

        (*args)[*argumentCount-1] = NULL;
        (*args)[*argumentCount-1] = arg;

        arg = strtok_r(NULL, " \t\n", &ptr);
    }

    // make the argument array NULL terminated
    *args = (char **)realloc(*args, ++(*argumentCount) * sizeof(char *));
    (*args)[*argumentCount-1] = NULL;

    printf("Args cnt: %d\n", *argumentCount);
}

void executeCommand(char **args, int argumentCount){
    pid_t pid;

    if(argumentCount <= 1) return;

    bool isEndWithAnd = (strcmp(args[argumentCount-2], "&") == 0); // the real last argument is argumentCount-2 because the last one is filled with NULL
    printf("isEndWithAnd: %d\n", isEndWithAnd);

    if(isEndWithAnd){
        args[argumentCount-2] = NULL;
    }

    pid = fork();
    if (pid < 0) { /* error occurred */
        fprintf(stderr, "Fork Failed");
        exit(-1);
    }
    else if (pid == 0) { /* child process */
        execvp(args[0], args);

        // if execvp returns, it must have failed. we need to exit this child process
        perror("execvp");
        exit(EXIT_FAILURE);
    }
    else { /* parent process */
    /* parent will wait for the child to complete */
        if(!isEndWithAnd){
            printf("waiting\n");
            waitpid(pid, NULL, 0);
        }
    }
}

int main() {
    while(1){
        printf("\n> ");
        fflush(stdout);
        
        char *line = NULL;
        char **args = NULL;
        int argumentCount = 0;
        size_t len = 0;
        ssize_t read;

        // read command
        read = getline(&line, &len, stdin);
        if (read == -1) { /* EOF or error: stop the shell instead of parsing garbage */
            free(line);
            break;
        }

        // parse the command line into arguments
        parseArgs(&args, &line, &argumentCount);

        // execute the command
        executeCommand(args, argumentCount);
        
        // free the allocated memory
        free(args);
        free(line);
    }
    
    return 0;
}