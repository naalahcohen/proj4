// #include <cerrno>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>  
#include <sys/wait.h> 
#include "process.h"


typedef struct DirNode {
    char *directory;
    struct DirNode *next;
} DirNode;

DirNode *dirStack = NULL;
int last_exit_status = 0; 

void printStack(DirNode *dirStack) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s", cwd);
    }

    DirNode *current = dirStack;
    while (current) {
        printf(" %s", current->directory);
        current = current->next;
    }
    printf("\n");
}

//getting every local varibale 
void getlocalvar(const CMD *cmdList) {
    for(int i = 0; i < cmdList->nLocal; i++) {
        setenv(cmdList->locVar[i], cmdList->locVal[i], 1);
    }
}

void redirection(const CMD *cmdList){
    //Redirect stdin to file
    if(cmdList->fromType == RED_IN){
        int old_opened = open(cmdList->fromFile, O_RDONLY); //file descriptor 
        if(old_opened == -1){
            perror("not valid file descriptor"); 
            exit(errno);
        }
        dup2(old_opened,STDIN_FILENO);
        close(old_opened);
    }
    else if (cmdList->toType == RED_OUT) {
        int fd = open(cmdList->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);  
        if (fd == -1) {
            perror("Error opening output file for redirection");
            exit(errno);
        }
        dup2(fd, STDOUT_FILENO);  
        close(fd);
    }
    else if (cmdList->toType == RED_OUT_APP) {
        int fd = open(cmdList->toFile, O_WRONLY | O_CREAT | O_APPEND, 0644);  
        if (fd == -1) {
            perror("Error opening output file for appending");
            exit(errno);
        }
        dup2(fd, STDOUT_FILENO);  
        close(fd);
    }
    else if (cmdList->fromType == RED_IN_HERE) {
        char tempFilePath[] = "/tmp/hereXXXXXX";
        int fd = mkstemp(tempFilePath);
        if (fd == -1) {
            perror("Error creating temporary file for HERE document");
            exit(errno);
        }

        if (write(fd, cmdList->fromFile, strlen(cmdList->fromFile)) == -1) {
            perror("Error writing to temporary HERE document file");
            close(fd);
            exit(errno);
        }

        if (lseek(fd, 0, SEEK_SET) == -1) {
            perror("Error seeking to start of HERE document file");
            close(fd);
            exit(errno);
        }

        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("Error redirecting stdin for HERE document");
            close(fd);
            exit(errno);
        }
        close(fd);
        unlink(tempFilePath);
}

}
//either built in simple command 
//not built in simple command 

int builtin_simplecase(const CMD *cmdList) {
    if (strcmp(cmdList->argv[0], "cd") == 0) {
        if (cmdList->argc > 2) {
            fprintf(stderr, "cd: too many arguments\n");
            last_exit_status = 1;
            return 1;
        }
        if (cmdList->argv[1] == NULL) {
            const char *home = getenv("HOME");
            if (home == NULL) {
                fprintf(stderr, "cd: HOME not set\n");
                last_exit_status = 1;
                return 1;
            }
            if (chdir(home) != 0) {
                fprintf(stderr, "directory not changed: %s\n", strerror(errno));
                last_exit_status = 1;
                return 1;
            }
            last_exit_status = 0;
            return 0;
        }
        if (chdir(cmdList->argv[1]) != 0) {
            fprintf(stderr, "directory not changed: %s\n", strerror(errno));
            last_exit_status = 1;
            return 1;
        }
        last_exit_status = 0;
        return 0;
    } 
    else if (strcmp(cmdList->argv[0], "pushd") == 0) {
        if (cmdList->argv[1] == NULL) {
            fprintf(stderr, "pushd: directory argument required\n");
            return 1;
        }

        char cwd[PATH_MAX];
        if (getcwd(cwd, PATH_MAX) == NULL) {
            perror("pushd: getcwd failed");
            return 1;
        }

        if (chdir(cmdList->argv[1]) != 0) {
            perror("pushd: chdir failed");
            return 1;
        }

        DirNode *newNode = malloc(sizeof(DirNode));
        if (!newNode) {
            perror("pushd: malloc failed");
            chdir(cwd); // Restore original directory
            return 1;
        }

        newNode->directory = strdup(cwd);
        if (!newNode->directory) {
            perror("pushd: strdup failed");
            free(newNode);
            chdir(cwd);
            return 1;
        }

        newNode->next = dirStack;
        dirStack = newNode;
        printStack(dirStack);
        return 0;
    } 
    else if (strcmp(cmdList->argv[0], "popd") == 0) {
        if (dirStack == NULL) {
            fprintf(stderr, "popd: directory stack empty\n");
            return 1;
        }

        DirNode *top = dirStack;
        if (chdir(top->directory) != 0) {
            perror("popd: chdir failed");
            return 1;
        }

        dirStack = dirStack->next;
        free(top->directory);
        free(top);
        printStack(dirStack);
        return 0;
    }
    last_exit_status = 1;  // Not a recognized built-in command
    return 1;
}

int simple_case(const CMD *cmdList) {
    if (strcmp(cmdList->argv[0], "cd") == 0 ||
        strcmp(cmdList->argv[0], "pushd") == 0 ||
        strcmp(cmdList->argv[0], "popd") == 0) {
        int status = builtin_simplecase(cmdList);
        last_exit_status = status;  // Update last_exit_status
        return status;
    }
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed");
        last_exit_status = 1;
        return 1;
    }

    if (pid == 0) {  // Child process
        redirection(cmdList);
        execvp(cmdList->argv[0], cmdList->argv);
        perror("execvp failed");
        exit(127);
    }

    // Parent process
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid failed");
        last_exit_status = 1;
        return 1;
    }

    if (WIFEXITED(status)) {
        last_exit_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        last_exit_status = 128 + WTERMSIG(status);
    } else {
        last_exit_status = 1;
    }

    return last_exit_status;
}

int process(const CMD *cmdList) {
    getlocalvar(cmdList);
    if (cmdList->type == SIMPLE) {
        return simple_case(cmdList);
    }
    return 0;
}
// int pipe_case (const CMD *cmdList){

// }

// int process (const CMD *cmdList){
//     getlocalvar(cmdList); 
//     if (cmdList->type == SIMPLE) {
//         return simple_case(cmdList);
//     }
//     // else if(cmdList->type == PIPE){
//     //     return pipe_case(cmdList); 
//     // }
//     return 0;
// }







