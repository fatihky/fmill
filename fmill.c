#include "fmill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <framer/cont.h>

struct fmill_sock {
  int fd;
  struct frm_out_frame_list ol;
  struct frm_parser parser;
  int active;
  int tcp;
  int waiting_out_trigger;
  tcpsock msock;
  chan events;
  chan out_trigger;
};

/*  temporary hack for getting fd from libmill's unix and tcp socks.
    see: https://github.com/sustrik/libmill/blob/master/unix.c#L43-L65
    see: https://github.com/sustrik/libmill/blob/master/tcp.c#L50-L74 */

struct tmp_mill_sockbase {
  enum tmp_mill_tcptype {
    TMP_MILL_LISTENER,
    TMP_MILL_CONN
  } type;
  int fd;
};

static coroutine tcpframer (fmill_sock self);
static coroutine tcpframesender (fmill_sock self);

static int millsockfd(void *sock) {
  return ((struct tmp_mill_sockbase *)sock)->fd;
}

static int fmill_parse_addr (char *addr, char **addrcpy, int *tcpp, int *port) {
  size_t len = strlen (addr);
  char *s = &addr[len - 1];
  int tcp = strncmp (addr, "tcp://", 6) == 0;
  if (strncmp (addr, "tcp://", 6) != 0 && strncmp (addr, "unix://", 7) != 0) {
    return -EINVAL;
  }
  if (tcp) {
    s = strrchr (addr, ':');
    if (!s || s == &addr[len - 1]) {
      return -EINVAL;
    }
    *port = atoi (s + 1);
    if (*port <= 0) {
      return -EINVAL;
    }
  } else
    *port = -1;

  /*  always decrease by 6. because if decrease by 7
      unix path's last character won't be included in addrbuf */
  size_t addrlen = (s - addr) - 6;
  *addrcpy = malloc(addrlen + 1);
  if (!*addrcpy) {
    return -ENOMEM;
  }
  memcpy (*addrcpy, addr + (tcp ? 6 : 7), addrlen);
  (*addrcpy)[addrlen] = '\0';
  *tcpp = tcp;
  return 0;
}

static fmill_sock fmill_sock_new() {
  fmill_sock self = malloc (sizeof (struct fmill_sock));
  if (!self)
    return NULL;
  self->fd = -1;
  self->active = 0;
  self->tcp = -1;
  self->waiting_out_trigger = 0;
  self->msock = NULL;
  self->events = chmake(struct fmill_event, 0);
  if (!self->events)
    goto fail1;
  self->out_trigger = chmake(int, 0);
  if (!self->out_trigger)
    goto fail2;
  frm_out_frame_list_init (&self->ol);
  frm_parser_init (&self->parser);
  return self;

fail2:
  chclose (self->events);
fail1:
  free (self);
  return NULL;
}

void fmill_sock_free (fmill_sock self) {
  assert (self);
  chclose (self->events);
  chclose (self->out_trigger);
  free (self);
}

static coroutine tcpacceptor(fmill_sock self) {
  printf ("start accepting\n");
  for (; self->active;) {
    tcpsock sock = tcpaccept (self->msock, now() + 10000);
    if (!sock)
      continue;
    fmill_sock conn = fmill_sock_new();
    if (!conn)
      continue; // no mem
    conn->msock = sock;
    struct fmill_event ev;
    ev.fr = NULL;
    ev.conn = conn;
    conn->active = 1;
    conn->tcp = 1;
    go (tcpframer(conn));
    go (tcpframesender (conn));
    chs (self->events, struct fmill_event, ev);
  }
  printf ("stopped accepting\n");
}

static coroutine tcpframer (fmill_sock self) {
  int fd = millsockfd((void *)self->msock);

wait_in:
  for (;;) {
    int events = fdwait (fd, FDW_IN, now() + 10000);
    if (!(events & FDW_IN)) {
      if (!self->active)
        goto complete;
      continue;
    }
    break;
  }

  int trys = 0;
  for (; trys < 5 && self->active;) {
    struct frm_cbuf *cbuf = frm_cbuf_new(1400);
    if (!cbuf)
      goto wait_in;

    ssize_t nread = read (fd, cbuf->buf, 1400);
    if (nread == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      frm_cbuf_unref (cbuf);
      goto wait_in;
    }
    if (nread == 0)
      assert (0 && "handle connection close");
    if (nread < 0) {
      printf ("[fmill] read failed\n");
      printf ("nread: %zd\n", nread);
      printf ("errno: %d\n", errno);
      assert (0 && "handle nread < 0");
    }

    int rc = frm_parser_parse (&self->parser, cbuf, nread);

    if (rc != 0) {
      printf ("err: %s\n", strerror (rc));
    } else {
      struct fmill_event ev;
      ev.conn = NULL;
      while (!frm_list_empty (&self->parser.in_frames)) {
        struct frm_list_item *li = frm_list_begin (&self->parser.in_frames);
        struct frm_frame *fr = frm_cont (li, struct frm_frame, item);
        frm_list_erase (&self->parser.in_frames, li);
        ev.fr = fr;
        chs (self->events, struct fmill_event, ev);
      }
    }

    frm_cbuf_unref (cbuf);
    trys++;

    if (!self->active)
      goto complete;

    if (trys >= 5)
      goto wait_in;
  }

complete:
  printf ("tcpframer routine complete\n");
}

static coroutine tcpframesender (fmill_sock self) {
  int fd = millsockfd((void *)self->msock);

wait_out_trigger:
  self->waiting_out_trigger = 1;
  chr (self->out_trigger, int);
  self->waiting_out_trigger = 0;

wait_out:
  for (;;) {
    int events = fdwait (fd, FDW_OUT, now() + 10000);
    if (!self->active)
      goto complete;
    if (events & FDW_OUT)
      break;
  }

  int trys = 0;
  struct iovec iovs[512];
  for (;;) {
    if (!self->active)
      goto complete;
    if (frm_list_empty (&self->ol.list))
      goto wait_out_trigger;

    int retiovcnt = 0;
    ssize_t tow = frm_out_frame_list_get_iovs (&self->ol, iovs, 512, &retiovcnt);
    ssize_t nwritten = writev(fd, iovs, retiovcnt);

    if (nwritten == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      self->ol.out_index = 0;
      goto wait_out;
    }

    if (nwritten < 0) {
      printf ("[fmill] write failed\n");
      printf ("nwritten: %zd\n", nwritten);
      printf ("errno: %d\n", errno);
      assert (0 && "handle nwritten < 0");
    }

    frm_out_frame_list_written (&self->ol, nwritten);

    trys++;
    if (trys >= 5)
      goto wait_out;
  }

complete:
  printf ("tcpframesender routine complete.\n");
}

fmill_sock fmill_sock_bind (char *addr) {
  fmill_sock self;
  int rc;
  int tcp;
  int port;
  char *addrcpy;
  rc = fmill_parse_addr(addr, &addrcpy, &tcp, &port);
  if (rc < 0) {
    errno = -rc;
    return NULL;
  }
  self = fmill_sock_new();
  if (!self) {
    free (addrcpy);
    errno = ENOMEM;
    return NULL;
  }
  if (tcp) {
    ipaddr ip = ipremote(addrcpy, port, IPADDR_IPV4, -1);
    self->msock = tcplisten(ip, 1024);
    if (!self->msock) {
      goto fail1;
    }
    self->active = 1;
    self->tcp = 1;
    go (tcpacceptor(self));
  } else {
    assert (0 && "only tcp transport supported currently");
  }
  free (addrcpy);
  return self;
fail1:
  free (addrcpy);
  return NULL;
}

fmill_sock fmill_sock_connect (char *addr) {
  fmill_sock self;
  int rc;
  int tcp;
  int port;
  char *addrcpy;
  rc = fmill_parse_addr(addr, &addrcpy, &tcp, &port);
  if (rc < 0) {
    errno = -rc;
    return NULL;
  }
  self = fmill_sock_new();
  if (!self) {
    free (addrcpy);
    errno = ENOMEM;
    return NULL;
  }
  if (tcp) {
    ipaddr ip = ipremote(addrcpy, port, IPADDR_IPV4, -1);
    self->msock = tcpconnect(ip, -1);
    if (!self->msock) {
      goto fail1;
    }
    self->active = 1;
    self->tcp = 1;
    go (tcpframer(self));
    go (tcpframesender (self));
  } else {
    assert (0 && "only tcp transport supported currently");
  }
  free (addrcpy);
  return self;
fail1:
  free (addrcpy);
  return NULL;
}

int fmill_send (fmill_sock self, struct frm_frame *fr) {
  struct frm_out_frame_list_item *li = frm_out_frame_list_item_new();
  if (!li) {
    errno = ENOMEM;
    return -1;
  }
  frm_out_frame_list_item_set_frame (li, fr);
  frm_out_frame_list_insert (&self->ol, li);
  if (self->waiting_out_trigger) {
    chs (self->out_trigger, int, 1);
    self->waiting_out_trigger = 0;
  }
  return 0;
}

int fmill_send2 (fmill_sock self, char *msg, int size) {
  struct frm_frame *fr = malloc (sizeof (struct frm_frame));
  if (!fr) {
    errno = ENOMEM;
    return -1;
  }
  frm_frame_init (fr);
  int rc = frm_frame_set_data (fr, msg, size);
  if (rc != 0) {
    errno = rc;
    free (fr);
    return -1;
  }
  struct frm_out_frame_list_item *li = frm_out_frame_list_item_new();
  if (!li) {
    errno = ENOMEM;
    frm_frame_term (fr);
    return -1;
  }
  frm_out_frame_list_item_set_frame (li, fr);
  frm_out_frame_list_insert (&self->ol, li);
  if (self->waiting_out_trigger) {
    chs (self->out_trigger, int, 1);
    self->waiting_out_trigger = 0;
  }
  return 0;
}

chan fmill_eventsch (fmill_sock self) {
  return self->events;
}