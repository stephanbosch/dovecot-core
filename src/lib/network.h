#ifndef __NETWORK_H
#define __NETWORK_H

#ifndef WIN32
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#endif

#ifdef HAVE_SOCKS_H
#include <socks.h>
#endif

#ifndef AF_INET6
#  ifdef PF_INET6
#    define AF_INET6 PF_INET6
#  else
#    define AF_INET6 10
#  endif
#endif

struct _IPADDR {
	unsigned short family;
#ifdef HAVE_IPV6
	struct in6_addr ip;
#else
	struct in_addr ip;
#endif
};

/* maxmimum string length of IP address */
#ifdef HAVE_IPV6
#  define MAX_IP_LEN INET6_ADDRSTRLEN
#else
#  define MAX_IP_LEN 20
#endif

#define IPADDR_IS_V4(ip) ((ip)->family == AF_INET)
#define IPADDR_IS_V6(ip) ((ip)->family == AF_INET6)

/* returns 1 if IPADDRs are the same */
int net_ip_compare(IPADDR *ip1, IPADDR *ip2);

/* Connect to socket with ip address */
int net_connect_ip(IPADDR *ip, int port, IPADDR *my_ip);
/* Connect to named UNIX socket */
int net_connect_unix(const char *path);
/* Disconnect socket */
void net_disconnect(int fd);
/* Try to let the other side close the connection, if it still isn't
   disconnected after certain amount of time, close it ourself */
void net_disconnect_later(int fd);

/* Set socket blocking/nonblocking */
void net_set_nonblock(int fd, int nonblock);
/* Set TCP_CORK if supported, ie. don't send out partial frames. */
void net_set_cork(int fd, int cork);

/* Listen for connections on a socket */
int net_listen(IPADDR *my_ip, int *port);
/* Listen for connections on an UNIX socket */
int net_listen_unix(const char *path);
/* Accept a connection on a socket */
int net_accept(int fd, IPADDR *addr, int *port);

/* Read data from socket, return number of bytes read, -1 = error */
int net_receive(int fd, void *buf, unsigned int len);
/* Transmit data, return number of bytes sent, -1 = error */
int net_transmit(int fd, const void *data, unsigned int len);

/* Get IP addresses for host. ips contains ips_count of IPs, they don't need
   to be free'd. Returns 0 = ok, others = error code for net_gethosterror() */
int net_gethostbyname(const char *addr, IPADDR **ips, int *ips_count);
/* get error of net_gethostname() */
const char *net_gethosterror(int error);
/* return TRUE if host lookup failed because it didn't exist (ie. not
   some error with name server) */
int net_hosterror_notfound(int error);

/* Get socket address/port */
int net_getsockname(int fd, IPADDR *addr, int *port);

/* IPADDR -> char* translation. `host' must be at least MAX_IP_LEN bytes */
int net_ip2host(IPADDR *ip, char *host);
/* char* -> IPADDR translation. */
int net_host2ip(const char *host, IPADDR *ip);

/* Get socket error */
int net_geterror(int fd);

/* Get name of TCP service */
char *net_getservbyport(int port);

int is_ipv4_address(const char *host);
int is_ipv6_address(const char *host);

#endif
