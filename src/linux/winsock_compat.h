#pragma once
// Linux: the Winsock xnet.cpp is written against, on BSD sockets. The guest gets Winsock's error codes and passes
// Winsock's option values, both translated here; the calls with Winsock's int lengths are overloads beside libc's.
#ifdef _WIN32
#error "winsock_compat.h is for the non-Windows builds"
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "linux/win32_compat.h"

typedef int SOCKET;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;

struct WSADATA {
    WORD wVersion;
};
#define MAKEWORD(a, b) (WORD(uint8_t(a) | (uint16_t(uint8_t(b)) << 8)))
inline int WSAStartup(WORD, WSADATA*) { return 0; }
inline int closesocket(SOCKET s) { return close(s); }

// Winsock's error codes, which the guest sees
enum : int {
    WSA_OPERATION_ABORTED = 995, WSA_IO_INCOMPLETE = 996, WSA_IO_PENDING = 997, WSAEINTR = 10004, WSAEBADF = 10009,
    WSAEACCES = 10013, WSAEFAULT = 10014, WSAEINVAL = 10022, WSAEMFILE = 10024, WSAEWOULDBLOCK = 10035,
    WSAEINPROGRESS = 10036, WSAEALREADY = 10037, WSAENOTSOCK = 10038, WSAEDESTADDRREQ = 10039, WSAEMSGSIZE = 10040,
    WSAEPROTOTYPE = 10041, WSAENOPROTOOPT = 10042, WSAEPROTONOSUPPORT = 10043, WSAESOCKTNOSUPPORT = 10044,
    WSAEOPNOTSUPP = 10045, WSAEAFNOSUPPORT = 10047, WSAEADDRINUSE = 10048, WSAEADDRNOTAVAIL = 10049,
    WSAENETDOWN = 10050, WSAENETUNREACH = 10051, WSAENETRESET = 10052, WSAECONNABORTED = 10053,
    WSAECONNRESET = 10054, WSAENOBUFS = 10055, WSAEISCONN = 10056, WSAENOTCONN = 10057, WSAESHUTDOWN = 10058,
    WSAETIMEDOUT = 10060, WSAECONNREFUSED = 10061, WSAEHOSTUNREACH = 10065, WSANOTINITIALISED = 10093,
    WSAHOST_NOT_FOUND = 11001,
};

inline int WsaError(int e) {
    switch (e) {
        case 0: return 0;
        case EINTR: return WSAEINTR;
        case EBADF: return WSAENOTSOCK;
        case EACCES: return WSAEACCES;
        case EFAULT: return WSAEFAULT;
        case EINVAL: return WSAEINVAL;
        case EMFILE: return WSAEMFILE;
        case EAGAIN: return WSAEWOULDBLOCK;
        // a nonblocking connect under way is WSAEWOULDBLOCK on Winsock
        case EINPROGRESS: return WSAEWOULDBLOCK;
        case EALREADY: return WSAEALREADY;
        case ENOTSOCK: return WSAENOTSOCK;
        case EDESTADDRREQ: return WSAEDESTADDRREQ;
        case EMSGSIZE: return WSAEMSGSIZE;
        case EPROTOTYPE: return WSAEPROTOTYPE;
        case ENOPROTOOPT: return WSAENOPROTOOPT;
        case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
        case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
        case EOPNOTSUPP: return WSAEOPNOTSUPP;
        case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
        case EADDRINUSE: return WSAEADDRINUSE;
        case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
        case ENETDOWN: return WSAENETDOWN;
        case ENETUNREACH: return WSAENETUNREACH;
        case ENETRESET: return WSAENETRESET;
        case ECONNABORTED: return WSAECONNABORTED;
        // a port unreachable reply to an earlier send: Winsock's UDP reports it as a reset
        case ECONNREFUSED: return WSAECONNRESET;
        case ECONNRESET: return WSAECONNRESET;
        case EPIPE: return WSAECONNRESET;
        case ENOBUFS: return WSAENOBUFS;
        case EISCONN: return WSAEISCONN;
        case ENOTCONN: return WSAENOTCONN;
        case ESHUTDOWN: return WSAESHUTDOWN;
        case ETIMEDOUT: return WSAETIMEDOUT;
        case EHOSTUNREACH: return WSAEHOSTUNREACH;
        default: return WSAEINVAL;
    }
}
inline int WSAGetLastError() { return WsaError(errno); }

// Winsock's ioctl commands
constexpr uint32_t kWsaFionbio = 0x8004667E;
constexpr uint32_t kWsaFionread = 0x4004667F;
inline int ioctlsocket(SOCKET s, long cmd, u_long* v) {
    if (uint32_t(cmd) == kWsaFionbio || cmd == long(FIONBIO)) {
        int fl = fcntl(s, F_GETFL, 0);
        if (fl < 0) return -1;
        return fcntl(s, F_SETFL, *v ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) < 0 ? -1 : 0;
    }
    if (uint32_t(cmd) == kWsaFionread || cmd == long(FIONREAD)) {
        int n = 0;
        if (ioctl(s, FIONREAD, &n) < 0) return -1;
        *v = u_long(n);
        return 0;
    }
    errno = EINVAL;
    return -1;
}
// SIO_UDP_CONNRESET and the like: BSD sockets report no resets on unconnected UDP sockets anyway
inline int WSAIoctl(SOCKET, DWORD, void*, DWORD, void*, DWORD, DWORD* ret, void*, void*) {
    if (ret) *ret = 0;
    return 0;
}

// Winsock's levels and options, as the guest passes them, to the host's; -1 for one with no counterpart
namespace wsa {
constexpr int kSolSocket = 0xFFFF;
inline bool SocketOption(int level, int opt, int& host_opt) {
    if (level != kSolSocket) return false;
    switch (opt) {
        case 0x0004: host_opt = SO_REUSEADDR; break;
        case 0x0008: host_opt = SO_KEEPALIVE; break;
        case 0x0010: host_opt = SO_DONTROUTE; break;
        case 0x0020: host_opt = SO_BROADCAST; break;
        case 0x0080: host_opt = SO_LINGER; break;
        case 0x0100: host_opt = SO_OOBINLINE; break;
        case 0x1001: host_opt = SO_SNDBUF; break;
        case 0x1002: host_opt = SO_RCVBUF; break;
        case 0x1005: host_opt = SO_SNDTIMEO; break;
        case 0x1006: host_opt = SO_RCVTIMEO; break;
        case 0x1007: host_opt = SO_ERROR; break;
        case 0x1008: host_opt = SO_TYPE; break;
        default: host_opt = -1; break;
    }
    return true;
}
}  // namespace wsa

inline int setsockopt(SOCKET s, int level, int opt, const char* value, int len) {
    int host_opt = opt;
    if (wsa::SocketOption(level, opt, host_opt)) {
        if (host_opt < 0) return 0;  // nothing to set here (SO_EXCLUSIVEADDRUSE and the like)
        if (host_opt == SO_LINGER && len >= 4) {
            // Winsock's linger is two u_shorts
            uint16_t w[2];
            std::memcpy(w, value, 4);
            linger l{w[0], w[1]};
            return ::setsockopt(s, SOL_SOCKET, SO_LINGER, static_cast<const void*>(&l), socklen_t(sizeof(l)));
        }
        if ((host_opt == SO_RCVTIMEO || host_opt == SO_SNDTIMEO) && len >= 4) {
            // Winsock's timeouts are DWORD milliseconds
            uint32_t ms;
            std::memcpy(&ms, value, 4);
            timeval tv{time_t(ms / 1000), suseconds_t((ms % 1000) * 1000)};
            return ::setsockopt(s, SOL_SOCKET, host_opt, static_cast<const void*>(&tv), socklen_t(sizeof(tv)));
        }
        return ::setsockopt(s, SOL_SOCKET, host_opt, static_cast<const void*>(value), socklen_t(len));
    }
    return ::setsockopt(s, level, opt, static_cast<const void*>(value), socklen_t(len));
}

inline int getsockopt(SOCKET s, int level, int opt, char* value, int* len) {
    int host_opt = opt;
    int host_level = level;
    if (wsa::SocketOption(level, opt, host_opt)) {
        if (host_opt < 0) {
            errno = ENOPROTOOPT;
            return -1;
        }
        host_level = SOL_SOCKET;
    }
    socklen_t l = socklen_t(*len);
    int r = ::getsockopt(s, host_level, host_opt, static_cast<void*>(value), &l);
    *len = int(l);
    // the pending error as Winsock numbers it
    if (r == 0 && host_level == SOL_SOCKET && host_opt == SO_ERROR && l >= sizeof(int)) {
        int e;
        std::memcpy(&e, value, sizeof(e));
        e = WsaError(e);
        std::memcpy(value, &e, sizeof(e));
    }
    return r;
}

inline int getsockname(SOCKET s, sockaddr* a, int* len) {
    socklen_t l = socklen_t(*len);
    int r = ::getsockname(s, a, &l);
    *len = int(l);
    return r;
}
inline SOCKET accept(SOCKET s, sockaddr* a, int* len) {
    socklen_t l = socklen_t(*len);
    SOCKET r = ::accept(s, a, &l);
    *len = int(l);
    return r;
}
inline int recvfrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
    socklen_t l = socklen_t(*fromlen);
    int r = int(::recvfrom(s, static_cast<void*>(buf), size_t(len), flags, from, &l));
    *fromlen = int(l);
    return r;
}
inline int recv(SOCKET s, char* buf, int len, int flags) {
    return int(::recv(s, static_cast<void*>(buf), size_t(len), flags));
}
// a closed peer is an error, not SIGPIPE
inline int send(SOCKET s, const char* buf, int len, int flags) {
    return int(::send(s, static_cast<const void*>(buf), size_t(len), flags | MSG_NOSIGNAL));
}
inline int sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    return int(::sendto(s, static_cast<const void*>(buf), size_t(len), flags | MSG_NOSIGNAL, to, socklen_t(tolen)));
}

// select's first argument is ignored by Winsock and the highest descriptor plus one here
inline int HostSelect(fd_set* r, fd_set* w, fd_set* e, timeval* t) { return ::select(FD_SETSIZE, r, w, e, t); }
