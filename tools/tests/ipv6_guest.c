/* SPDX-License-Identifier: Apache-2.0 */
/* Standalone Linux/POSIX IPv6 interoperability probe; no kernel-private ABI. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Report a failed system call or semantic check with its errno.
 * @param what Brief description of the failed operation. @return Minus one. */
static int fail(const char *what)
{
    int saved = errno;
    fprintf(stderr, "FAIL %s: errno=%d (%s)\n", what, saved, strerror(saved));
    return -1;
}

/** @brief Create a socket with finite receive and send waits.
 * @param family Socket address family. @param type Socket type. @param protocol Protocol.
 * @return Open descriptor or minus one with errno preserved. */
static int timed_socket(int family, int type, int protocol)
{
    int fd = socket(family, type, protocol);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 4};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        (type == SOCK_STREAM && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/** @brief Send every byte of a short stream message.
 * @param fd Connected socket. @param data Borrowed bytes. @param length Byte count.
 * @return Zero or minus one. */
static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;
    while (length) {
        ssize_t n = send(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

/** @brief Receive an exact short stream message, including short reads.
 * @param fd Connected socket. @param data Destination. @param length Required bytes.
 * @return Zero or minus one. */
static int read_all(int fd, void *data, size_t length)
{
    unsigned char *p = data;
    while (length) {
        ssize_t n = recv(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

/** @brief Bind an IPv6 socket to loopback or wildcard and obtain its ephemeral port.
 * @param fd Socket. @param wildcard Bind all addresses when true. @param address Output.
 * @return Zero or minus one. */
static int bind6(int fd, bool wildcard, struct sockaddr_in6 *address)
{
    *address = (struct sockaddr_in6){.sin6_family = AF_INET6,
        .sin6_addr = wildcard ? in6addr_any : in6addr_loopback};
    if (bind(fd, (const struct sockaddr *)address, sizeof(*address)) < 0) return -1;
    socklen_t length = sizeof(*address);
    if (getsockname(fd, (struct sockaddr *)address, &length) < 0) return -1;
    if (length != 28 || address->sin6_family != AF_INET6 || !address->sin6_port) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

/** @brief Confirm getifaddrs exposes the actual IPv6 loopback address.
 * @return Zero or minus one. */
static int check_interfaces(void)
{
    unsigned index = if_nametoindex("lo");
    char name[IF_NAMESIZE];
    if (!index || !if_indextoname(index,name) || strcmp(name,"lo"))
        return fail("if_nametoindex/if_indextoname");
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) < 0) return fail("getifaddrs");
    bool found = false;
    for (struct ifaddrs *it = list; it; it = it->ifa_next) {
        if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET6 &&
            IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)it->ifa_addr)->sin6_addr)) found = true;
    }
    freeifaddrs(list);
    if (!found) { errno = ENOENT; return fail("getifaddrs IPv6 ::1"); }
    puts("PASS getifaddrs IPv6 ::1");
    return 0;
}

/** @brief Exchange one IPv6 datagram and inspect address and ancillary metadata.
 * @return Zero or minus one. */
static int check_udp(void)
{
    int rx = timed_socket(AF_INET6, SOCK_DGRAM, 0);
    if (rx < 0) return fail("UDP6 receive socket");
    int tx = timed_socket(AF_INET6, SOCK_DGRAM, 0), rc = -1, one = 1;
    if (tx < 0) { fail("UDP6 send socket"); goto done; }
    if (setsockopt(rx, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)) < 0 ||
        setsockopt(rx, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one)) < 0) {
        fail("UDP6 metadata options"); goto done;
    }
    struct sockaddr_in6 address;
    if (bind6(rx, false, &address) < 0) { fail("UDP6 bind/name"); goto done; }
    static const char payload[] = "relief-ipv6-udp";
    if (sendto(tx, payload, sizeof(payload), 0, (struct sockaddr *)&address, sizeof(address)) != (ssize_t)sizeof(payload)) {
        fail("UDP6 sendto"); goto done;
    }
    char buffer[64];
    union { struct cmsghdr align; unsigned char data[CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(int))]; } control;
    struct sockaddr_in6 source = {0};
    struct iovec iov = {.iov_base = buffer, .iov_len = sizeof(buffer)};
    struct msghdr msg = {.msg_name=&source, .msg_namelen=sizeof(source), .msg_iov=&iov,
                         .msg_iovlen=1, .msg_control=control.data, .msg_controllen=sizeof(control.data)};
    ssize_t n = recvmsg(rx, &msg, 0);
    if (n < 0) { fail("UDP6 recvmsg"); goto done; }
    if (n != (ssize_t)sizeof(payload) || memcmp(buffer,payload,sizeof(payload)) ||
        msg.msg_namelen != 28 || source.sin6_family != AF_INET6 ||
        !IN6_IS_ADDR_LOOPBACK(&source.sin6_addr) || (msg.msg_flags & (MSG_TRUNC|MSG_CTRUNC))) {
        errno = EPROTO; fail("UDP6 bytes/source address"); goto done;
    }
    bool pktinfo = false, hoplimit = false;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg,c)) {
        if (c->cmsg_level != IPPROTO_IPV6) continue;
        if (c->cmsg_type == IPV6_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
            struct in6_pktinfo pi; memcpy(&pi,CMSG_DATA(c),sizeof(pi));
            pktinfo = pi.ipi6_ifindex != 0 && IN6_IS_ADDR_LOOPBACK(&pi.ipi6_addr);
        } else if (c->cmsg_type == IPV6_HOPLIMIT && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int hops; memcpy(&hops,CMSG_DATA(c),sizeof(hops)); hoplimit = hops > 0 && hops <= 255;
        }
    }
    if (!pktinfo || !hoplimit) { errno = EPROTO; fail("UDP6 PKTINFO/HOPLIMIT"); goto done; }
    puts("PASS UDP6 sockaddr/PKTINFO/HOPLIMIT"); rc = 0;
done:
    if (tx >= 0) close(tx);
    close(rx);
    return rc;
}

/** @brief Verify IPv6 stream listen, forked exchange, and both half-closes.
 * @return Zero or minus one. */
static int check_tcp(void)
{
    int listener = timed_socket(AF_INET6,SOCK_STREAM,0), peer = -1, rc = -1;
    if (listener < 0) return fail("TCP6 listener socket");
    struct sockaddr_in6 address;
    if (bind6(listener,false,&address) < 0 || listen(listener,2) < 0) { fail("TCP6 listen"); goto done; }
    fflush(NULL);
    pid_t child=fork();
    if (child < 0) { fail("TCP6 fork"); goto done; }
    if (!child) {
        alarm(8);
        int fd=timed_socket(AF_INET6,SOCK_STREAM,0);
        char reply[4], end;
        if (fd < 0 || connect(fd,(struct sockaddr *)&address,sizeof(address)) < 0 ||
            write_all(fd,"ping",4) < 0 || shutdown(fd,SHUT_WR) < 0 ||
            read_all(fd,reply,4) < 0 || memcmp(reply,"pong",4) || recv(fd,&end,1,0) != 0) _exit(1);
        close(fd); _exit(0);
    }
    peer=accept(listener,NULL,NULL);
    if (peer < 0) { fail("TCP6 accept"); goto child_done; }
    char request[4], end;
    if (read_all(peer,request,4) < 0 || memcmp(request,"ping",4) || recv(peer,&end,1,0) != 0 ||
        write_all(peer,"pong",4) < 0 || shutdown(peer,SHUT_WR) < 0) {
        if (!errno) errno=EPROTO;
        fail("TCP6 exchange/half-close"); goto child_done;
    }
    rc=0;
child_done: {
        int status;
        if (peer >= 0) { close(peer); peer=-1; }
        if (rc) kill(child,SIGTERM);
        if (waitpid(child,&status,0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (!rc) { errno=EPROTO; fail("TCP6 child exchange"); }
            rc=-1;
        }
    }
    if (!rc) puts("PASS TCP6 listen/accept/exchange/shutdown");
done:
    if (peer >= 0) close(peer);
    close(listener);
    return rc;
}

/** @brief Check IPv4-mapped traffic and the IPV6_V6ONLY listener boundary.
 * @return Zero or minus one. */
static int check_mapped(void)
{
    int listener=timed_socket(AF_INET6,SOCK_STREAM,0), client=-1, peer=-1, rc=-1;
    if (listener < 0) return fail("mapped listener socket");
    int value=0; socklen_t value_len=sizeof(value);
    if (setsockopt(listener,IPPROTO_IPV6,IPV6_V6ONLY,&value,sizeof(value)) < 0 ||
        getsockopt(listener,IPPROTO_IPV6,IPV6_V6ONLY,&value,&value_len) < 0 || value != 0) {
        if (!errno) errno=EPROTO;
        fail("IPV6_V6ONLY=0"); goto done;
    }
    struct sockaddr_in6 bound;
    if (bind6(listener,true,&bound) < 0 || listen(listener,2) < 0) { fail("mapped listen"); goto done; }
    client=timed_socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=bound.sin6_port};
    dest.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (client < 0 || connect(client,(struct sockaddr *)&dest,sizeof(dest)) < 0) { fail("mapped IPv4 connect"); goto done; }
    struct sockaddr_in6 source={0}; socklen_t source_len=sizeof(source);
    peer=accept(listener,(struct sockaddr *)&source,&source_len);
    if (peer < 0) { fail("mapped accept"); goto done; }
    char data[2];
    if (source_len != 28 || source.sin6_family != AF_INET6 || !IN6_IS_ADDR_V4MAPPED(&source.sin6_addr) ||
        write_all(client,"v4",2) < 0 || read_all(peer,data,2) < 0 || memcmp(data,"v4",2) ||
        write_all(peer,"ok",2) < 0 || read_all(client,data,2) < 0 || memcmp(data,"ok",2)) {
        if (!errno) errno=EPROTO;
        fail("mapped IPv4 exchange"); goto done;
    }
    close(peer); peer=-1; close(client); client=-1; close(listener);
    listener=timed_socket(AF_INET6,SOCK_STREAM,0); value=1;
    if (listener < 0 || setsockopt(listener,IPPROTO_IPV6,IPV6_V6ONLY,&value,sizeof(value)) < 0 ||
        bind6(listener,true,&bound) < 0 || listen(listener,2) < 0) { fail("IPV6_V6ONLY=1 listen"); goto done; }
    value=0; value_len=sizeof(value);
    if (getsockopt(listener,IPPROTO_IPV6,IPV6_V6ONLY,&value,&value_len) < 0 || value != 1) {
        if (!errno) errno=EPROTO;
        fail("IPV6_V6ONLY=1 readback"); goto done;
    }
    client=timed_socket(AF_INET,SOCK_STREAM,0); dest.sin_port=bound.sin6_port;
    if (client < 0) { fail("V6ONLY IPv4 socket"); goto done; }
    if (connect(client,(struct sockaddr *)&dest,sizeof(dest)) == 0 || errno != ECONNREFUSED) {
        errno=EPROTO; fail("V6ONLY rejects IPv4 connection"); goto done;
    }
    puts("PASS IPV6_V6ONLY and mapped IPv4 traffic"); rc=0;
done:
    if (peer >= 0) close(peer);
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    return rc;
}

/** @brief Optionally exchange an ICMPv6 echo when raw sockets are permitted.
 * @return Zero on pass or unavailable privilege/protocol, otherwise minus one. */
static int check_raw(void)
{
    int fd=timed_socket(AF_INET6,SOCK_RAW,IPPROTO_ICMPV6);
    if (fd < 0 && (errno==EPERM || errno==EACCES || errno==EAFNOSUPPORT || errno==EPROTONOSUPPORT)) {
        puts("SKIP raw ICMPv6 (unavailable or unprivileged)"); return 0;
    }
    if (fd < 0) return fail("raw ICMPv6 socket");
    struct sockaddr_in6 dest={.sin6_family=AF_INET6,.sin6_addr=IN6ADDR_LOOPBACK_INIT};
    struct icmp6_hdr request={0}; request.icmp6_type=ICMP6_ECHO_REQUEST;
    request.icmp6_id=htons((uint16_t)getpid()); request.icmp6_seq=htons(7);
    if (sendto(fd,&request,sizeof(request),0,(struct sockaddr *)&dest,sizeof(dest)) != sizeof(request)) {
        fail("raw ICMPv6 send"); close(fd); return -1;
    }
    for (unsigned i=0;i<8;++i) {
        unsigned char buffer[256]; ssize_t n=recv(fd,buffer,sizeof(buffer),0);
        if (n < 0) { fail("raw ICMPv6 receive"); close(fd); return -1; }
        if ((size_t)n >= sizeof(struct icmp6_hdr)) {
            struct icmp6_hdr reply; memcpy(&reply,buffer,sizeof(reply));
            if (reply.icmp6_type==ICMP6_ECHO_REPLY && reply.icmp6_id==request.icmp6_id &&
                reply.icmp6_seq==request.icmp6_seq) { puts("PASS raw ICMPv6 echo"); close(fd); return 0; }
        }
    }
    errno=EPROTO; fail("raw ICMPv6 echo reply"); close(fd); return -1;
}

/** @brief Optionally verify fixture DNS resolves the configured numeric peer.
 * @param numeric Expected server address. @param port Port string. @return Zero or minus one. */
static int check_dns(const struct sockaddr_in6 *numeric,const char *port)
{
    if (!getenv("IPV6_GUEST_TEST_DNS")) { puts("SKIP DNS (fixture not requested)"); return 0; }
    struct addrinfo hints={.ai_family=AF_INET6,.ai_socktype=SOCK_STREAM,.ai_flags=AI_NUMERICSERV};
    struct addrinfo *list=NULL; int rc=getaddrinfo("ipv6.reliefos.test",port,&hints,&list);
    if (rc) { fprintf(stderr,"FAIL DNS ipv6.reliefos.test: %s errno=%d\n",gai_strerror(rc),errno); return -1; }
    bool found=false;
    for (struct addrinfo *it=list;it;it=it->ai_next)
        if (it->ai_addrlen >= sizeof(struct sockaddr_in6) &&
            !memcmp(&((struct sockaddr_in6 *)it->ai_addr)->sin6_addr,&numeric->sin6_addr,sizeof(numeric->sin6_addr))) found=true;
    freeaddrinfo(list);
    if (!found) { errno=ENOENT; return fail("DNS IPv6 fixture address"); }
    puts("PASS DNS IPv6 fixture"); return 0;
}

/** @brief Exchange stream and fragmented-size datagram payloads with an IPv6 echo server.
 * @param host Numeric server IPv6 address. @param port Numeric service port.
 * @return Zero or minus one. */
static int check_network(const char *host,const char *port)
{
    struct addrinfo hints={.ai_family=AF_INET6,.ai_socktype=SOCK_STREAM,.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV};
    struct addrinfo *list=NULL; int result=getaddrinfo(host,port,&hints,&list);
    if (result) { fprintf(stderr,"FAIL numeric IPv6 endpoint: %s errno=%d\n",gai_strerror(result),errno); return -1; }
    if (!list || list->ai_addrlen != sizeof(struct sockaddr_in6)) {
        if (list) freeaddrinfo(list);
        errno=EPROTO; return fail("numeric IPv6 address length");
    }
    struct sockaddr_in6 address; memcpy(&address,list->ai_addr,sizeof(address)); freeaddrinfo(list);
    if (check_dns(&address,port) < 0) return -1;
    int fd=timed_socket(AF_INET6,SOCK_STREAM,0);
    if (fd < 0) return fail("network TCP6 socket");
    char reply[16]; static const char ping[]="ipv6-echo";
    if (connect(fd,(struct sockaddr *)&address,sizeof(address)) < 0 ||
        write_all(fd,ping,sizeof(ping)) < 0 || read_all(fd,reply,sizeof(ping)) < 0 ||
        memcmp(reply,ping,sizeof(ping))) {
        if (!errno) errno=EPROTO;
        fail("network TCP6 echo"); close(fd); return -1;
    }
    close(fd); puts("PASS network TCP6 echo");
    fd=timed_socket(AF_INET6,SOCK_DGRAM,0);
    if (fd < 0) return fail("network UDP6 socket");
    unsigned char data[4096], received[4096];
    for (size_t i=0;i<sizeof(data);++i) data[i]=(unsigned char)(i*37u+11u);
    if (sendto(fd,data,sizeof(data),0,(struct sockaddr *)&address,sizeof(address)) != sizeof(data)) {
        fail("network UDP6 4K send"); close(fd); return -1;
    }
    ssize_t n=recv(fd,received,sizeof(received),0);
    if (n != sizeof(received) || memcmp(data,received,sizeof(data))) {
        if (n>=0) errno=EPROTO;
        fail("network UDP6 4K echo"); close(fd); return -1;
    }
    puts("PASS network UDP6 4K echo");
    unsigned char large[8192], returned[8192];
    for (size_t i=0;i<sizeof large;++i) large[i]=(unsigned char)(i*19u+3u);
    if (connect(fd,(struct sockaddr *)&address,sizeof address) < 0 ||
        write(fd,large,sizeof large) != sizeof large ||
        read(fd,returned,sizeof returned) != sizeof returned ||
        memcmp(large,returned,sizeof large)) {
        if (!errno) errno=EPROTO;
        fail("network UDP6 8K read/write atomicity"); close(fd); return -1;
    }
    puts("PASS network UDP6 8K read/write atomicity");
    struct iovec vectors[2] = {{.iov_base=large,.iov_len=3072},
                              {.iov_base=large+3072,.iov_len=sizeof large-3072}};
    struct iovec destinations[2] = {{.iov_base=returned,.iov_len=2048},
                                   {.iov_base=returned+2048,.iov_len=sizeof returned-2048}};
    memset(returned,0,sizeof returned);
    if (writev(fd,vectors,2) != sizeof large ||
        readv(fd,destinations,2) != sizeof returned || memcmp(large,returned,sizeof large)) {
        if (!errno) errno=EPROTO;
        fail("network UDP6 vector datagram atomicity"); close(fd); return -1;
    }
    close(fd); puts("PASS network UDP6 vector datagram atomicity"); return 0;
}

/** @brief Report a bounded probe timeout without calling non-signal-safe stdio.
 * @param signum Signal number. This handler terminates the process. */
static void timeout_exit(int signum)
{
    static const char text[]="FAIL probe timeout\n";
    (void)signum;
    (void)write(STDERR_FILENO,text,sizeof(text)-1);
    _exit(124);
}

/** @brief Select a bounded loopback or external echo-server interoperability run.
 * @param argc Argument count. @param argv Argument vector. @return Process status. */
int main(int argc,char **argv)
{
    signal(SIGPIPE,SIG_IGN);
    signal(SIGALRM,timeout_exit);
    if (argc==2 && !strcmp(argv[1],"--loopback")) {
        alarm(30);
        return check_interfaces() || check_udp() || check_tcp() || check_mapped() || check_raw() ? 1 : 0;
    }
    if (argc==4 && !strcmp(argv[1],"--network")) {
        alarm(25);
        return check_network(argv[2],argv[3]) ? 1 : 0;
    }
    fprintf(stderr,"usage: %s --loopback | --network NUMERIC_IPV6 PORT\n",argv[0]);
    return 2;
}
