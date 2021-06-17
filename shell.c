#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
/*W związku z tym, że używam Manjaroo który używa nowszej wersji biblioteki readline mianowicie readline 8 zmuszony jestem dołączyć bibliotekę stdio.h gdyż w przeciwnym wypadku próba kompilacji kodu wiąże się z tego typu błędami kompilacji
In file included from /usr/include/readline/readline.h:35,
                 from shell.c:1:
/usr/include/readline/rltypedefs.h:71:28: error: unknown type name ‘FILE’
   71 | typedef int rl_getc_func_t PARAMS((FILE *));
      |                            ^~~~~~
In file included from /usr/include/readline/readline.h:36,
                 from shell.c:1:
/usr/include/readline/rltypedefs.h:1:1: note: ‘FILE’ is defined in header ‘<stdio.h>’; did you forget to ‘#include <stdio.h>’?
  +++ |+#include <stdio.h>
    1 |  rltypedefs.h -- Type declarations for readline functions. 
In file included from /usr/include/readline/readline.h:35,
*/

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static sigjmp_buf loop_env;

static void sigint_handler(int sig) {
  siglongjmp(loop_env, sig);
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp) {
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}
/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++) {
    /* TODO: Handle tokens and open files as requested. */
    mode = token[i];
    if (mode == T_INPUT) {
      *inputp = Open(token[i + 1], O_RDONLY, S_IRWXU);
      token[i] = T_NULL;
      token[i + 1] = T_NULL;
    } else if (mode == T_OUTPUT) {
      *outputp = Open(token[i + 1], O_CREAT | O_WRONLY, S_IRWXU);
      token[i] = T_NULL;
      token[i + 1] = T_NULL;
    } else if (mode == T_NULL) {
      continue;
    } else {
      token[i] = mode;
      //token[n] = mode;
      n++;
    }
    
  }

  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);
  

  if (!bg) {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */
  pid_t pid = Fork();
  if (pid == 0) {
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    if (input != -1) {
      Dup2(input, STDIN_FILENO);
      MaybeClose(&input);
    }
    if (output != -1) {
      Dup2(output, STDOUT_FILENO);
      MaybeClose(&output);
    }
    Sigprocmask(SIG_SETMASK, &mask, NULL);
    Setpgid(0, 0);
    external_command(token);
  }
  int j = addjob(pid, bg);
  addproc(j, pid, token);
  if (bg == FG) {
    exitcode = monitorjob(&mask);
  }


  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens) {
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

    
  /* TODO: Start a subprocess and make sure it's moved to a process group. */
    pid_t pid = Fork();
    if (pid == 0) {
    Signal(SIGINT, sigint_handler);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    if (input != -1) {
      Dup2(input, STDIN_FILENO);
      MaybeClose(&input);
    }
    if (output != -1) {
      Dup2(output, STDOUT_FILENO);
      MaybeClose(&output);
    }
    Sigprocmask(SIG_SETMASK, mask, NULL);
    if (pgid != 0)
      Setpgid(0, pgid);
    else{
      Setpgid(0,0);
    }
    
    external_command(token);
  }
  MaybeClose(&input);
  MaybeClose(&output);
  if (pgid != 0)
      Setpgid(pid, pgid);

    else{
      Setpgid(pid,pid);
    }
  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */
  int i;
  int n=0;
  token_t *bufor = malloc(sizeof(token_t) * (10 + 1));;
  for (i = 0; i < ntokens; i++){
    bufor[n]=token[i];
    n++;
    if(token[i]==T_PIPE){
      if(pgid==0){
        MaybeClose(&input);
        bufor[n-1] = T_NULL; 
        pgid=do_stage(pgid,&mask,input,output,bufor,(n-1));
        n=0;
        job = addjob(pgid,bg);
        addproc(job,pgid,bufor);
      }
      else{
        input=next_input;
        mkpipe(&next_input,&output);
        bufor[n-1] = T_NULL;
        pid = do_stage(pgid,&mask,input,output,bufor,(n-1));
        n=0;
        addproc(job,pid,bufor);
      }
    }
  }
  input=next_input;
  output=dup(STDOUT_FILENO);
  pid = do_stage(pgid,&mask,input,output,bufor,n);
  addproc(job,pid,bufor);
  
  if (bg == FG) {
    exitcode = monitorjob(&mask);
  }

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  free(bufor);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

int main(int argc, char *argv[]) {
  rl_initialize();

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  Setpgid(0, 0);

  initjobs();

  Signal(SIGINT, sigint_handler);
  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  char *line;
  while (true) {
    if (!sigsetjmp(loop_env, 1)) {
      line = readline("# ");
    } else {
      msg("\n");
      continue;
    }

    if (line == NULL)
      break;

    if (strlen(line)) {
      add_history(line);
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
