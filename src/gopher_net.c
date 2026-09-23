#include <string.h>
#include "mega65/memory.h"
#include "meganet.h"
#include "gopher_net.h"
#include "gopher_scratch.h"

static unsigned char last_frame = 0;

/* Advances once per real video frame, so callers pace polling to real
 * wall-clock time instead of spinning thousands of times before a single
 * packet could arrive (see REQUIREMENTS.md section 4). */
static unsigned char tick(void)
{
  if (PEEK(0xd7fa) != last_frame) {
    last_frame = PEEK(0xd7fa);
    return 1;
  }
  return 0;
}

unsigned char gopher_net_error = GNERR_NONE;
unsigned char gopher_net_attempts = 0;

const char *gopher_net_strerror(void)
{
  switch (gopher_net_error) {
  case GNERR_DNS_BUSY:
    return "DNS: stack busy, query not sent";
  case GNERR_DNS_NOREPLY:
    return "DNS: no reply from name server";
  case GNERR_DNS_NOTFOUND:
    return "DNS: host not found";
  case GNERR_CONN_REFUSED:
    return "Refused by host (nothing listening?)";
  case GNERR_CONN_NOREPLY:
    return "No reply from host";
  case GNERR_SEND:
    return "Could not send request";
  case GNERR_NO_DATA:
    return "Connected, but no reply to the request";
  default:
    return "Unknown error";
  }
}

/* DHCP: three attempts of eight seconds. mega-net retries DISCOVER itself
 * every four seconds; the outer loop restarts a machine that has given up
 * or is stuck behind a link that was still coming up at power-on. */
#define DHCP_ATTEMPTS 3
#define DHCP_WAIT_FRAMES 400

unsigned char gopher_net_init(void)
{
  unsigned int frames;
  unsigned char attempt, st;

  mega65_io_enable();
  meganet_call(MEGANET_INIT, 0, 0, 0, 0);

  for (attempt = 0; attempt < DHCP_ATTEMPTS; attempt++) {
    meganet_dhcp_start();
    frames = 0;
    while (frames < DHCP_WAIT_FRAMES) {
      meganet_poll();
      if (tick()) {
        frames++;
        st = meganet_dhcp_state();
        if (st == MEGANET_DHCP_BOUND)
          return 1;
        if (st == MEGANET_DHCP_FAILED)
          break;
      }
    }
  }
  return 0;
}

/* Parse a dotted-quad like "192.168.1.232" without going near DNS.
 * FR-2 only calls for DNS when given a name rather than an IP, and this
 * also makes the client testable against a LAN server when public gopher
 * hosts start rate-limiting us (see R-8c). Returns 1 if the string was a
 * complete, valid dotted-quad. */
static unsigned char parse_dotted_quad(const char *s, unsigned char *out)
{
  unsigned int octet;
  unsigned char part, digits;

  for (part = 0; part < 4; part++) {
    octet = 0;
    digits = 0;
    while (*s >= 0x30 && *s <= 0x39) { /* '0'-'9' */
      octet = octet * 10 + (unsigned char)(*s - 0x30);
      if (octet > 255)
        return 0;
      digits++;
      s++;
    }
    if (digits == 0)
      return 0;
    out[part] = (unsigned char)octet;
    if (part < 3) {
      if (*s != 0x2e) /* '.' */
        return 0;
      s++;
    }
  }
  return *s == 0;
}

/* --- DNS cache --------------------------------------------------------
 *
 * gopher_dns_resolve() is called on every fetch, so moving around within
 * one site used to re-resolve the same hostname each time. A lookup is a
 * round trip to the name server every time; the cache makes it one.
 *
 * Browsing stays within one host most of the time, so a handful of
 * entries removes nearly every lookup after the first.
 *
 * Entries are only invalidated explicitly (gopher_dns_invalidate(), called
 * when a connection fails) rather than on a timer: there is no clock to
 * expire against, and a wrong address that is never retried would be worse
 * than an extra lookup. */
#define DNS_CACHE_ENTRIES 4
#define DNS_CACHE_HOST_LEN 31

/* In fixed low RAM, not .bss; see gopher_scratch.h. The entry layout is
 * declared there so the address can be typed. */
#define dns_cache SCR_DNS_CACHE

static unsigned char dns_cache_next = 0; /* round-robin victim */

/* Hostnames are case-insensitive, and one typed by hand need not match the
 * capitalisation a menu supplied. */
static unsigned char host_equal_ci(const char *a, const char *b)
{
  unsigned char ca, cb;

  for (;;) {
    ca = (unsigned char)*a++;
    cb = (unsigned char)*b++;
    if (ca >= 0x41 && ca <= 0x5a)
      ca = (unsigned char)(ca + 32);
    if (cb >= 0x41 && cb <= 0x5a)
      cb = (unsigned char)(cb + 32);
    if (ca != cb)
      return 0;
    if (ca == 0)
      return 1;
  }
}

static unsigned char dns_cache_lookup(const char *hostname, unsigned char *out)
{
  unsigned char i;

  for (i = 0; i < DNS_CACHE_ENTRIES; i++) {
    if (dns_cache[i].used && host_equal_ci(dns_cache[i].host, hostname)) {
      out[0] = dns_cache[i].ip[0];
      out[1] = dns_cache[i].ip[1];
      out[2] = dns_cache[i].ip[2];
      out[3] = dns_cache[i].ip[3];
      return 1;
    }
  }
  return 0;
}

static void dns_cache_store(const char *hostname, const unsigned char *ip)
{
  unsigned char i;

  if (strlen(hostname) > DNS_CACHE_HOST_LEN)
    return; /* too long to store; it will just be resolved again */

  /* refresh an existing entry rather than duplicating it */
  for (i = 0; i < DNS_CACHE_ENTRIES; i++)
    if (dns_cache[i].used && host_equal_ci(dns_cache[i].host, hostname))
      break;
  if (i == DNS_CACHE_ENTRIES) {
    i = dns_cache_next;
    dns_cache_next = (unsigned char)((dns_cache_next + 1) % DNS_CACHE_ENTRIES);
  }

  strcpy(dns_cache[i].host, hostname);
  dns_cache[i].ip[0] = ip[0];
  dns_cache[i].ip[1] = ip[1];
  dns_cache[i].ip[2] = ip[2];
  dns_cache[i].ip[3] = ip[3];
  dns_cache[i].used = 1;
}

void gopher_dns_invalidate(const char *hostname)
{
  unsigned char i;

  for (i = 0; i < DNS_CACHE_ENTRIES; i++)
    if (dns_cache[i].used && host_equal_ci(dns_cache[i].host, hostname))
      dns_cache[i].used = 0;
}

/* mega-net retransmits a lost query itself (three tries, two seconds
 * apart) and reports FAILED for both "no such host" and "no answer", so
 * the client waits for its verdict and reports which by whether the wait
 * ran out first. */
#define DNS_WAIT_FRAMES 400

unsigned char gopher_dns_resolve(const char *hostname, unsigned char *out)
{
  unsigned int frames;
  unsigned char state;

  if (parse_dotted_quad(hostname, out))
    return 1;
  if (dns_cache_lookup(hostname, out))
    return 1;

  gopher_net_attempts = 1;
  meganet_dns_start(hostname);
  state = MEGANET_DNS_WAITING;
  frames = 0;
  while (frames < DNS_WAIT_FRAMES) {
    meganet_poll();
    if (tick()) {
      frames++;
      state = meganet_dns_state();
      if (state != MEGANET_DNS_WAITING)
        break;
    }
  }
  if (state != MEGANET_DNS_DONE) {
    gopher_net_error = (state == MEGANET_DNS_FAILED && frames < DNS_WAIT_FRAMES)
                           ? GNERR_DNS_NOTFOUND
                           : GNERR_DNS_NOREPLY;
    return 0;
  }
  meganet_dns_result(out);
  dns_cache_store(hostname, out);
  return 1;
}

/* Fifteen seconds for a connection. mega-net resends an unanswered SYN
 * itself, so one attempt here is several on the wire. */
#define CONNECT_WAIT_FRAMES 750

unsigned char gopher_tcp_connect(const unsigned char *ip, unsigned int port)
{
  unsigned int frames;
  unsigned char st, fl;

  /* Servers close their end after responding, which leaves the previous
   * connection in CLOSE_WAIT until we close too. Drop it before opening
   * another: this is local cleanup on a connection the peer has already
   * finished with. */
  meganet_tcp_abort();
  for (frames = 0; frames < 3;) {
    meganet_poll();
    if (tick())
      frames++;
  }

  gopher_net_attempts = 1;
  meganet_tcp_connect(ip, port);
  frames = 0;
  while (frames < CONNECT_WAIT_FRAMES) {
    meganet_poll();
    if (tick()) {
      frames++;
      st = meganet_tcp_state(&fl, 0);
      if (st == MEGANET_TCP_ESTABLISHED) {
        gopher_net_error = GNERR_NONE;
        return 1;
      }
      if (st == MEGANET_TCP_CLOSED) {
        gopher_net_error = (fl & MEGANET_TCP_F_REFUSED) ? GNERR_CONN_REFUSED
                                                        : GNERR_CONN_NOREPLY;
        return 0;
      }
    }
  }
  meganet_tcp_abort();
  gopher_net_error = GNERR_CONN_NOREPLY;
  return 0;
}

/* The request goes out as one segment: the stack sends whatever is
 * queued when it next runs, and a selector plus CRLF fits any MSS. */
#define SELECTOR_MAX 200
static unsigned char reqbuf[SELECTOR_MAX + 2];

unsigned char gopher_tcp_send_selector(const char *selector)
{
  unsigned char len = strlen(selector);
  unsigned char i;

  if (len > SELECTOR_MAX)
    len = SELECTOR_MAX;
  for (i = 0; i < len; i++)
    reqbuf[i] = (unsigned char)selector[i];
  reqbuf[len] = 0x0d;
  reqbuf[len + 1] = 0x0a;

  if (meganet_tcp_send(reqbuf, (unsigned int)(len + 2)) != (unsigned int)(len + 2)) {
    gopher_net_error = GNERR_SEND;
    return 0;
  }
  meganet_poll();
  gopher_net_error = GNERR_NONE;
  return 1;
}

/* Nonzero once the peer has finished sending or the connection is gone.
 * Bytes already received are still readable through gopher_tcp_recv_byte
 * after this reports -- "closed" is not "all data received". */
unsigned char gopher_tcp_pump(void)
{
  unsigned char st, fl;

  meganet_poll();
  st = meganet_tcp_state(&fl, 0);
  if (st == MEGANET_TCP_CLOSED)
    return 1;
  return (fl & (MEGANET_TCP_F_EOF | MEGANET_TCP_F_RESET | MEGANET_TCP_F_TIMEOUT)) ? 1 : 0;
}

void gopher_tcp_disconnect(void)
{
  unsigned int frames;

  meganet_tcp_close();
  /* Give the close a moment to complete. Best-effort: a peer that does not
   * answer must not hold us up on the way out. */
  frames = 0;
  while (frames < 60) {
    meganet_poll();
    if (tick()) {
      frames++;
      if (meganet_tcp_state(0, 0) == MEGANET_TCP_CLOSED)
        break;
    }
  }
}

/* The client reads a byte at a time; the stack hands over up to a frame
 * at a time. A small buffer in between keeps both happy. */
#define RECV_CHUNK 256
static unsigned char recvbuf[RECV_CHUNK];
static unsigned int recv_head = 0, recv_count = 0;

unsigned char gopher_tcp_recv_byte(unsigned char *out)
{
  if (recv_count == 0) {
    recv_count = meganet_tcp_recv(recvbuf, RECV_CHUNK);
    recv_head = 0;
    if (recv_count == 0)
      return 0;
  }
  *out = recvbuf[recv_head++];
  recv_count--;
  return 1;
}
