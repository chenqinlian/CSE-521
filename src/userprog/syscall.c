#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#include "threads/vaddr.h"
#include "threads/init.h"
#include "userprog/process.h"
#include <list.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "devices/input.h"
#include "threads/synch.h"

static void syscall_handler (struct intr_frame *);

static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write (int fd, const void *buffer, unsigned length);
static void sys_halt (void);
static void sys_close (int file_desc);
static bool sys_create (const char *file, unsigned initial_size);
static int sys_open (const char *file);
static int sys_exec (const char *cmd);
static int sys_wait(tid_t pid);
static int sys_filesize (int fd);
static void sys_seek(int file_desc, int pos);
static unsigned sys_tell(int file_desc);
static bool sys_remove (const char *file);

typedef int (*handler) (uint32_t, uint32_t, uint32_t);
static handler syscall_vec[128];
static struct lock file_lock;

struct filedescriptor_elem {
    int filedesc;
    struct file *file;
    struct list_elem elem;
    struct list_elem thread_elem;
  };

static struct list open_file_list;
static struct file *find_file_by_fde (int file_desc);
static struct filedescriptor_elem *find_fde_by_file (int file_desc);

void
syscall_init (void)  {
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");

  syscall_vec[SYS_EXIT] = (handler)sys_exit;
  syscall_vec[SYS_HALT] = (handler)sys_halt;
  syscall_vec[SYS_CREATE] = (handler)sys_create;
  syscall_vec[SYS_OPEN] = (handler)sys_open;
  syscall_vec[SYS_CLOSE] = (handler)sys_close;
  syscall_vec[SYS_READ] = (handler)sys_read;
  syscall_vec[SYS_WRITE] = (handler)sys_write;
  syscall_vec[SYS_EXEC] = (handler)sys_exec;
  syscall_vec[SYS_WAIT] = (handler)sys_wait;
  syscall_vec[SYS_FILESIZE] = (handler)sys_filesize;
  syscall_vec[SYS_SEEK] = (handler)sys_seek;
  syscall_vec[SYS_TELL] = (handler)sys_tell;
  syscall_vec[SYS_REMOVE] = (handler)sys_remove;
  
  list_init (&open_file_list);
  lock_init (&file_lock);

}

/* This function calls the appropriate function. */
static void
syscall_handler (struct intr_frame *f UNUSED)  {
  handler hndlr;
  int *stk_pos;
  int return_value;
  
  stk_pos = f->esp;
  
  if (!is_user_vaddr (stk_pos))
     sys_exit (-1);
  
  if (*stk_pos < SYS_HALT || *stk_pos > SYS_INUMBER)
     sys_exit (-1);
  
  hndlr = syscall_vec[*stk_pos];

  if (!(is_user_vaddr (stk_pos + 1) && is_user_vaddr (stk_pos + 2) && is_user_vaddr (stk_pos + 3)))
     sys_exit (-1);

  return_value = hndlr (*(stk_pos + 1), *(stk_pos + 2), *(stk_pos + 3));

  f->eax = return_value;
  return;
}

/* Read from the current file in execution */
static int sys_read (int file_desc, void *buffer, unsigned size) {
  struct file * fl;
  int return_val=-1;

  lock_acquire(&file_lock);
  if(file_desc==STDIN_FILENO)
  {
    uint16_t i;
    for(i=0;i<size;i++)
      *(uint8_t *)(buffer + i)= input_getc();
    return_val=size;
  }
  else if(file_desc==STDOUT_FILENO)
    return_val = -1;
  else if(!is_user_vaddr(buffer) || !is_user_vaddr(buffer + size))
  {
    lock_release(&file_lock);
    sys_exit(-1);
  }
  else
  {
    fl=find_file_by_fde(file_desc);
    if(fl)
      return_val=file_read(fl,buffer,size);
    else  
      return_val = -1;
  }

  lock_release (&file_lock);
  return return_val;
}

/* Write to the file. */
static int sys_write (int file_desc, const void *buffer, unsigned length) {
  struct file * f;
  int return_val = -1;
  lock_acquire (&file_lock);
  if (file_desc == STDOUT_FILENO) /* stdout */
    putbuf (buffer, length);
  
  else if (file_desc == STDIN_FILENO)  
    return_val = -1;
  else if (!is_user_vaddr (buffer) || !is_user_vaddr (buffer + length))
    {
      lock_release (&file_lock);
      sys_exit (-1);
    }
  else
    {
      f = find_file_by_fde (file_desc);
      if (!f)
        return_val = -1;
        
      return_val = file_write (f, buffer, length);
    }
    
  lock_release (&file_lock);
  return return_val;
}

/* Terminates and exits the process */
int sys_exit(int status)
{

  struct thread *cur;
  cur=thread_current();

  if (lock_held_by_current_thread (&file_lock))
    lock_release (&file_lock);

  if(list_empty(&cur->all_files))
    goto end;
  else {
    struct list_elem *element;
    while(!list_empty(&cur->all_files))
    {
      element=list_begin(&cur->all_files);
      sys_close(list_entry(element, struct filedescriptor_elem, thread_elem)->filedesc);
    }
  }

end:
  cur->return_status=status;
  thread_exit();
  return -1;
}

/* Close the file in execution */ 
static void sys_close(int file_desc) {

  struct filedescriptor_elem *fde;
  fde=find_fde_by_file(file_desc);

  if(fde!=NULL)
  { 
	  file_close(fde->file);
	  list_remove(&fde->elem);
	  list_remove(&fde->thread_elem);
	  free(fde);
  }
}

/* Creates a new file */
static bool sys_create (const char *file, unsigned initial_size)
{
 
  if(!file)
    sys_exit(-1);
  
  int return_value;
  lock_acquire(&file_lock);
  return_value = filesys_create (file, initial_size);
  lock_release(&file_lock);

  return return_value;
}

/* Opens a file for file operations */
static int sys_open (const char *file)
{
  struct file *f;
  struct filedescriptor_elem *fde;
  
  if (file == NULL) 
     return -1;
  if (!is_user_vaddr (file))
    sys_exit (-1);
  f = filesys_open (file);
  if (!f) 
    return -1;
    
  fde = (struct filedescriptor_elem *)malloc (sizeof (struct filedescriptor_elem));
  if (!fde) 
    {
      file_close (f);
      return -1;
    }
  else
  {
    static int fid=2;
    fde->file = f;
    fde->filedesc = fid++;
    list_push_back (&open_file_list, &fde->elem);
    list_push_back (&thread_current ()->all_files, &fde->thread_elem);
    return fde->filedesc;
  }
  return -1;
}

/* Halts the execution of the file */
static void sys_halt (void) {
  shutdown_power_off ();  
}

/* Starts execution of the clind process */
static int sys_exec (const char *cmd) {
  int ret;
  if (!cmd || !is_user_vaddr(cmd))
    return -1;

  lock_acquire(&file_lock);
  ret = process_execute(cmd);
  lock_release(&file_lock);
  return ret;
}

/* Here the parent will wait for a child process to die. */
static int sys_wait (tid_t tid) {
  return process_wait(tid);
}

/* This is a new method.  It checks if the file belongs any file descriptor. If yes, it returns the file ense it returns null*/
static struct file *find_file_by_fde (int file_desc) {
  struct filedescriptor_elem *fd;
  struct list_elem *el;
  
  for (el = list_begin (&open_file_list); el != list_end (&open_file_list); el = list_next (el))
    {
      fd = list_entry (el, struct filedescriptor_elem, elem);
      if (fd->filedesc == file_desc)
        return fd->file;
    }
    
  return NULL;
}

/* gets the size of the file using the file_length function */
static int sys_filesize (int file_desc) {
  struct file *file_size;
  file_size = find_file_by_fde(file_desc);
  if (!file_size)
    return -1;

  return file_length(file_size);
}

/* Tells the current position in the file which is under execution*/
static unsigned sys_tell(int file_desc) {
  struct file *f;
  f = find_file_by_fde(file_desc);

  if(!f)
    return -1;
  
  unsigned status_val =  file_tell(f);
  return status_val;
}

/* Changes position in the file while performing file operation */
static void sys_seek(int file_desc, int pos) {
  struct file *fl;
  fl = find_file_by_fde(file_desc);
  if(!fl)
    sys_exit (-1);
  
  file_seek(fl, pos);
}

static bool sys_remove (const char *file) {
  if (!file)
    return false;

  if (!is_user_vaddr (file))
    sys_exit (-1);

  lock_acquire(&file_lock);
  int return_value = filesys_remove (file);
  lock_release(&file_lock);

  return return_value;
}

/* My Implementation 
   This struct is used to get the structure of the file. This is used provide the structure of the file which is needed to retieve the file description 
   of the file to be closed.	
*/
static struct filedescriptor_elem *find_fde_by_file (int file_desc) {
  struct filedescriptor_elem *fd;
  struct list_elem *el;
  struct thread *cur;

  cur=thread_current();
  
  for (el = list_begin (&cur->all_files); el != list_end (&cur->all_files); el = list_next (el))
    {
      fd = list_entry (el, struct filedescriptor_elem, thread_elem);
      if (fd->filedesc == file_desc)
        return fd;
    }
    
  return NULL;
}
