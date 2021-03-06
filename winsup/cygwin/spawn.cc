/* spawn.cc

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004,
   2005, 2006, 2007, 2008, 2009, 2010, 2011 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <stdlib.h>
#include <unistd.h>
#include <process.h>
#include <sys/wait.h>
#include <wingdi.h>
#include <winuser.h>
#include <wchar.h>
#include <ctype.h>
#include "cygerrno.h"
#include <sys/cygwin.h>
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "sigproc.h"
#include "cygheap.h"
#include "child_info.h"
#include "pinfo.h"
#include "environ.h"
#include "cygtls.h"
#include "tls_pbuf.h"
#include "winf.h"
#include "ntdll.h"

static suffix_info exe_suffixes[] =
{
  suffix_info ("", 1),
  suffix_info (".exe", 1),
  suffix_info (".com"),
  suffix_info (NULL)
};

#if 0
/* CV, 2009-11-05: Used to be used when searching for DLLs in calls to
   dlopen().  However, dlopen() on other platforms never adds a suffix by
   its own.  Therefore we use stat_suffixes now, which only adds a .exe
   suffix for symmetry. */
static suffix_info dll_suffixes[] =
{
  suffix_info (".dll"),
  suffix_info ("", 1),
  suffix_info (".exe", 1),
  suffix_info (NULL)
};
#endif

child_info_spawn *chExeced;

/* Add .exe to PROG if not already present and see if that exists.
   If not, return PROG (converted from posix to win32 rules if necessary).
   The result is always BUF.

   Returns (possibly NULL) suffix */

static const char *
perhaps_suffix (const char *prog, path_conv& buf, int& err, unsigned opt)
{
  const char *ext;

  err = 0;
  debug_printf ("prog '%s'", prog);
  buf.check (prog, PC_SYM_FOLLOW | PC_NULLEMPTY,
	     (opt & FE_DLL) ? stat_suffixes : exe_suffixes);

  if (buf.isdir ())
    {
      err = EACCES;
      ext = NULL;
    }
  else if (!buf.exists ())
    {
      err = ENOENT;
      ext = NULL;
    }
  else if (buf.known_suffix)
    ext = buf.get_win32 () + (buf.known_suffix - buf.get_win32 ());
  else
    ext = strchr (buf.get_win32 (), '\0');

  debug_printf ("buf %s, suffix found '%s'", (char *) buf.get_win32 (), ext);
  return ext;
}

/* Find an executable name, possibly by appending known executable
   suffixes to it.  The win32-translated name is placed in 'buf'.
   Any found suffix is returned in known_suffix.

   If the file is not found and !null_if_not_found then the win32 version
   of name is placed in buf and returned.  Otherwise the contents of buf
   is undefined and NULL is returned.  */

const char * __stdcall
find_exec (const char *name, path_conv& buf, const char *mywinenv,
	   unsigned opt, const char **known_suffix)
{
  const char *suffix = "";
  debug_printf ("find_exec (%s)", name);
  const char *retval;
  tmp_pathbuf tp;
  char *tmp = tp.c_get ();
  const char *posix = (opt & FE_NATIVE) ? NULL : name;
  bool has_slash = !!strpbrk (name, "/\\");
  int err;

  /* Check to see if file can be opened as is first.
     Win32 systems always check . first, but PATH may not be set up to
     do this. */
  if ((has_slash || opt & FE_CWD)
      && (suffix = perhaps_suffix (name, buf, err, opt)) != NULL)
    {
      if (posix && !has_slash)
	{
	  tmp[0] = '.';
	  tmp[1] = '/';
	  strcpy (tmp + 2, name);
	  posix = tmp;
	}
      retval = buf.get_win32 ();
      goto out;
    }

  win_env *winpath;
  const char *path;
  const char *posix_path;

  posix = (opt & FE_NATIVE) ? NULL : tmp;

  if (strchr (mywinenv, '/'))
    {
      /* it's not really an environment variable at all */
      int n = cygwin_conv_path_list (CCP_POSIX_TO_WIN_A, mywinenv, NULL, 0);
      char *s = (char *) alloca (n);
      if (cygwin_conv_path_list (CCP_POSIX_TO_WIN_A, mywinenv, s, n))
	goto errout;
      path = s;
      posix_path = mywinenv - 1;
    }
  else if (has_slash || strchr (name, '\\') || isdrive (name)
      || !(winpath = getwinenv (mywinenv))
      || !(path = winpath->get_native ()) || *path == '\0')
    /* Return the error condition if this is an absolute path or if there
       is no PATH to search. */
    goto errout;
  else
    posix_path = winpath->get_posix () - 1;

  debug_printf ("%s%s", mywinenv, path);
  /* Iterate over the specified path, looking for the file with and without
     executable extensions. */
  do
    {
      posix_path++;
      char *eotmp = strccpy (tmp, &path, ';');
      /* An empty path or '.' means the current directory, but we've
	 already tried that.  */
      if (opt & FE_CWD && (tmp[0] == '\0' || (tmp[0] == '.' && tmp[1] == '\0')))
	continue;

      *eotmp++ = '\\';
      strcpy (eotmp, name);

      debug_printf ("trying %s", tmp);

      int err1;

      if ((suffix = perhaps_suffix (tmp, buf, err1, opt)) != NULL)
	{
	  if (buf.has_acls () && check_file_access (buf, X_OK, true))
	    continue;

	  if (posix == tmp)
	    {
	      eotmp = strccpy (tmp, &posix_path, ':');
	      if (eotmp == tmp)
		*eotmp++ = '.';
	      *eotmp++ = '/';
	      strcpy (eotmp, name);
	    }
	  retval = buf.get_win32 ();
	  goto out;
	}
    }
  while (*path && *++path && (posix_path = strchr (posix_path, ':')));

 errout:
  posix = NULL;
  /* Couldn't find anything in the given path.
     Take the appropriate action based on null_if_not_found. */
  if (opt & FE_NNF)
    retval = NULL;
  else if (!(opt & FE_NATIVE))
    retval = name;
  else
    {
      buf.check (name);
      retval = buf.get_win32 ();
    }

 out:
  if (posix)
    retval = buf.set_path (posix);
  debug_printf ("%s = find_exec (%s)", (char *) buf.get_win32 (), name);
  if (known_suffix)
    *known_suffix = suffix ?: strchr (buf.get_win32 (), '\0');
  if (!retval && err)
    set_errno (err);
  return retval;
}

/* Utility for spawn_guts.  */

static HANDLE
handle (int fd, bool writing)
{
  HANDLE h;
  cygheap_fdget cfd (fd);

  if (cfd < 0)
    h = INVALID_HANDLE_VALUE;
  else if (cfd->close_on_exec ())
    h = INVALID_HANDLE_VALUE;
  else if (!writing)
    h = cfd->get_handle ();
  else
    h = cfd->get_output_handle ();

  return h;
}

int
iscmd (const char *argv0, const char *what)
{
  int n;
  n = strlen (argv0) - strlen (what);
  if (n >= 2 && argv0[1] != ':')
    return 0;
  return n >= 0 && strcasematch (argv0 + n, what) &&
	 (n == 0 || isdirsep (argv0[n - 1]));
}

struct pthread_cleanup
{
  _sig_func_ptr oldint;
  _sig_func_ptr oldquit;
  sigset_t oldmask;
  pthread_cleanup (): oldint (NULL), oldquit (NULL), oldmask ((sigset_t) -1) {}
};

static void
do_cleanup (void *args)
{
# define cleanup ((pthread_cleanup *) args)
  if (cleanup->oldmask != (sigset_t) -1)
    {
      signal (SIGINT, cleanup->oldint);
      signal (SIGQUIT, cleanup->oldquit);
      sigprocmask (SIG_SETMASK, &(cleanup->oldmask), NULL);
    }
# undef cleanup
}


int
spawn_guts (const char *prog_arg, const char *const *argv,
	    const char *const envp[], int mode, int __stdin, int __stdout)
{
  bool rc;
  pid_t cygpid;
  int res = -1;

  /* Check if we have been called from exec{lv}p or spawn{lv}p and mask
     mode to keep only the spawn mode. */
  bool p_type_exec = !!(mode & _P_PATH_TYPE_EXEC);
  mode = _P_MODE (mode);

  if (prog_arg == NULL)
    {
      syscall_printf ("prog_arg is NULL");
      set_errno (EFAULT);	/* As on Linux. */
      return -1;
    }
  if (!prog_arg[0])
    {
      syscall_printf ("prog_arg is empty");
      set_errno (ENOENT);	/* Per POSIX */
      return -1;
    }

  syscall_printf ("spawn_guts (%d, %.9500s)", mode, prog_arg);

  /* FIXME: This is no error condition on Linux. */
  if (argv == NULL)
    {
      syscall_printf ("argv is NULL");
      set_errno (EINVAL);
      return -1;
    }

  /* FIXME: There is a small race here and FIXME: not thread safe! */

  pthread_cleanup cleanup;
  if (mode == _P_SYSTEM)
    {
      sigset_t child_block;
      cleanup.oldint = signal (SIGINT, SIG_IGN);
      cleanup.oldquit = signal (SIGQUIT, SIG_IGN);
      sigemptyset (&child_block);
      sigaddset (&child_block, SIGCHLD);
      sigprocmask (SIG_BLOCK, &child_block, &cleanup.oldmask);
    }
  pthread_cleanup_push (do_cleanup, (void *) &cleanup);
  av newargv;
  linebuf one_line;
  child_info_spawn ch;
  PWCHAR envblock = NULL;
  path_conv real_path;
  bool reset_sendsig = false;

  tmp_pathbuf tp;
  PWCHAR runpath = tp.w_get ();
  int c_flags;
  bool wascygexec;
  cygheap_exec_info *moreinfo;

  bool null_app_name = false;
  STARTUPINFOW si = {};
  int looped = 0;
  HANDLE orig_wr_proc_pipe = NULL;

  myfault efault;
  if (efault.faulted ())
    {
      if (get_errno () == ENOMEM)
	set_errno (E2BIG);
      else
	set_errno (EFAULT);
      res = -1;
      goto out;
    }

  child_info_types chtype;
  if (mode != _P_OVERLAY)
    chtype = PROC_SPAWN;
  else
    chtype = PROC_EXEC;

  moreinfo = (cygheap_exec_info *) ccalloc_abort (HEAP_1_EXEC, 1,
						  sizeof (cygheap_exec_info));
  moreinfo->old_title = NULL;

  /* CreateProcess takes one long string that is the command line (sigh).
     We need to quote any argument that has whitespace or embedded "'s.  */

  int ac;
  for (ac = 0; argv[ac]; ac++)
    /* nothing */;

  newargv.set (ac, argv);

  int err;
  const char *ext;
  if ((ext = perhaps_suffix (prog_arg, real_path, err, FE_NADA)) == NULL)
    {
      set_errno (err);
      res = -1;
      goto out;
    }


  wascygexec = real_path.iscygexec ();
  res = newargv.fixup (prog_arg, real_path, ext, p_type_exec);

  if (res)
    goto out;

  if (!real_path.iscygexec () && cygheap->cwd.get_error ())
    {
      small_printf ("Error: Current working directory %s.\n"
		    "Can't start native Windows application from here.\n\n",
		    cygheap->cwd.get_error_desc ());
      set_errno (cygheap->cwd.get_error ());
      res = -1;
      goto out;
    }

  if (ac == 3 && argv[1][0] == '/' && argv[1][1] == 'c' &&
      (iscmd (argv[0], "command.com") || iscmd (argv[0], "cmd.exe")))
    {
      real_path.check (prog_arg);
      one_line.add ("\"");
      if (!real_path.error)
	one_line.add (real_path.get_win32 ());
      else
	one_line.add (argv[0]);
      one_line.add ("\"");
      one_line.add (" ");
      one_line.add (argv[1]);
      one_line.add (" ");
      one_line.add (argv[2]);
      real_path.set_path (argv[0]);
      null_app_name = true;
    }
  else
    {
      if (wascygexec)
	newargv.dup_all ();
      else if (!one_line.fromargv (newargv, real_path.get_win32 (),
				   real_path.iscygexec ()))
	{
	  res = -1;
	  goto out;
	}


      newargv.all_calloced ();
      moreinfo->argc = newargv.argc;
      moreinfo->argv = newargv;

      if (mode != _P_OVERLAY ||
	  !DuplicateHandle (GetCurrentProcess (), myself.shared_handle (),
			    GetCurrentProcess (), &moreinfo->myself_pinfo,
			    0, TRUE, DUPLICATE_SAME_ACCESS))
	moreinfo->myself_pinfo = NULL;
      else
	VerifyHandle (moreinfo->myself_pinfo);
    }
  WCHAR wone_line[one_line.ix + 1];
  if (one_line.ix)
    sys_mbstowcs (wone_line, one_line.ix + 1, one_line.buf);
  else
    wone_line[0] = L'\0';

  PROCESS_INFORMATION pi;
  pi.hProcess = pi.hThread = NULL;
  pi.dwProcessId = pi.dwThreadId = 0;

  /* Set up needed handles for stdio */
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = handle ((__stdin < 0 ? 0 : __stdin), false);
  si.hStdOutput = handle ((__stdout < 0 ? 1 : __stdout), true);
  si.hStdError = handle (2, true);

  si.cb = sizeof (si);

  c_flags = GetPriorityClass (GetCurrentProcess ());
  sigproc_printf ("priority class %d", c_flags);
  c_flags |= CREATE_SEPARATE_WOW_VDM | CREATE_UNICODE_ENVIRONMENT;

  if (mode == _P_DETACH)
    c_flags |= DETACHED_PROCESS;
  else
    fhandler_console::need_invisible ();

  if (mode != _P_OVERLAY)
    myself->exec_sendsig = NULL;
  else
    {
      /* Reset sendsig so that any process which wants to send a signal
	 to this pid will wait for the new process to become active.
	 Save the old value in case the exec fails.  */
      if (!myself->exec_sendsig)
	{
	  myself->exec_sendsig = myself->sendsig;
	  myself->exec_dwProcessId = myself->dwProcessId;
	  myself->sendsig = NULL;
	  reset_sendsig = true;
	}
      /* Save a copy of a handle to the current process around the first time we
	 exec so that the pid will not be reused.  Why did I stop cygwin from
	 generating its own pids again? */
      if (cygheap->pid_handle)
	/* already done previously */;
      else if (DuplicateHandle (GetCurrentProcess (), GetCurrentProcess (),
				GetCurrentProcess (), &cygheap->pid_handle,
				PROCESS_QUERY_INFORMATION, TRUE, 0))
	ProtectHandleINH (cygheap->pid_handle);
      else
	system_printf ("duplicate to pid_handle failed, %E");
    }

  if (null_app_name)
    runpath = NULL;
  else
    {
      USHORT len = real_path.get_nt_native_path ()->Length / sizeof (WCHAR);
      if (RtlEqualUnicodePathPrefix (real_path.get_nt_native_path (),
				     &ro_u_natp, FALSE))
	{
	  runpath = real_path.get_wide_win32_path (runpath);
	  /* If the executable path length is < MAX_PATH, make sure the long
	     path win32 prefix is removed from the path to make subsequent
	     not long path aware native Win32 child processes happy. */
	  if (len < MAX_PATH + 4)
	    {
	      if (runpath[5] == ':')
		runpath += 4;
	      else if (len < MAX_PATH + 6)
		*(runpath += 6) = L'\\';
	    }
	}
      else if (len < NT_MAX_PATH - ro_u_globalroot.Length / sizeof (WCHAR))
	{
	  UNICODE_STRING rpath;

	  RtlInitEmptyUnicodeString (&rpath, runpath,
				     (NT_MAX_PATH - 1) * sizeof (WCHAR));
	  RtlCopyUnicodeString (&rpath, &ro_u_globalroot);
	  RtlAppendUnicodeStringToString (&rpath,
					  real_path.get_nt_native_path ());
	}
      else
      	{
	  set_errno (ENAMETOOLONG);
	  res = -1;
	  goto out;
	}
    }
  syscall_printf ("null_app_name %d (%W, %.9500W)", null_app_name,
		  runpath, wone_line);

  cygbench ("spawn-guts");

  if (!real_path.iscygexec())
    cygheap->fdtab.set_file_pointers_for_exec ();

  moreinfo->envp = build_env (envp, envblock, moreinfo->envc,
			      real_path.iscygexec ());
  if (!moreinfo->envp || !envblock)
    {
      set_errno (E2BIG);
      res = -1;
      goto out;
    }
  ch.set (chtype, real_path.iscygexec ());
  ch.moreinfo = moreinfo;
  ch.__stdin = __stdin;
  ch.__stdout = __stdout;

  si.lpReserved2 = (LPBYTE) &ch;
  si.cbReserved2 = sizeof (ch);

  /* Depends on ch.set call above.
     Some file types might need extra effort in the parent after CreateProcess
     and before copying the datastructures to the child.  So we have to start
     the child in suspend state, unfortunately, to avoid a race condition. */
  if (!newargv.win16_exe
      && (!ch.iscygwin () || mode != _P_OVERLAY
	  || cygheap->fdtab.need_fixup_before ()))
    c_flags |= CREATE_SUSPENDED;

  /* When ruid != euid we create the new process under the current original
     account and impersonate in child, this way maintaining the different
     effective vs. real ids.
     FIXME: If ruid != euid and ruid != saved_uid we currently give
     up on ruid. The new process will have ruid == euid. */
loop:
  cygheap->user.deimpersonate ();

  if (!real_path.iscygexec () && mode == _P_OVERLAY)
    myself->process_state |= PID_NOTCYGWIN;

  if (!cygheap->user.issetuid ()
      || (cygheap->user.saved_uid == cygheap->user.real_uid
	  && cygheap->user.saved_gid == cygheap->user.real_gid
	  && !cygheap->user.groups.issetgroups ()
	  && !cygheap->user.setuid_to_restricted))
    {
      rc = CreateProcessW (runpath,	  /* image name - with full path */
			   wone_line,	  /* what was passed to exec */
			   &sec_none_nih, /* process security attrs */
			   &sec_none_nih, /* thread security attrs */
			   TRUE,	  /* inherit handles from parent */
			   c_flags,
			   envblock,	  /* environment */
			   NULL,
			   &si,
			   &pi);
    }
  else
    {
      /* Give access to myself */
      if (mode == _P_OVERLAY)
	myself.set_acl();

      WCHAR wstname[1024] = { L'\0' };
      HWINSTA hwst_orig = NULL, hwst = NULL;
      HDESK hdsk_orig = NULL, hdsk = NULL;
      PSECURITY_ATTRIBUTES sa;
      DWORD n;

      hwst_orig = GetProcessWindowStation ();
      hdsk_orig = GetThreadDesktop (GetCurrentThreadId ());
      GetUserObjectInformationW (hwst_orig, UOI_NAME, wstname, 1024, &n);
      /* Prior to Vista it was possible to start a service with the
	 "Interact with desktop" flag.  This started the service in the
	 interactive window station of the console.  A big security
	 risk, but we don't want to disable this behaviour for older
	 OSes because it's still heavily used by some users.  They have
	 been warned. */
      if (!cygheap->user.setuid_to_restricted
	  && wcscasecmp (wstname, L"WinSta0") != 0)
	{
	  WCHAR sid[128];

	  sa = sec_user ((PSECURITY_ATTRIBUTES) alloca (1024),
			 cygheap->user.sid ());
	  /* We're creating a window station per user, not per logon session.
	     First of all we might not have a valid logon session for
	     the user (logon by create_token), and second, it doesn't
	     make sense in terms of security to create a new window
	     station for every logon of the same user.  It just fills up
	     the system with window stations for no good reason. */
	  hwst = CreateWindowStationW (cygheap->user.get_windows_id (sid), 0,
				       GENERIC_READ | GENERIC_WRITE, sa);
	  if (!hwst)
	    system_printf ("CreateWindowStation failed, %E");
	  else if (!SetProcessWindowStation (hwst))
	    system_printf ("SetProcessWindowStation failed, %E");
	  else if (!(hdsk = CreateDesktopW (L"Default", NULL, NULL, 0,
					    GENERIC_ALL, sa)))
	    system_printf ("CreateDesktop failed, %E");
	  else
	    {
	      wcpcpy (wcpcpy (wstname, sid), L"\\Default");
	      si.lpDesktop = wstname;
	      debug_printf ("Desktop: %W", si.lpDesktop);
	    }
	}

      rc = CreateProcessAsUserW (cygheap->user.primary_token (),
			   runpath,	  /* image name - with full path */
			   wone_line,	  /* what was passed to exec */
			   &sec_none_nih, /* process security attrs */
			   &sec_none_nih, /* thread security attrs */
			   TRUE,	  /* inherit handles from parent */
			   c_flags,
			   envblock,	  /* environment */
			   NULL,
			   &si,
			   &pi);
      if (hwst)
	{
	  SetProcessWindowStation (hwst_orig);
	  CloseWindowStation (hwst);
	}
      if (hdsk)
	{
	  SetThreadDesktop (hdsk_orig);
	  CloseDesktop (hdsk);
	}
    }

  /* Restore impersonation. In case of _P_OVERLAY this isn't
     allowed since it would overwrite child data. */
  if (mode != _P_OVERLAY || !rc)
    cygheap->user.reimpersonate ();

  /* Set errno now so that debugging messages from it appear before our
     final debugging message [this is a general rule for debugging
     messages].  */
  if (!rc)
    {
      __seterrno ();
      syscall_printf ("CreateProcess failed, %E");
      /* If this was a failed exec, restore the saved sendsig. */
      if (reset_sendsig)
	{
	  myself->sendsig = myself->exec_sendsig;
	  myself->exec_sendsig = NULL;
	}
      myself->process_state &= ~PID_NOTCYGWIN;
      res = -1;
      goto out;
    }

  if (!(c_flags & CREATE_SUSPENDED))
    strace.write_childpid (ch, pi.dwProcessId);

  /* Fixup the parent data structures if needed and resume the child's
     main thread. */
  if (cygheap->fdtab.need_fixup_before ())
    cygheap->fdtab.fixup_before_exec (pi.dwProcessId);

  if (mode != _P_OVERLAY)
    cygpid = cygwin_pid (pi.dwProcessId);
  else
    cygpid = myself->pid;

  /* We print the original program name here so the user can see that too.  */
  syscall_printf ("%d = spawn_guts (%s, %.9500s)",
		  rc ? cygpid : (unsigned int) -1, prog_arg, one_line.buf);

  /* Name the handle similarly to proc_subproc. */
  ProtectHandle1 (pi.hProcess, childhProc);

  bool synced;
  pid_t pid;
  if (mode == _P_OVERLAY)
    {
      chExeced = &ch;	/* FIXME: there's a race here if a user sneaks in CTRL-C */
      myself->dwProcessId = pi.dwProcessId;
      strace.execing = 1;
      myself.hProcess = hExeced = pi.hProcess;
      real_path.get_wide_win32_path (myself->progname); // FIXME: race?
      sigproc_printf ("new process name %W", myself->progname);
      /* If wr_proc_pipe doesn't exist then this process was not started by a cygwin
	 process.  So, we need to wait around until the process we've just "execed"
	 dies.  Use our own wait facility to wait for our own pid to exit (there
	 is some minor special case code in proc_waiter and friends to accommodate
	 this).

	 If wr_proc_pipe exists, then it should be duplicated to the child.
	 If the child has exited already, that's ok.  The parent will pick up
	 on this fact when we exit.  dup_proc_pipe will close our end of the pipe.
	 Note that wr_proc_pipe may also be == INVALID_HANDLE_VALUE.  That will make
	 dup_proc_pipe essentially a no-op.  */
      if (!newargv.win16_exe && myself->wr_proc_pipe)
	{
	  if (!looped)
	    myself->sync_proc_pipe ();	/* Make sure that we own wr_proc_pipe
					   just in case we've been previously
					   execed. */
	  orig_wr_proc_pipe = myself->dup_proc_pipe (pi.hProcess);
	}
      pid = myself->pid;
      if (!ch.iscygwin ())
	close_all_files ();
    }
  else
    {
      myself->set_has_pgid_children ();
      ProtectHandle (pi.hThread);
      pinfo child (cygpid,
		   PID_IN_USE | (real_path.iscygexec () ? 0 : PID_NOTCYGWIN));
      if (!child)
	{
	  syscall_printf ("pinfo failed");
	  if (get_errno () != ENOMEM)
	    set_errno (EAGAIN);
	  res = -1;
	  goto out;
	}
      child->dwProcessId = pi.dwProcessId;
      child.hProcess = pi.hProcess;

      real_path.get_wide_win32_path (child->progname);
      /* FIXME: This introduces an unreferenced, open handle into the child.
	 The purpose is to keep the pid shared memory open so that all of
	 the fields filled out by child.remember do not disappear and so there
	 is not a brief period during which the pid is not available.
	 However, we should try to find another way to do this eventually. */
      DuplicateHandle (GetCurrentProcess (), child.shared_handle (),
		       pi.hProcess, NULL, 0, 0, DUPLICATE_SAME_ACCESS);
      child->start_time = time (NULL); /* Register child's starting time. */
      child->nice = myself->nice;
      if (!child.remember (mode == _P_DETACH))
	{
	  /* FIXME: Child in strange state now */
	  CloseHandle (pi.hProcess);
	  ForceCloseHandle (pi.hThread);
	  res = -1;
	  goto out;
	}
      pid = child->pid;
    }

  /* Start the child running */
  if (c_flags & CREATE_SUSPENDED)
    {
      ResumeThread (pi.hThread);
      strace.write_childpid (ch, pi.dwProcessId);
    }
  ForceCloseHandle (pi.hThread);

  sigproc_printf ("spawned windows pid %d", pi.dwProcessId);

  if ((mode == _P_DETACH || mode == _P_NOWAIT) && !ch.iscygwin ())
    synced = false;
  else
    synced = ch.sync (pi.dwProcessId, pi.hProcess, INFINITE);

  switch (mode)
    {
    case _P_OVERLAY:
      myself.hProcess = pi.hProcess;
      if (!synced)
	{
	  if (orig_wr_proc_pipe)
	    {
	      myself->wr_proc_pipe_owner = GetCurrentProcessId ();
	      myself->wr_proc_pipe = orig_wr_proc_pipe;
	    }
	  if (!ch.proc_retry (pi.hProcess))
	    {
	      looped++;
	      goto loop;
	    }
	  close_all_files (true);
	}
      else
	{
	  close_all_files (true);
	  if (!myself->wr_proc_pipe
	      && WaitForSingleObject (pi.hProcess, 0) == WAIT_TIMEOUT)
	    {
	      extern bool is_toplevel_proc;
	      is_toplevel_proc = true;
	      myself.remember (false);
	      waitpid (myself->pid, &res, 0);
	    }
	}
      myself.exit (EXITCODE_NOSET);
      break;
    case _P_WAIT:
    case _P_SYSTEM:
      if (waitpid (cygpid, &res, 0) != cygpid)
	res = -1;
      break;
    case _P_DETACH:
      res = 0;	/* Lost all memory of this child. */
      break;
    case _P_NOWAIT:
    case _P_NOWAITO:
    case _P_VFORK:
      res = cygpid;
      break;
    default:
      break;
    }

out:
  if (envblock)
    free (envblock);
  pthread_cleanup_pop (1);
  return (int) res;
#undef ch
}

extern "C" int
cwait (int *result, int pid, int)
{
  return waitpid (pid, result, 0);
}

/*
* Helper function for spawn runtime calls.
* Doesn't search the path.
*/

extern "C" int
spawnve (int mode, const char *path, const char *const *argv,
       const char *const *envp)
{
  static char *const empty_env[] = { NULL };

  int ret;
#ifdef NEWVFORK
  vfork_save *vf = vfork_storage.val ();

  if (vf != NULL && (vf->pid < 0) && mode == _P_OVERLAY)
    mode = _P_NOWAIT;
  else
    vf = NULL;
#endif

  syscall_printf ("spawnve (%s, %s, %x)", path, argv[0], envp);

  if (!envp)
    envp = empty_env;

  switch (_P_MODE (mode))
    {
    case _P_OVERLAY:
      spawn_guts (path, argv, envp, mode);
      /* Errno should be set by spawn_guts.  */
      ret = -1;
      break;
    case _P_VFORK:
    case _P_NOWAIT:
    case _P_NOWAITO:
    case _P_WAIT:
    case _P_DETACH:
    case _P_SYSTEM:
      ret = spawn_guts (path, argv, envp, mode);
#ifdef NEWVFORK
      if (vf)
	{
	  if (ret > 0)
	    {
	      debug_printf ("longjmping due to vfork");
	      vf->restore_pid (ret);
	    }
	}
#endif
      break;
    default:
      set_errno (EINVAL);
      ret = -1;
      break;
    }
  return ret;
}

/*
* spawn functions as implemented in the MS runtime library.
* Most of these based on (and copied from) newlib/libc/posix/execXX.c
*/

extern "C" int
spawnl (int mode, const char *path, const char *arg0, ...)
{
  int i;
  va_list args;
  const char *argv[256];

  va_start (args, arg0);
  argv[0] = arg0;
  i = 1;

  do
      argv[i] = va_arg (args, const char *);
  while (argv[i++] != NULL);

  va_end (args);

  return spawnve (mode, path, (char * const  *) argv, cur_environ ());
}

extern "C" int
spawnle (int mode, const char *path, const char *arg0, ...)
{
  int i;
  va_list args;
  const char * const *envp;
  const char *argv[256];

  va_start (args, arg0);
  argv[0] = arg0;
  i = 1;

  do
    argv[i] = va_arg (args, const char *);
  while (argv[i++] != NULL);

  envp = va_arg (args, const char * const *);
  va_end (args);

  return spawnve (mode, path, (char * const *) argv, (char * const *) envp);
}

extern "C" int
spawnlp (int mode, const char *file, const char *arg0, ...)
{
  int i;
  va_list args;
  const char *argv[256];
  path_conv buf;

  va_start (args, arg0);
  argv[0] = arg0;
  i = 1;

  do
      argv[i] = va_arg (args, const char *);
  while (argv[i++] != NULL);

  va_end (args);

  return spawnve (mode | _P_PATH_TYPE_EXEC, find_exec (file, buf),
		  (char * const *) argv, cur_environ ());
}

extern "C" int
spawnlpe (int mode, const char *file, const char *arg0, ...)
{
  int i;
  va_list args;
  const char * const *envp;
  const char *argv[256];
  path_conv buf;

  va_start (args, arg0);
  argv[0] = arg0;
  i = 1;

  do
    argv[i] = va_arg (args, const char *);
  while (argv[i++] != NULL);

  envp = va_arg (args, const char * const *);
  va_end (args);

  return spawnve (mode | _P_PATH_TYPE_EXEC, find_exec (file, buf),
		  (char * const *) argv, envp);
}

extern "C" int
spawnv (int mode, const char *path, const char * const *argv)
{
  return spawnve (mode, path, argv, cur_environ ());
}

extern "C" int
spawnvp (int mode, const char *file, const char * const *argv)
{
  path_conv buf;
  return spawnve (mode | _P_PATH_TYPE_EXEC, find_exec (file, buf), argv,
		  cur_environ ());
}

extern "C" int
spawnvpe (int mode, const char *file, const char * const *argv,
	  const char * const *envp)
{
  path_conv buf;
  return spawnve (mode | _P_PATH_TYPE_EXEC, find_exec (file, buf), argv, envp);
}

int
av::fixup (const char *prog_arg, path_conv& real_path, const char *ext,
	   bool p_type_exec)
{
  const char *p;
  bool exeext = ascii_strcasematch (ext, ".exe");
  if ((exeext && real_path.iscygexec ()) || ascii_strcasematch (ext, ".bat"))
    return 0;
  if (!*ext && ((p = ext - 4) > real_path.get_win32 ())
      && (ascii_strcasematch (p, ".bat") || ascii_strcasematch (p, ".cmd")
	  || ascii_strcasematch (p, ".btm")))
    return 0;
  while (1)
    {
      char *pgm = NULL;
      char *arg1 = NULL;
      char *ptr, *buf;
      OBJECT_ATTRIBUTES attr;
      IO_STATUS_BLOCK io;
      HANDLE h;
      NTSTATUS status;

      status = NtOpenFile (&h, SYNCHRONIZE | GENERIC_READ,
			   real_path.get_object_attr (attr, sec_none_nih),
			   &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   FILE_SYNCHRONOUS_IO_NONALERT
			   | FILE_OPEN_FOR_BACKUP_INTENT
			   | FILE_NON_DIRECTORY_FILE);
      if (!NT_SUCCESS (status))
	{
	  /* File is not readable?  Doesn't mean it's not executable.
	     Test for executablility and if so, just assume the file is
	     a cygwin executable and go ahead. */
	  if (status == STATUS_ACCESS_DENIED && real_path.has_acls ()
	      && check_file_access (real_path, X_OK, true) == 0)
	    {
	      real_path.set_cygexec (true);
	      break;
	    }
	  goto err;
	}

      HANDLE hm = CreateFileMapping (h, &sec_none_nih, PAGE_READONLY, 0, 0, NULL);
      NtClose (h);
      if (!hm)
	{
	  /* ERROR_FILE_INVALID indicates very likely an empty file. */
	  if (GetLastError () == ERROR_FILE_INVALID)
	    {
	      debug_printf ("zero length file, treat as script.");
	      goto just_shell;
	    }
	  goto err;
	}
      buf = (char *) MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
      CloseHandle (hm);
      if (!buf)
	goto err;

      {
	myfault efault;
	if (efault.faulted ())
	  {
	    UnmapViewOfFile (buf);
	    real_path.set_cygexec (false);
	    break;
	  }
	if (buf[0] == 'M' && buf[1] == 'Z')
	  {
	    WORD subsys;
	    unsigned off = (unsigned char) buf[0x18] | (((unsigned char) buf[0x19]) << 8);
	    win16_exe = off < sizeof (IMAGE_DOS_HEADER);
	    if (!win16_exe)
	      real_path.set_cygexec (!!hook_or_detect_cygwin (buf, NULL, subsys));
	    else
	      real_path.set_cygexec (false);
	    UnmapViewOfFile (buf);
	    break;
	  }
      }

      debug_printf ("%s is possibly a script", real_path.get_win32 ());

      ptr = buf;
      if (*ptr++ == '#' && *ptr++ == '!')
	{
	  ptr += strspn (ptr, " \t");
	  size_t len = strcspn (ptr, "\r\n");
	  if (len)
	    {
	      char *namebuf = (char *) alloca (len + 1);
	      memcpy (namebuf, ptr, len);
	      namebuf[len] = '\0';
	      for (ptr = pgm = namebuf; *ptr; ptr++)
		if (!arg1 && (*ptr == ' ' || *ptr == '\t'))
		  {
		    /* Null terminate the initial command and step over any
		       additional white space.  If we've hit the end of the
		       line, exit the loop.  Otherwise, we've found the first
		       argument. Position the current pointer on the last known
		       white space. */
		    *ptr = '\0';
		    char *newptr = ptr + 1;
		    newptr += strspn (newptr, " \t");
		    if (!*newptr)
		      break;
		    arg1 = newptr;
		    ptr = newptr - 1;
		  }
	    }
	}
      UnmapViewOfFile (buf);
just_shell:
      if (!pgm)
	{
	  if (!p_type_exec)
	    {
	      /* Not called from exec[lv]p.  Don't try to treat as script. */
	      debug_printf ("%s is not a valid executable",
			    real_path.get_win32 ());
	      set_errno (ENOEXEC);
	      return -1;
	    }
	  if (ascii_strcasematch (ext, ".com"))
	    break;
	  pgm = (char *) "/bin/sh";
	  arg1 = NULL;
	}

      /* Check if script is executable.  Otherwise we start non-executable
	 scripts successfully, which is incorrect behaviour. */
      if (real_path.has_acls ()
	  && check_file_access (real_path, X_OK, true) < 0)
	return -1;	/* errno is already set. */

      /* Replace argv[0] with the full path to the script if this is the
	 first time through the loop. */
      replace0_maybe (prog_arg);

      /* pointers:
       * pgm	interpreter name
       * arg1	optional string
       */
      if (arg1)
	unshift (arg1);

      /* FIXME: This should not be using FE_NATIVE.  It should be putting
	 the posix path on the argv list. */
      find_exec (pgm, real_path, "PATH=", FE_NATIVE, &ext);
      unshift (real_path.get_win32 (), 1);
    }
  return 0;

err:
  __seterrno ();
  return -1;
}
