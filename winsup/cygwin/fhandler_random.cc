/* fhandler_random.cc: code to access /dev/random and /dev/urandom

   Copyright 2000, 2001, 2002, 2003, 2004, 2005, 2007, 2009
   Red Hat, Inc.

   Written by Corinna Vinschen (vinschen@cygnus.com)

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <unistd.h>
#include "cygerrno.h"
#include "path.h"
#include "fhandler.h"

#define RANDOM   8
#define URANDOM  9

#define PSEUDO_MULTIPLIER       (6364136223846793005LL)
#define PSEUDO_SHIFTVAL		(21)

fhandler_dev_random::fhandler_dev_random ()
  : fhandler_base (), crypt_prov ((HCRYPTPROV) NULL)
{
}

int
fhandler_dev_random::open (int flags, mode_t)
{
  set_flags ((flags & ~O_TEXT) | O_BINARY);
  nohandle (true);
  set_open_status ();
  dummy_offset = 0;
  return 1;
}

bool
fhandler_dev_random::crypt_gen_random (void *ptr, size_t len)
{
  if (!crypt_prov
      && !CryptAcquireContext (&crypt_prov, NULL, MS_DEF_PROV, PROV_RSA_FULL,
			       CRYPT_VERIFYCONTEXT | CRYPT_MACHINE_KEYSET)
      && !CryptAcquireContext (&crypt_prov, NULL, MS_DEF_PROV, PROV_RSA_FULL,
			       CRYPT_VERIFYCONTEXT | CRYPT_MACHINE_KEYSET
			       | CRYPT_NEWKEYSET))
    {
      debug_printf ("%E = CryptAquireContext()");
      return false;
    }
  if (!CryptGenRandom (crypt_prov, len, (BYTE *)ptr))
    {
      debug_printf ("%E = CryptGenRandom()");
      return false;
    }
  return true;
}

int
fhandler_dev_random::pseudo_write (const void *ptr, size_t len)
{
  /* Use buffer to mess up the pseudo random number generator. */
  for (size_t i = 0; i < len; ++i)
    pseudo = (pseudo + ((unsigned char *)ptr)[i]) * PSEUDO_MULTIPLIER + 1;
  return len;
}

ssize_t __stdcall
fhandler_dev_random::write (const void *ptr, size_t len)
{
  if (!len)
    return 0;
  if (!ptr)
    {
      set_errno (EINVAL);
      return -1;
    }

  /* Limit len to a value <= 512 since we don't want to overact.
     Copy to local buffer because CryptGenRandom violates const. */
  unsigned char buf[512];
  size_t limited_len = len <= 512 ? len : 512;
  memcpy (buf, ptr, limited_len);

  /* Mess up system entropy source. Return error if device is /dev/random. */
  if (!crypt_gen_random (buf, limited_len) && dev () == FH_RANDOM)
    {
      __seterrno ();
      return -1;
    }
  /* Mess up the pseudo random number generator. */
  pseudo_write (buf, limited_len);
  return len;
}

int
fhandler_dev_random::pseudo_read (void *ptr, size_t len)
{
  /* Use pseudo random number generator as fallback entropy source.
     This multiplier was obtained from Knuth, D.E., "The Art of
     Computer Programming," Vol 2, Seminumerical Algorithms, Third
     Edition, Addison-Wesley, 1998, p. 106 (line 26) & p. 108 */
  for (size_t i = 0; i < len; ++i)
    {
      pseudo = pseudo * PSEUDO_MULTIPLIER + 1;
      ((unsigned char *)ptr)[i] = (pseudo >> PSEUDO_SHIFTVAL) & UCHAR_MAX;
    }
  return len;
}

void __stdcall
fhandler_dev_random::read (void *ptr, size_t& len)
{
  if (!len)
    return;

  if (!ptr)
    {
      set_errno (EINVAL);
      len = (size_t) -1;
      return;
    }

  if (crypt_gen_random (ptr, len))
    return;

  /* If device is /dev/urandom, use pseudo number generator as fallback.
     Don't do this for /dev/random since it's intended for uses that need
     very high quality randomness. */
  if (dev () == FH_URANDOM)
    {
      len = pseudo_read (ptr, len);
      return;
    }

  __seterrno ();
  len = (size_t) -1;
}

_off64_t
fhandler_dev_random::lseek (_off64_t off, int whence)
{
  /* As on Linux, fake being able to set an offset.  The fact that neither
     reading nor writing changes the dummy offset is also the same as on
     Linux (tested with kernel 2.6.23). */
  _off64_t new_off;

  switch (whence)
    {
    case SEEK_SET:
      new_off = off;
      break;
    case SEEK_CUR:
      new_off = dummy_offset + off;
      break;
    default:
      set_errno (EINVAL);
      return (_off64_t) -1;
    }
  if (new_off < 0)
    {
      set_errno (EINVAL);
      return (_off64_t) -1;
    }
  return dummy_offset = new_off;
}

int
fhandler_dev_random::close ()
{
  if (!hExeced && crypt_prov)
    while (!CryptReleaseContext (crypt_prov, 0)
	   && GetLastError () == ERROR_BUSY)
      Sleep (10);
  return 0;
}

int
fhandler_dev_random::dup (fhandler_base *child)
{
  fhandler_dev_random *fhr = (fhandler_dev_random *) child;
  fhr->crypt_prov = (HCRYPTPROV)NULL;
  return 0;
}
