#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

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

// args2 is expected to point to a file name for output redirection
void executeOutputRedirection(char **args, char **args2){
    pid_t pid = fork();

    if (pid < 0) { /* error occurred */
        fprintf(stderr, "Fork Failed");
        exit(-1);
    }
    else if (pid == 0) { /* child process */
        // open target output file
        int fd = open(args2[0], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0){
            perror("open file failed");
            exit(1);
        }

        // redirect standard output to the file descriptor
        dup2(fd, STDOUT_FILENO);

        // close the file descriptor, complete the redirection
        close(fd);

        // execute the command normally
        execvp(args[0], args);

        // if execvp returns, it must have failed. we need to exit this child process
        perror("execvp");
        exit(EXIT_FAILURE);
        
    }
    else { /* parent process */
    /* parent will wait for the child to complete */
        waitpid(pid, NULL, 0);
    }
}

void executeInputRedirection(char **args, char **args2){
    pid_t pid = fork();

    if (pid < 0) { /* error occurred */
        fprintf(stderr, "Fork Failed");
        exit(-1);
    }
    else if (pid == 0) { /* child process */
        // open target input file
        int fd = open(args2[0], O_RDONLY);
        if (fd < 0){
            perror("open file failed");
            exit(1);
        }

        // redirect standard input to the file descriptor
        dup2(fd, STDIN_FILENO);

        // close the file descriptor, complete the redirection
        close(fd);

        // execute the command normally
        execvp(args[0], args);

        // if execvp returns, it must have failed. we need to exit this child process
        perror("execvp");
        exit(EXIT_FAILURE);
        
    }
    else { /* parent process */
    /* parent will wait for the child to complete */
        waitpid(pid, NULL, 0);
    }
}

void executePipe(char **args, char **args2){
    pid_t pid, pid2;
    // create a pipe
    int p[2];

    if(pipe(p) == -1){
        perror("open pipe failed");
        exit(1);
    }

    // execute the first command
    pid = fork();
    if (pid < 0) { /* error occurred */
        fprintf(stderr, "Fork Failed");
        exit(-1);
    }
    else if (pid == 0) { /* child process */
        // write the output of the first command to pipe
        dup2(p[1], STDOUT_FILENO);
        execvp(args[0], args);

        // if execvp returns, it must have failed. we need to exit this child process
        perror("execvp");
        exit(EXIT_FAILURE);
    }
    else { /* parent process */
    /* parent will wait for the child to complete */
        waitpid(pid, NULL, 0);
        close(p[1]); // close write end of the pipe in the parent process
    }

    // execute the second command
    pid2 = fork();
    if (pid2 < 0) { /* error occurred */
        fprintf(stderr, "Fork Failed");
        exit(-1);
    }
    else if (pid2 == 0) { /* child process */
        // read the input of the second command from pipe
        dup2(p[0], STDIN_FILENO);
        execvp(args2[0], args2);

        // if execvp returns, it must have failed. we need to exit this child process
        perror("execvp");
        exit(EXIT_FAILURE);
    }
    else { /* parent process */
    /* parent will wait for the child to complete */
        waitpid(pid2, NULL, 0);
        close(p[0]); // close read end of the pipe in the parent process
    }
}

#define INPUT_REDIRECTION '<'
#define OUTPUT_REDIRECTION '>'
#define PIPE '|'
void executeCommand(char **args, int argumentCount){
    if(argumentCount <= 1) return;

    if(strcmp(args[0], "exit") == 0){
        exit(0);
    }

    // parse input/output redirection direction
    // Including '<' for input redirection, '>' for output redirection, and '|' for pipe
    int IODirectionFlag = 0;
    char **args2 = NULL;

    for(int i=0; i<argumentCount-1; i++){
        if(strcmp(args[i], "<") == 0){
            // handle input redirection
            IODirectionFlag = INPUT_REDIRECTION;
            args2 = &args[i+1];
            args[i] = NULL;
            break;
        }
        else if(strcmp(args[i], ">") == 0){
            // handle output redirection
            IODirectionFlag = OUTPUT_REDIRECTION;
            args2 = &args[i+1];
            args[i] = NULL;
            break;
        }
        else if(strcmp(args[i], "|") == 0){
            // handle pipe
            IODirectionFlag = PIPE;
            args2 = &args[i+1];
            args[i] = NULL;
            break;
        }
    }
    
    switch(IODirectionFlag){
        case INPUT_REDIRECTION:
            // handle input redirection
            executeInputRedirection(args, args2);
            break;
        case OUTPUT_REDIRECTION:
            // handle output redirection
            // args2 is expected to point to a file name
            executeOutputRedirection(args, args2);
            break;
        case PIPE:
            // handle pipe
            executePipe(args, args2);
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