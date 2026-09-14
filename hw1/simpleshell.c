#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
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

    // printf("Args cnt: %d\n", *argumentCount);
}

// parse input/output redirection direction
// Including '<' for input redirection, '>' for output redirection, and '|' for pipe
#define INPUT_REDIRECTION '<'
#define OUTPUT_REDIRECTION '>'
#define PIPE '|'
int parseIODirection(char **args, int argumentCount){
    // the last argument is always NULL
    for(int i=0; i<argumentCount-1; i++){
        if(strcmp(args[i], "<") == 0){
            // handle input redirection
            return INPUT_REDIRECTION;
        }
        else if(strcmp(args[i], ">") == 0){
            // handle output redirection
            return OUTPUT_REDIRECTION;
        }
        else if(strcmp(args[i], "|") == 0){
            // handle pipe
            return PIPE;
        }
    }
    return 0; // no redirection or pipe found
}

void executeSingleCommand(char **args, int argumentCount){
    pid_t pid;

    bool isEndWithAnd = (strcmp(args[argumentCount-2], "&") == 0); // the real last argument is argumentCount-2 because the last one is filled with NULL

    if(isEndWithAnd){
        args[argumentCount-2] = NULL;
    }

    pid = fork();
    if (pid < 0) { /* error occurred */
        fprintf(stderr, "Fork Failed");
        exit(-1);
    }
    else if (pid == 0) { /* child process */
        if(!isEndWithAnd){
            execvp(args[0], args);

            // if execvp returns, it must have failed. we need to exit this child process
            perror("execvp");
            exit(EXIT_FAILURE);
        }

        // if end with '&'. the main shell process won't wait for it.
        // double fork to prevent zombie process and allow the main shell to continue without waiting
        else{
            pid_t pid2;
            pid2 = fork();

            if (pid2 < 0) { /* fork creation failed */
                fprintf(stderr, "Fork Failed");
                exit(-1);
            }
            else if (pid2 == 0){ /* grand child process */
                execvp(args[0], args);

                // if execvp returns, it must have failed. we need to exit this child process
                perror("execvp");
                exit(EXIT_FAILURE);
            }
            else {
                // the child process exit immediatly, leaving the grand child process as an orphan process.
                // acheiving the purpose of double-fork.
                exit(0);
            }
        }
    }
    else { /* parent process */
    /* parent will wait for the child to complete */
        waitpid(pid, NULL, 0);
    }
}

void executeCommand(char **args, int argumentCount){
    if(argumentCount <= 1) return;

    int IODirectionFlag = parseIODirection(args, argumentCount);
    
    switch(IODirectionFlag){
        case INPUT_REDIRECTION:
            // handle input redirection
            break;
        case OUTPUT_REDIRECTION:
            // handle output redirection
            break;
        case PIPE:
            // handle pipe
            break;
        default:
            executeSingleCommand(args, argumentCount);
            break;
    }
}

int main() {
    while(1){
        printf("> ");
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