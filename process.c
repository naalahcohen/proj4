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

void free_list(DirNode *head) {
    DirNode *current = head;
    while (current != NULL) {
        DirNode *next = current->next;
        free(current->directory); // Free any dynamically allocated strings
        free(current);
        current = next;
    }
}

void cleanup() {
    // Free all global structures, lists, and other dynamically allocated memory here
    free_list(dirStack);  // Example for freeing a directory stack
    // Free any other allocated resources
}


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

void clear_local_vars(const CMD *cmdList) {
    // Iterate over the local variables in cmdList and unset them
    for (int i = 0; i < cmdList->nLocal; i++) {
        unsetenv(cmdList->locVar[i]);  // Remove the variable from the environment
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
    if (cmdList->argv[0] == NULL) {
        fprintf(stderr, "Error: Command is NULL\n");
        *status = 1;
        return *status;
    }

    if (cmdList->argv[0] != NULL && strcmp(cmdList->argv[0], "cd") == 0) {
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
    else if (cmdList->argv[0] != NULL && strcmp(cmdList->argv[0], "pushd") == 0) {
        // Ensure directory argument is provided
        if (cmdList->argc < 2 || cmdList->argv[1] == NULL) {
            fprintf(stderr, "pushd: directory argument required\n");
            *status = 1;
            return *status;
        }
        // Call pushd with the specified directory
        return pushd(cmdList->argv[1], status);
    }
    else if (cmdList->argv[0] != NULL &&strcmp(cmdList->argv[0], "popd") == 0) {
        // Call popd to pop and change to the previous directory in the stack
        return popd(status);
    }
    
    *status = 1;  // Not a recognized built-in command
    return *status;
}

int simple_case(const CMD *cmdList, int *status) {
    if (cmdList->argv[0] == NULL) {
        fprintf(stderr, "Error: Command is NULL\n");
        *status = 1;
        return *status;
    }

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
        *status = (int)errno;
        return *status;
    }

    if (pid == 0) {  // Child process
        *status = redirection(cmdList);
        if (*status != 0) {
            exit(*status);
        }
        execvp(cmdList->argv[0], cmdList->argv);
        perror("execvp failed");
        exit(127);
    }

    // Parent process
    int wait_status;
    if (waitpid(pid, &wait_status, 0) == -1) {
        perror("waitpid failed");
        *status = (int)errno;
        return *status;
    }

    *status = STATUS(wait_status);
    return *status;
}

int pipe_case(const CMD *cmdList, int *status) {
    int status1, status2;
    int pid, pid1;
    CMD *cLeft = cmdList->left;
    CMD *cRight = cmdList->right;

    int fd[2];
    if (pipe(fd) == -1) {
        perror("pipe failed");
        return errno;
    }

    // Fork for the left command
    pid = fork();
    if (pid == -1) {
        perror("fork failed for left command");
        return errno;
    }
    if (pid == 0) {  // Left process
        close(fd[0]);                    // Close unused read end
        dup2(fd[1], STDOUT_FILENO);       // Redirect stdout to write end of the pipe
        close(fd[1]);                     // Close write end after duplication
        status1 = process(cLeft); // Process left command
        exit(status1);                // Exit with the left command's status
    }

    // Fork for the right command
    pid1 = fork();
    if (pid1 == -1) {
        perror("fork failed for right command");
        return errno;
    }
    if (pid1 == 0) {  // Right process
        close(fd[1]);                    // Close unused write end
        dup2(fd[0], STDIN_FILENO);       // Redirect stdin to read end of the pipe
        close(fd[0]);                    // Close read end after duplication
        status2 = process(cRight); // Process right command
        exit(status2);               // Exit with the right command's status
    }
    else{
        close(fd[0]);
        close(fd[1]);

        // Wait for both commands to complete and apply STATUS to interpret them
        waitpid(pid, &status1, 0);       // Wait for left command
        status1 = STATUS(status1);       // Apply STATUS macro

        waitpid(pid1, &status2, 0);      // Wait for right command
        status2 = STATUS(status2); 

    }
    // Parent process: close both ends of the pipe and wait for both children
    // Extract the exit status of the right command (last in the pipeline)
    if (status2 != 0) {
        *status = status2;
    } else {
        *status = status1; // Non-zero exit status if the process did not exit normally
    }

    // Set environment variable $? to the final status
    char status_str[10];
    snprintf(status_str, sizeof(status_str), "%d", *status);
    setenv("?", status_str, 1);

    return *status;
}

int subcommand_case(const CMD *cmdList, int *status) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed for subcommand");
        *status = errno;  // Correctly assign the value to the dereferenced status pointer
        return errno;
    }
    if (pid == 0) {  // Child process
        exit(process(cmdList->left));  // Process the subcommand and exit with its status
    }

    // Parent process: wait for the subcommand and capture its exit status
    int wait_status;
    if (waitpid(pid, &wait_status, 0) == -1) {
        perror("waitpid failed for subcommand");
        *status = errno;  // Correctly assign the value to the dereferenced status pointer
    } else {
        *status = STATUS(wait_status);  // Use STATUS macro to interpret exit status
    }

    return *status;
}


int process(const CMD *cmdList) {
    int status = 0;  // Default to success unless an error occurs

    if (cmdList->type == SIMPLE) {
        getlocalvar(cmdList);  // Set up the local variables for SIMPLE command
        simple_case(cmdList, &status);
        clear_local_vars(cmdList);  // Clear after processing SIMPLE command
    }
    else if (cmdList->type == PIPE) {
        getlocalvar(cmdList);  // Set up for PIPE command
        pipe_case(cmdList, &status);
        clear_local_vars(cmdList);  // Clear after processing PIPE command
    }
    else if (cmdList->type == SEP_AND) {
        int left_status = process(cmdList->left);
        if (left_status == 0) {
            getlocalvar(cmdList->right);  // Set up for right child if needed
            status = process(cmdList->right);
            clear_local_vars(cmdList->right);  // Clear after processing the right command
        } else {
            status = left_status;
        }
        clear_local_vars(cmdList->left);  // Clear after processing the left command
    }
    else if (cmdList->type == SEP_OR) {
        int left_status = process(cmdList->left);
        if (left_status != 0) {
            getlocalvar(cmdList->right);  // Set up for right child if needed
            status = process(cmdList->right);
            clear_local_vars(cmdList->right);  // Clear after processing the right command
        } else {
            status = left_status;
        }
        clear_local_vars(cmdList->left);  // Clear after processing the left command
    }
    else if (cmdList->type == SEP_END) {
        if (cmdList->left != NULL) {
            getlocalvar(cmdList->left);
            simple_case(cmdList->left, &status);
            clear_local_vars(cmdList->left);
        }
        if (cmdList->right != NULL) {
            getlocalvar(cmdList->right);
            simple_case(cmdList->right, &status);
            clear_local_vars(cmdList->right);
        }
    }
    else if(cmdList->type == SUBCMD){
        getlocalvar(cmdList);
        subcommand_case(cmdList, &status); 
        clear_local_vars(cmdList);
    }

    // Update environment variable $? with the exit status
    char status_str[10];
    snprintf(status_str, sizeof(status_str), "%d", status);
    setenv("?", status_str, 1);

    return status;
}


//create a split terminal 
//re read spec line by line (particular sentences that are somehting i need to handle)




//testing different combinations of different commands
//simple test cases for isolatign the com

//nac64@kangaroo:~/cs323/proj4/starter-code$ ./Bash < tests/quick_test.txt 

//~/cs323/proj4/starter-code$ /c/cs323/proj4/Bash 

