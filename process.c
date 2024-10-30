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

int redirection(const CMD *cmdList) {
    // Redirect stdin to file
    if (cmdList->fromType == RED_IN) {
        int old_opened = open(cmdList->fromFile, O_RDONLY);
        if (old_opened == -1) {
            perror("Error: invalid file descriptor for input redirection");
            return errno;  // Return errno instead of exiting
        }
        dup2(old_opened, STDIN_FILENO);
        close(old_opened);
    }
    else if (cmdList->toType == RED_OUT) {
        int fd = open(cmdList->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            perror("Error opening output file for redirection");
            return errno;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    else if (cmdList->toType == RED_OUT_APP) {
        int fd = open(cmdList->toFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd == -1) {
            perror("Error opening output file for appending");
            return errno;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    else if (cmdList->fromType == RED_IN_HERE) {
        char tempFilePath[] = "/tmp/hereXXXXXX";
        int fd = mkstemp(tempFilePath);
        if (fd == -1) {
            perror("Error creating temporary file for HERE document");
            return errno;
        }

        if (write(fd, cmdList->fromFile, strlen(cmdList->fromFile)) == -1) {
            perror("Error writing to temporary HERE document file");
            close(fd);
            return errno;
        }

        if (lseek(fd, 0, SEEK_SET) == -1) {
            perror("Error seeking to start of HERE document file");
            close(fd);
            return errno;
        }

        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("Error redirecting stdin for HERE document");
            close(fd);
            return errno;
        }
        close(fd);
        unlink(tempFilePath);
    }
    
    return 0;  // Success
}
int pushd(const char *dir, int *status) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("pushd: getcwd failed");
        *status = (int)errno;
        return *status;
    }

    if (chdir(dir) != 0) {
        perror("pushd: chdir failed");
        *status = (int)errno;
        return *status;
    }

    DirNode *newNode = malloc(sizeof(DirNode));
    if (!newNode) {
        perror("pushd: malloc failed");
        chdir(cwd);  // Restore original directory
        *status = (int)errno;
        return *status;
    }

    newNode->directory = strdup(cwd);
    if (!newNode->directory) {
        perror("pushd: strdup failed");
        free(newNode);
        chdir(cwd);  // Restore original directory
        *status = (int)errno;
        return *status;
    }

    newNode->next = dirStack;
    dirStack = newNode;

    printStack(dirStack);
    *status = 0;
    return *status;
}

int popd(int *status) {
    if (dirStack == NULL) {
        fprintf(stderr, "popd: directory stack empty\n");
        *status = 1;
        return *status;
    }

    DirNode *top = dirStack;
    if (chdir(top->directory) != 0) {
        perror("popd: chdir failed");
        *status = (int)errno;
        return *status;
    }
    dirStack = dirStack->next;
    free(top->directory);
    free(top);

    printStack(dirStack);
    *status = 0;
    return *status;
}
//either built in simple command 
//not built in simple command 
int builtin_simplecase(const CMD *cmdList, int *status) {
    if (strcmp(cmdList->argv[0], "cd") == 0) {
        // Check for too many arguments
        if (cmdList->argc > 2) {
            fprintf(stderr, "cd: too many arguments\n");
            *status = 1;
            return *status;
        }

        // No argument provided, use HOME directory
        if (cmdList->argv[1] == NULL) {
            const char *home = getenv("HOME");
            if (home == NULL) {
                fprintf(stderr, "cd: HOME not set\n");
                *status = 1;
                return *status;
            }
            if (chdir(home) != 0) {
                perror("cd: error changing to HOME directory");
                *status = (int)errno;
                return *status;
            }
            *status = 0;
            return *status;
        }

        // Change to specified directory
        if (chdir(cmdList->argv[1]) != 0) {
            perror("cd: error changing directory");
            *status = (int)errno;
            return *status;
        }
        *status = 0;
        return *status;
    } 
    else if (strcmp(cmdList->argv[0], "pushd") == 0) {
        // Ensure directory argument is provided
        if (cmdList->argc < 2) {
            fprintf(stderr, "pushd: directory argument required\n");
            *status = 1;
            return *status;
        }
        // Call pushd with the specified directory
        return pushd(cmdList->argv[1], status);
    }
    else if (strcmp(cmdList->argv[0], "popd") == 0) {
        // Call popd to pop and change to the previous directory in the stack
        return popd(status);
    }
    
    *status = 1;  // Not a recognized built-in command
    return *status;
}



int simple_case(const CMD *cmdList, int *status) {
    if (strcmp(cmdList->argv[0], "cd") == 0 ||
        strcmp(cmdList->argv[0], "pushd") == 0 ||
        strcmp(cmdList->argv[0], "popd") == 0) {
        // Call builtin command and update status
        return builtin_simplecase(cmdList, status);
    }

    // For non-builtin commands
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed");
        *status = (int)errno;  // Set status to errno on failure
        return *status;
    }

    if (pid == 0) {  // Child process
        *status = redirection(cmdList);  // Handle redirection
        if (*status != 0) {
            exit(*status);  // If redirection fails, exit with error status
        }
        execvp(cmdList->argv[0], cmdList->argv);
        perror("execvp failed");
        exit(127);  // If execvp fails, exit with specific code
    }

    // Parent process
    int wait_status;
    if (waitpid(pid, &wait_status, 0) == -1) {
        perror("waitpid failed");
        *status = (int)errno;  // Set errno as status if waitpid fails
        return *status;
    }

    // Use STATUS macro to interpret child process status
    *status = STATUS(wait_status);
    return *status;
}
int pipe_case (const CMD *cmdList){

}

int process(const CMD *cmdList) {
    int status = 0;  // Default to success unless an error occurs
    getlocalvar(cmdList);

    if (cmdList->type == SIMPLE) {
        simple_case(cmdList, &status);
    }

    // Update environment variable $? with the exit status
    char status_str[10];
    snprintf(status_str, sizeof(status_str), "%d", status);
    setenv("?", status_str, 1);

    return status;
}








