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
        dprintf(STDOUT_FILENO,"%s", cwd);
    }

    DirNode *current = dirStack;
    while (current) {
        dprintf(STDOUT_FILENO," %s", current->directory);
        current = current->next;
    }
    dprintf(STDOUT_FILENO,"\n");
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

int map_errno_to_exit_status(int err, const char *cmd) {
    if (err == ENOENT && (strcmp(cmd, "cd") == 0 || strcmp(cmd, "pushd") == 0)) {
        return 2;  // Command failed due to missing file or directory
    }
    switch (err) {
        case EINVAL:
            return 2;  // Incorrect usage
        case ENOENT:
            return 127;  // Command not found for exec errors
        case EACCES:
            return 126;  // Permission denied
        default:
            return err;  // Return errno for general cases
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
        int fd = open(cmdList->toFile, O_WRONLY | O_CREAT | O_APPEND, 0666);
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

int pushd(const CMD *cmdList, int *status) {
    if (cmdList->argc < 2 || cmdList->argv[1] == NULL) {  // Missing directory argument
        fprintf(stderr, "usage: pushd <dirName>\n");
        *status = 1;  // Usage error
        return *status;
    }

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("pushd: getcwd failed");
        *status = 2;  // Directory error if unable to get current directory
        return *status;
    }

    if (chdir(cmdList->argv[1]) != 0) {  // Failed to change to the new directory
        perror("pushd: chdir failed");
        *status = 2;
        return *status;
    }

    DirNode *newNode = malloc(sizeof(DirNode));
    if (!newNode) {
        perror("pushd: malloc failed");
        chdir(cwd);  // Restore original directory
        *status = 1;  // Memory allocation error is a usage issue
        return *status;
    }

    newNode->directory = strdup(cwd);  // Save previous directory
    if (!newNode->directory) {
        perror("pushd: strdup failed");
        free(newNode);
        chdir(cwd);  // Restore original directory
        *status = 1;  // Memory allocation error is a usage issue
        return *status;
    }

    newNode->next = dirStack;
    dirStack = newNode;

    printStack(dirStack);  // Show the updated stack
    *status = 0;
    return *status;
}



int popd(int *status) {
    if (dirStack == NULL) {
        fprintf(stderr, "popd: directory stack empty\n");
        *status = 1;  // Return 1 for empty stack error
        return *status;
    }

    DirNode *top = dirStack;
    if (chdir(top->directory) != 0) {
        perror("popd: chdir failed");
        *status = map_errno_to_exit_status(errno, "popd");  // Map errno for consistent exit status
        return *status;
    }

    dirStack = dirStack->next;
    free(top->directory);
    free(top);

    printStack(dirStack);  // Print updated stack after pop
    *status = 0;  // Success
    return *status;
}
//either built in simple command 
//not built in simple command 
int cd_command(const CMD *cmdList, int *status) {
    if (cmdList->argc > 2) {  // Too many arguments
        fprintf(stderr, "usage: cd OR cd <dirName>\n");
        *status = 1;  // Usage error
        return *status;
    }

    if (cmdList->argv[1] == NULL) {  // No argument provided; change to HOME
        const char *home = getenv("HOME");
        if (home == NULL) {
            fprintf(stderr, "cd: HOME not set\n");
            *status = 1;
            return *status;
        }
        if (chdir(home) != 0) {  // Failed to access HOME directory
            perror("cd: chdir fail");
            *status = 2;  // Directory access error
            return *status;
        }
        *status = 0;
        return *status;
    }

    if (chdir(cmdList->argv[1]) != 0) {  // Failed to access specified directory
        perror("cd: chdir fail");
        *status = 2;
        return *status;
    }

    *status = 0;  // Success
    return *status;
}


 
int builtin_simplecase(const CMD *cmdList, int *status) {
    // Redirect output if necessary
    int saveout = dup(STDOUT_FILENO);
    if (saveout == -1) {
        perror("dup failed");
        *status = map_errno_to_exit_status(errno, "dup");
        return *status;
    }

    int redirect = redirection(cmdList);
    if (redirect != 0) {
        *status = map_errno_to_exit_status(redirect, "redirection");  // Use mapped error for redirection issues
        dup2(saveout, STDOUT_FILENO);  // Restore stdout in case of redirection error
        close(saveout);
        return *status;
    }

    // Check for and execute built-in commands
    if (cmdList->argv[0] == NULL) {
        fprintf(stderr, "Error: Command is NULL\n");
        *status = 1;
        dup2(saveout, STDOUT_FILENO);  // Restore stdout
        close(saveout);
        return *status;
    }

    if (strcmp(cmdList->argv[0], "cd") == 0) {
        // Execute `cd` and handle status
        *status = cd_command(cmdList, status);
    }
    else if (strcmp(cmdList->argv[0], "pushd") == 0) {
        // Execute `pushd` and handle status
        *status = pushd(cmdList, status);
    }
    else if (strcmp(cmdList->argv[0], "popd") == 0) {
        // Execute `popd` and handle status
        *status = popd(status);
    } else {
        *status = 1;  // Not a recognized built-in command
    }

    // Restore stdout in case of redirection
    dup2(saveout, STDOUT_FILENO);
    close(saveout);
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
        // *status = redirection(cmdList);
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
int groupcommand_case(const CMD *cmdList, int *status) {
    if (cmdList == NULL) {
        fprintf(stderr, "Error: Command is NULL\n");
        *status = 1;
        return *status;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed");
        *status = errno;
        return *status;
    }

    if (pid == 0) {  // Child process
        *status = redirection(cmdList);
        if (*status != 0) {
            exit(*status);
        }

        // Process left command
        if (cmdList->left != NULL) {
            int left_status = process(cmdList->left);
            if (left_status != 0) {
                exit(left_status);  // Exit if the left command fails
            }
        }

        // Process right command
        if (cmdList->right != NULL) {
            int right_status = process(cmdList->right);
            if (right_status != 0) {
                exit(right_status);  // Exit if the right command fails
            }
        }

        exit(0);  // Success if all commands in the group run successfully
    }

    // Parent process waits for the child
    int wait_status;
    if (waitpid(pid, &wait_status, 0) == -1) {
        perror("waitpid failed");
        *status = errno;
        return *status;
    }

    *status = STATUS(wait_status);  // Update status based on the child process exit
    return *status;
}




// int background_case (const CMD *cmdList, int *status){

// }

//without pid 
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
    } 
    else {
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
            status = process(cmdList->left);
            clear_local_vars(cmdList->left);
        }
        if (cmdList->right != NULL) {
            getlocalvar(cmdList->right);
            status = process(cmdList->right);
            clear_local_vars(cmdList->right);
        }
    }
    else if(cmdList->type == SUBCMD){
        getlocalvar(cmdList);
        subcommand_case(cmdList, &status); 
        clear_local_vars(cmdList);
    }
    else if (cmdList->type == PAR_LEFT && cmdList->toType == PAR_RIGHT){
        getlocalvar(cmdList);
        groupcommand_case(cmdList,&status);
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


//if SEP_BG
//WNOHANG
//flag to check if something is backgrounded or not, pass the flag through
//look at the flag and put in the double nohang
//need to aslo print out, checl every time you go into process fucntion if there are any process in the background that have finished
    //while loop that is continuly checkinf if there are any children that are done
    ////print out the message "backgrounded" to terminal
        //waitpid(-1) first arugment