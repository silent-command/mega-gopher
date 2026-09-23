/* Networking layer for the gopher client, built on mega-net through its
 * jump-table ABI (see ../mega-net/src/abi/meganet.h). The interface is
 * unchanged from the earlier stack's version so that the client above it is
 * unchanged: that is the point of the parity test. */
#ifndef GOPHER_NET_H
#define GOPHER_NET_H

/* --- Failure reasons --------------------------------------------------
 *
 * "ERROR: connection failed" covered at least five distinct causes, which
 * made it impossible to tell a fault in this client from a server that
 * was simply down. Each entry point below records why it gave up, so the
 * UI can say something the user can act on.
 *
 * mega-net retries lost queries and SYNs itself, so the "no reply" codes
 * surface only once the stack has given up. */
#define GNERR_NONE 0
#define GNERR_DNS_BUSY 1      /* stack refused to send the query at all */
#define GNERR_DNS_NOREPLY 2   /* no answer before we gave up */
#define GNERR_DNS_NOTFOUND 3  /* server answered: no such host */
#define GNERR_CONN_REFUSED 4  /* host answered, but rejected the port */
#define GNERR_CONN_NOREPLY 5  /* nothing came back at all */
#define GNERR_SEND 6          /* request could not be handed to the stack */
#define GNERR_NO_DATA 7       /* connected and asked, but nothing came back */

/* Set whenever one of the calls below returns 0. */
extern unsigned char gopher_net_error;

/* A short, screen-safe description of gopher_net_error. */
const char *gopher_net_strerror(void);

/* How many attempts the last call actually made, for diagnostics: 1 means
 * it worked first time, >1 means the network dropped something and the
 * retry saved it. */
extern unsigned char gopher_net_attempts;

/* Brings up the ethernet controller and obtains a DHCP lease. Blocks
 * (frame-paced) until done or it gives up. Returns 1 on success. */
unsigned char gopher_net_init(void);

/* Resolves hostname to a 4-byte IP (out[0..3]). Returns 1 on success. */
unsigned char gopher_dns_resolve(const char *hostname, unsigned char *out);

/* Opens a TCP connection to ip:port. Returns 1 if established. */
unsigned char gopher_tcp_connect(const unsigned char *ip, unsigned int port);

/* Sends a gopher selector (raw bytes, not null-terminated requirement)
 * followed by CRLF. Returns 1 if all bytes were accepted. */
unsigned char gopher_tcp_send_selector(const char *selector);

/* Polls the connection once (call every real frame). Returns 0 while
 * steady/connected, nonzero when the peer closed or another TCP event
 * fired -- treat nonzero as "no more data coming", and keep reading until
 * gopher_tcp_recv_byte() runs dry. */
unsigned char gopher_tcp_pump(void);

/* Non-blocking: returns 1 and sets *out if a byte was available this
 * call, 0 if the receive buffer was empty. */
unsigned char gopher_tcp_recv_byte(unsigned char *out);

#endif

/* Closes any open TCP connection, so quitting does not leave the server
 * holding a half-open socket. Safe to call when nothing is connected. */
void gopher_tcp_disconnect(void);

/* Drops any cached address for this hostname. Call when a connection
 * fails, so a stale cached address cannot strand the user. */
void gopher_dns_invalidate(const char *hostname);
