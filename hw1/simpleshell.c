#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define DEBUG

// args is passed by reference because we are assigning a new memory block to it within the function
// line is passed by reference because strtok modifies the original string
void parseArgs(char** *args, char* *line){
    char *ptr = NULL, *arg = NULL;
    int argumentCount = 0;

    *args = (char **)malloc(sizeof(char *));
    arg = strtok_r(*line, " \t\n", &ptr);
    
    while(arg != NULL){
        *args = (char **)realloc(*args, ++argumentCount * sizeof(char *));

        (*args)[argumentCount-1] = NULL;
        (*args)[argumentCount-1] = arg;

        arg = strtok_r(NULL, " \t\n", &ptr);
    }
}

int executeCommand(char **args){
    pid_t pid;

    pid = fork();
    if (pid < 0) { /* error occurred */
        fprintf(stderr, "Fork Failed");
        exit(-1);
    }
    else if (pid == 0) { /* child process */
        execvp(args[0], args);
    }
    else { /* parent process */
    /* parent will wait for the child to complete */
        wait (NULL);
    }
}

int main() {
    while(1){
        printf("\n> ");
        
        char *line = NULL;
        char **args = NULL;
        size_t len = 0;
        ssize_t read;

        // read command
        read = getline(&line, &len, stdin);
        
        // parse the command line into arguments
        parseArgs(&args, &line);

        // execute the command
        executeCommand(args);
        
        // free the allocated memory
        free(args);
        free(line);
    }
    
    return 0;
}