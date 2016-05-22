/*
  Copyright (c) 2016 Fatih Kaya <1994274@gmail.com>

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.
*/

#ifndef FMILL_H_INCLUDED
#define FMILL_H_INCLUDED

#include <framer/framer.h>
#include <framer/frame-list.h>
#include <libmill.h>

typedef struct fmill_sock *fmill_sock;

struct fmill_event {
  fmill_sock conn;
  struct frm_frame *fr;
};

fmill_sock fmill_sock_bind (char *addr); // ex: tcp://0.0.0.0:7458
fmill_sock fmill_sock_connect (char *addr);
int fmill_send (fmill_sock self, struct frm_frame *fr);
int fmill_send2 (fmill_sock self, char *msg, int size);
chan fmill_eventsch (fmill_sock self);

#endif
