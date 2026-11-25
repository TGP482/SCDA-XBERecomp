// System link on host Winsock. The game reaches the network only through the XNet winsock thunks, replaced
// here. The XNet object [0x5E7784] stays null, so the XNet calls not replaced (Live QoS and the like) fail with
// WSANOTINITIALISED on their own, and the XOnline calls with XONLINE_E_NOT_INITIALIZED.
// Addresses: an XNADDR's ina is the peer's real IPv4, so secure addresses map to IPs one to one.
// With --instance N the game binds 127.0.0.N and a broadcast goes to the other loopback instances.
// Join IP (a host past the LAN): the system link search's broadcasts also go to that address, and the host's
// broadcasts (LAN discovery answers every search with one, to 255.255.255.255:9100) go to the public addresses
// that have reached it. With one peer
// (co-op) a LAN address in an XNADDR, which only means something on its owner's LAN, stands for the peer's public
// one: the join address on the client, the address its packets come from on the host. Ports a NAT rewrote are
// answered on the port the peer's packets came from.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// select's first argument is ignored by Winsock
inline int HostSelect(fd_set* r, fd_set* w, fd_set* e, timeval* t) { return ::select(0, r, w, e, t); }
#else
#include "linux/winsock_compat.h"
#endif

#include <algorithm>
#include <array>
#include <cwchar>
#include <random>

#include "app/app.h"
#include "audio/audio.h"
#include "xdk/hle.h"

using kernel::HostPtr;
using kernel::R32;
using kernel::W32;

namespace {

// XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
constexpr uint32_t kXnAddrSize = 0x24;
constexpr uint32_t kXnAddrEnet = 0x0A;
// WSAOVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent
constexpr uint32_t kOvlStatus = 0x00;
constexpr uint32_t kOvlBytes = 0x04;
constexpr uint32_t kOvlEvent = 0x10;
constexpr uint32_t kOvlPending = 0x103;
// XNDNS: iStatus, cina, aina[8]
constexpr uint32_t kXnDnsSize = 0x28;
// fd_set: fd_count, fd_array[64]
constexpr uint32_t kFdSetMax = 64;

constexpr uint32_t kSocketError = 0xFFFFFFFF;
constexpr uint32_t kLinkUp = 0x01 | 0x02 | 0x08;          // active, 100 Mbps, full duplex
constexpr uint32_t kXnAddrEthernet = 0x02 | 0x04;         // ethernet, static address
constexpr DWORD kUdpConnReset = 0x9800000C;  // SIO_UDP_CONNRESET: off, or a send to a closed port fails the next recv
constexpr int kLoopbackInstances = 8;
constexpr uint32_t kVdpProtocol = 254;
constexpr uint32_t kGuestFionbio = 0x8004667E;  // Winsock's FIONBIO, as the guest passes it  // IPPROTO_VDP: game data and voice in one datagram

struct Socket {
    SOCKET s = INVALID_SOCKET;
    bool udp = false;
    bool nonblocking = false;  // the guest's FIONBIO; the host socket is always nonblocking
    uint16_t port = 0;         // bound, network order
    bool vdp = false;          // the voice link follows the game's VDP sends (co-op: its voice socket; versus: the game's)
};

struct Span {
    uint32_t ptr, len;
};

// the byte count and flags pointers are only written when the call completes at once, as in Winsock:
// Quazal passes its flags on the stack
struct PendingRecv {
    uint32_t sock, ovl, from, fromlen;
    std::vector<Span> spans;  // copied: Quazal's WSABUF array lives on its stack
};

std::mutex g_lock;
std::unordered_map<uint32_t, Socket> g_sockets;
uint32_t g_next_socket = 0x100;
std::vector<PendingRecv> g_pending;
// peer address -> the VDP socket and port the game last sent to it, for the voice link's packets
struct VdpRoute {
    uint32_t sock;
    uint16_t port;
};
std::mutex g_route_lock;
std::unordered_map<uint32_t, VdpRoute> g_routes;

std::once_flag g_init;
int g_instance = 0;
uint32_t g_local = 0;  // network order
std::array<uint8_t, 8> g_kid{};
std::unordered_map<uint32_t, std::array<uint8_t, kXnAddrSize>> g_peers;

thread_local uint32_t t_error = 0;

std::atomic<uint32_t> g_remote{0};  // the last public address a packet came from
// a session: the game sent another machine a packet this recently (lockstep and PRUDP keep sending)
constexpr uint64_t kSessionHoldMs = 10000;
std::atomic<uint64_t> g_peer_sent{0};  // GetTickCount64 of the last one
std::mutex g_nat_lock;
std::unordered_map<uint64_t, uint16_t> g_nat;  // (peer address, our port) -> the port its packets come from
std::vector<uint32_t> g_public;                // public addresses packets came from, the latest last
constexpr size_t kMaxPublic = 8;

// Co-op settings beside the game's PRUDP traffic (UDP 1000): a host sends its tick rate and input delay floor to the
// peers the game talks to there, on the same socket, and a client takes them out before the game sees them
constexpr uint16_t kPrudpPort = 1000;
constexpr char kCoopMagic[4] = {'S', 'C', 'D', 'C'};
constexpr uint8_t kCoopVersion = 1;
constexpr int kCoopPacket = 16;            // magic, version, 3 spare, rate, floor
constexpr uint64_t kCoopPeerHoldMs = 5000;  // a peer the game sent PRUDP to this recently is sent ours
constexpr uint64_t kCoopSendMs = 250;
struct CoopPeer {
    uint32_t sock;
    uint16_t port;
    uint64_t at;
};
std::mutex g_coop_lock;
std::unordered_map<uint32_t, CoopPeer> g_coop_peers;  // address -> where the game last sent it PRUDP
bool (*g_coop_host)(uint32_t& rate, uint32_t& floor) = nullptr;
std::atomic<uint64_t> g_coop_values{0};  // the host's: 1 << 63 | rate << 32 | floor, 0 for none

void StoreCoop(uint32_t rate, uint32_t floor) { g_coop_values = 1ull << 63 | uint64_t(rate & 0x7FFFFFFF) << 32 | floor; }

// a host's settings packet: stored, and kept from the game
bool TakeCoopPacket(const uint8_t* data, int len) {
    if (len != kCoopPacket || std::memcmp(data, kCoopMagic, 4) || data[4] != kCoopVersion) return false;
    uint32_t rate, floor;
    std::memcpy(&rate, data + 8, 4);
    std::memcpy(&floor, data + 12, 4);
    uint64_t before = g_coop_values;
    StoreCoop(rate, floor);
    if (g_coop_values != before) LOGI("xnet: the host's co-op tick rate %u, input delay floor %u ms", rate, floor);
    return true;
}

std::string Ip(uint32_t a) {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    return s;
}
std::string Addr(const sockaddr_in& a) { return Ip(a.sin_addr.s_addr) + ":" + std::to_string(ntohs(a.sin_port)); }

bool Private(uint32_t a) {
    uint8_t b0 = a & 0xFF, b1 = (a >> 8) & 0xFF;
    return b0 == 10 || b0 == 127 || (b0 == 172 && (b1 & 0xF0) == 16) || (b0 == 192 && b1 == 168) ||
           (b0 == 169 && b1 == 254);
}

uint64_t NatKey(uint32_t address, uint16_t port) { return uint64_t(address) << 16 | port; }

// the F10 join address, resolved when it changes; 0 for none
uint32_t JoinAddress() {
    static std::mutex m;
    static std::string last;
    static uint32_t address = 0;
    std::string text;
    {
        std::lock_guard<std::recursive_mutex> ui(app::UiLock());
        text = app::Get().join_ip;
    }
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    std::lock_guard<std::mutex> lock(m);
    if (text == last) return address;
    last = text;
    address = 0;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (!text.empty() && getaddrinfo(text.c_str(), nullptr, &hints, &res) == 0) {
        address = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
    }
    if (!text.empty()) LOGI("xnet: join %s -> %s", text.c_str(), address ? Ip(address).c_str() : "not found");
    return address;
}

uint32_t PrimaryAddress() {
    uint32_t addr = htonl(INADDR_LOOPBACK);
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);  // no packet is sent: connect only picks the route
    sockaddr_in me{};
    int len = sizeof(me);
    if (s != INVALID_SOCKET && ::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr)
        addr = me.sin_addr.s_addr;
    if (s != INVALID_SOCKET) ::closesocket(s);
    return addr;
}

void Pump();

void Init() {
    std::call_once(g_init, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_instance = app::Instance();
        g_local = g_instance ? htonl(0x7F000000u | uint32_t(g_instance)) : PrimaryAddress();
        LOGI("xnet: %s address %s", g_instance ? "loopback instance" : "LAN", Ip(g_local).c_str());
        std::thread(Pump).detach();
    });
}

uint32_t Fail(int err) {
    t_error = uint32_t(err);
    return kSocketError;
}
int HostError() {
    int e = WSAGetLastError();
    return e == WSAECONNRESET ? WSAEWOULDBLOCK : e;  // a port unreachable reply from an earlier send
}

Socket Find(uint32_t sock) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(sock);
    return it == g_sockets.end() ? Socket{} : it->second;
}

sockaddr_in GuestAddr(uint32_t va) {
    sockaddr_in a{};
    std::memcpy(&a, HostPtr(va), sizeof(a));
    return a;
}

void Signal(uint32_t event) {
    if (!event) return;
    if (auto o = kernel::FromHandle(event)) kernel::SetEvent(*o);
}

bool Wait(SOCKET s, bool write, int ms) {
    fd_set f;
    FD_ZERO(&f);
    FD_SET(s, &f);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return HostSelect(write ? nullptr : &f, write ? &f : nullptr, nullptr, &tv) > 0;
}

std::vector<Span> Spans(uint32_t bufs, uint32_t count) {
    std::vector<Span> v;
    for (uint32_t i = 0; i < count; i++) v.push_back({R32(bufs + 8 * i + 4), R32(bufs + 8 * i)});
    return v;
}

// one datagram into guest buffers: bytes, or -1 with err
int Recv(uint32_t sock, SOCKET s, const std::vector<Span>& spans, uint32_t from, uint32_t fromlen, int& err) {
    thread_local std::vector<uint8_t> buf(65536);
    sockaddr_in src{};
    int sl = sizeof(src);
    int n;
    for (;;) {
        sl = sizeof(src);
        n = ::recvfrom(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&src), &sl);
        if (n < 0) {
            err = HostError();
            if (err != WSAEWOULDBLOCK) LOG_FIRST(20, "xnet: recv %X failed (%d)", sock, err);
            return -1;
        }
        // the voice link shares the game's VDP sockets and the co-op settings its PRUDP one; theirs never reach the game
        if (!audio::TakeVoiceLinkPacket(src.sin_addr.s_addr, buf.data(), n) && !TakeCoopPacket(buf.data(), n)) break;
    }
    int done = 0;
    for (const Span& sp : spans) {
        int c = std::min<int>(n - done, int(sp.len));
        if (c <= 0) break;
        std::memcpy(HostPtr(sp.ptr), buf.data() + done, size_t(c));
        done += c;
    }
    if (done < n) LOG_FIRST(10, "xnet: %d byte datagram cut to %d", n, done);
    uint32_t peer = src.sin_addr.s_addr;
    if (peer != g_local && !Private(peer)) {
        if (g_remote.exchange(peer) != peer) LOGI("xnet: peer %s", Addr(src).c_str());
        auto it = g_sockets.find(sock);
        std::lock_guard<std::mutex> lock(g_nat_lock);
        if (it != g_sockets.end() && it->second.port) g_nat[NatKey(peer, it->second.port)] = src.sin_port;
        std::erase(g_public, peer);
        g_public.push_back(peer);
        if (g_public.size() > kMaxPublic) g_public.erase(g_public.begin());
    }
    if (from) std::memcpy(HostPtr(from), &src, sizeof(src));
    if (fromlen) W32(fromlen, sizeof(src));
    return done;
}

int SendOnce(SOCKET s, const uint8_t* data, int len, const sockaddr_in* to) {
    if (!to) return ::send(s, reinterpret_cast<const char*>(data), len, 0);
    sockaddr_in d = *to;
    if (d.sin_addr.s_addr == INADDR_BROADCAST) {
        std::vector<sockaddr_in> also;
        if (uint32_t join = JoinAddress()) {
            also.push_back(d);
            also.back().sin_addr.s_addr = join;
        }
        {
            std::lock_guard<std::mutex> lock(g_nat_lock);
            for (uint32_t peer : g_public) {
                also.push_back(d);
                also.back().sin_addr.s_addr = peer;
                if (auto it = g_nat.find(NatKey(peer, d.sin_port)); it != g_nat.end()) also.back().sin_port = it->second;
            }
        }
        for (const sockaddr_in& a : also)
            ::sendto(s, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    } else {
        std::lock_guard<std::mutex> lock(g_nat_lock);
        auto it = g_nat.find(NatKey(d.sin_addr.s_addr, d.sin_port));
        if (it != g_nat.end()) d.sin_port = it->second;
    }
    if (g_instance && d.sin_addr.s_addr == INADDR_BROADCAST) {
        for (int i = 1; i <= kLoopbackInstances; i++) {
            if (i == g_instance) continue;
            d.sin_addr.s_addr = htonl(0x7F000000u | uint32_t(i));
            ::sendto(s, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<sockaddr*>(&d), sizeof(d));
        }
        return len;
    }
    return ::sendto(s, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<const sockaddr*>(&d), sizeof(d));
}

int Send(uint32_t sock, SOCKET s, const uint8_t* data, int len, const sockaddr_in* to) {
    int n = SendOnce(s, data, len, to);
    bool unicast = !to || (to->sin_addr.s_addr != INADDR_BROADCAST && to->sin_addr.s_addr != g_local);
    if (n >= 0 && unicast) g_peer_sent = GetTickCount64();
    if (n >= 0 && to && to->sin_addr.s_addr != INADDR_BROADCAST && Find(sock).port == htons(kPrudpPort)) {
        std::lock_guard<std::mutex> lock(g_coop_lock);
        g_coop_peers[to->sin_addr.s_addr] = {sock, to->sin_port, GetTickCount64()};
    }
    if (n < 0) LOG_FIRST(20, "xnet: send %X failed (%d)", sock, WSAGetLastError());
    else if (to && to->sin_addr.s_addr != INADDR_BROADCAST && Find(sock).vdp) {
        {
            std::lock_guard<std::mutex> lock(g_route_lock);
            g_routes[to->sin_addr.s_addr] = {sock, to->sin_port};
        }
        audio::VoiceSent(to->sin_addr.s_addr);
    }
    return n;
}

std::vector<uint8_t> Gather(uint32_t bufs, uint32_t count) {
    std::vector<uint8_t> v;
    for (const Span& sp : Spans(bufs, count)) {
        auto p = static_cast<const uint8_t*>(HostPtr(sp.ptr));
        v.insert(v.end(), p, p + sp.len);
    }
    return v;
}

void Complete(uint32_t ovl, uint32_t status, uint32_t bytes) {
    W32(ovl + kOvlBytes, bytes);
    W32(ovl + kOvlStatus, status);
    Signal(R32(ovl + kOvlEvent));
}

// with g_lock held: false while no datagram is waiting
bool TryComplete(PendingRecv& p) {
    auto it = g_sockets.find(p.sock);
    if (it == g_sockets.end()) {
        Complete(p.ovl, WSAENOTSOCK, 0);
        return true;
    }
    int err = 0;
    int n = Recv(p.sock, it->second.s, p.spans, p.from, p.fromlen, err);
    if (n < 0 && err == WSAEWOULDBLOCK) return false;
    Complete(p.ovl, n < 0 ? uint32_t(err) : 0, n < 0 ? 0 : uint32_t(n));
    return true;
}

// the host's settings to its PRUDP peers, every kCoopSendMs while the game says it hosts
void SendCoop() {
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    uint32_t rate, floor;
    if (now - last < kCoopSendMs || !g_coop_host || !g_coop_host(rate, floor)) return;
    last = now;
    uint8_t packet[kCoopPacket] = {};
    std::memcpy(packet, kCoopMagic, 4);
    packet[4] = kCoopVersion;
    std::memcpy(packet + 8, &rate, 4);
    std::memcpy(packet + 12, &floor, 4);
    std::vector<std::pair<uint32_t, CoopPeer>> peers;
    {
        std::lock_guard<std::mutex> lock(g_coop_lock);
        for (const auto& [address, peer] : g_coop_peers)
            if (now - peer.at < kCoopPeerHoldMs) peers.emplace_back(address, peer);
    }
    for (const auto& [address, peer] : peers) {
        Socket k = Find(peer.sock);
        if (k.s == INVALID_SOCKET) continue;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = peer.port;
        to.sin_addr.s_addr = address;
        SendOnce(k.s, packet, kCoopPacket, &to);
        LOG_FIRST(4, "xnet: our co-op tick rate %u, input delay floor %u ms to %s", rate, floor, Ip(address).c_str());
    }
}

void Pump() {
    for (;;) {
        SendCoop();
        fd_set set;
        FD_ZERO(&set);
        uint32_t count = 0;  // fd_set's own count is Winsock's alone
        {
            std::lock_guard<std::mutex> lock(g_lock);
            for (auto& p : g_pending) {
                auto it = g_sockets.find(p.sock);
                if (it != g_sockets.end() && count < FD_SETSIZE) {
                    FD_SET(it->second.s, &set);
                    count++;
                }
            }
        }
        if (!count) {
            Sleep(1);
            continue;
        }
        timeval tv{0, 2000};
        if (HostSelect(&set, nullptr, nullptr, &tv) <= 0) {
            Sleep(1);
            continue;
        }
        std::lock_guard<std::mutex> lock(g_lock);
        for (size_t i = 0; i < g_pending.size();) {
            auto it = g_sockets.find(g_pending[i].sock);
            bool ready = it == g_sockets.end() || FD_ISSET(it->second.s, &set);
            if (ready && TryComplete(g_pending[i]))
                g_pending.erase(g_pending.begin() + ptrdiff_t(i));
            else
                i++;
        }
    }
}

void OwnXnAddr(uint8_t* x) {
    std::memset(x, 0, kXnAddrSize);
    std::memcpy(x, &g_local, 4);
    const uint8_t mac[6] = {0x00, 0x50, 0xF2, uint8_t(g_local >> 8), uint8_t(g_local >> 16), uint8_t(g_local >> 24)};
    std::memcpy(x + kXnAddrEnet, mac, 6);
}

void Random(uint32_t va, uint32_t n) {
    static std::mutex m;
    static std::mt19937 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(m);
    auto p = static_cast<uint8_t*>(HostPtr(va));
    for (uint32_t i = 0; i < n; i++) p[i] = uint8_t(rng());
}

}  // namespace

namespace xnet {

// the voice link's packets go out of the socket the game sends its VDP on, to the port it sends to, so they reach a
// peer wherever the game's own voice does
bool SendVoiceLink(uint32_t address, const void* data, int len) {
    VdpRoute route;
    {
        std::lock_guard<std::mutex> lock(g_route_lock);
        auto it = g_routes.find(address);
        if (it == g_routes.end()) return false;
        route = it->second;
    }
    Socket k = Find(route.sock);
    if (k.s == INVALID_SOCKET) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = route.port;
    to.sin_addr.s_addr = address;
    return SendOnce(k.s, static_cast<const uint8_t*>(data), len, &to) >= 0;
}

bool InSession() {
    uint64_t sent = g_peer_sent;
    return sent && GetTickCount64() - sent < kSessionHoldMs;
}

void SetCoopHost(bool (*host)(uint32_t& rate, uint32_t& floor)) { g_coop_host = host; }

bool CoopHostValues(uint32_t& rate, uint32_t& floor) {
    // a relaunch brings the values its predecessor had (--coop-host=rate,floor)
    static std::once_flag carried;
    std::call_once(carried, [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--coop-host=");
        unsigned r = 0, f = 0;
        if (p && std::swscanf(p + 12, L"%u,%u", &r, &f) == 2 && !g_coop_values) StoreCoop(r, f);
    });
    uint64_t v = g_coop_values;
    if (!v) return false;
    rate = uint32_t(v >> 32) & 0x7FFFFFFF;
    floor = uint32_t(v);
    return true;
}

void CloseSockets() {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [id, socket] : g_sockets) ::closesocket(socket.s);
    g_sockets.clear();
    g_pending.clear();
}

}  // namespace xnet

HLE_STDCALL(XNetStartup, 1) {
    Init();
    return 0;
}
HLE_STDCALL(WSAStartup, 2) {
    Init();
    if (a[1]) {
        std::memset(HostPtr(a[1]), 0, 400);
        W32(a[1], 0x02020202);  // wVersion, wHighVersion
    }
    return 0;
}
HLE_STDCALL(XNetGetEthernetLinkStatus, 0) { return kLinkUp; }
HLE_STDCALL(XOnlineStartup, 1) { return 0; }
HLE_STDCALL(XOnlineCleanup, 0) { return 0; }
HLE_STDCALL(WSAGetLastError, 0) { return t_error; }

HLE_STDCALL(XNetRandom, 2) {
    Random(a[0], a[1]);
    return 0;
}
HLE_STDCALL(XNetCreateKey, 2) {
    Random(a[0], 8);
    Random(a[1], 16);
    kernel::G<uint8_t>(a[0]) &= 0x0F;
    return 0;
}
HLE_STDCALL(XNetRegisterKey, 2) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::memcpy(g_kid.data(), HostPtr(a[0]), 8);
    return 0;
}
HLE_STDCALL(XNetUnregisterKey, 1) { return 0; }

HLE_STDCALL(XNetGetTitleXnAddr, 1) {
    Init();
    OwnXnAddr(static_cast<uint8_t*>(HostPtr(a[0])));
    return kXnAddrEthernet;
}
HLE_STDCALL(XNetXnAddrToInAddr, 3) {
    uint32_t ina = R32(a[0]);
    if (!ina) return WSAEINVAL;
    if (ina != g_local && Private(ina) && !g_instance) {
        uint32_t join = JoinAddress(), remote = g_remote;
        if (join && !Private(join)) ina = join;
        else if (remote) ina = remote;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        std::memcpy(g_peers[ina].data(), HostPtr(a[0]), kXnAddrSize);
    }
    W32(a[2], ina);
    return 0;
}
HLE_STDCALL(XNetInAddrToXnAddr, 3) {
    uint32_t ina = a[0];
    uint8_t x[kXnAddrSize];
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_peers.find(ina);
    if (!ina || ina == g_local) {
        OwnXnAddr(x);
    } else if (it != g_peers.end()) {
        std::memcpy(x, it->second.data(), kXnAddrSize);
    } else {
        std::memset(x, 0, kXnAddrSize);
        std::memcpy(x, &ina, 4);
    }
    if (a[1]) std::memcpy(HostPtr(a[1]), x, kXnAddrSize);
    if (a[2]) std::memcpy(HostPtr(a[2]), g_kid.data(), 8);
    return 0;
}

HLE_STDCALL(XNetDnsLookup, 3) {
    std::string name = kernel::ReadCString(a[0], 256);
    uint32_t dns = mem::AllocSystem(kXnDnsSize);
    std::memset(HostPtr(dns), 0, kXnDnsSize);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    uint32_t count = 0;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) == 0) {
        for (addrinfo* r = res; r && count < 8; r = r->ai_next)
            W32(dns + 8 + 4 * count++, reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(res);
    }
    W32(dns, count ? 0 : WSAHOST_NOT_FOUND);
    W32(dns + 4, count);
    W32(a[2], dns);
    LOGI("xnet: dns %s -> %u addresses", name.c_str(), count);
    Signal(a[1]);
    return 0;
}
HLE_STDCALL(XNetDnsRelease, 1) {
    if (a[0]) mem::FreeSystem(a[0]);
    return 0;
}

static uint32_t AddSocket(SOCKET s, bool udp, bool vdp = false) {
    u_long on = 1;
    ::ioctlsocket(s, FIONBIO, &on);
    if (udp) {
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, kUdpConnReset, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    }
    std::lock_guard<std::mutex> lock(g_lock);
    uint32_t id = g_next_socket;
    g_next_socket += 4;
    g_sockets[id] = {s, udp, false, 0, vdp};
    return id;
}

HLE_STDCALL(socket, 3) {
    Init();
    uint32_t af = a[0], type = a[1];  // the protocol: UDP, or VDP, which is UDP between two copies of this
    if (af != AF_INET) return Fail(WSAEAFNOSUPPORT);
    bool udp = type == SOCK_DGRAM;
    if (!udp && type != SOCK_STREAM) return Fail(WSAESOCKTNOSUPPORT);
    SOCKET s = ::socket(AF_INET, int(type), udp ? IPPROTO_UDP : IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Fail(HostError());
    return AddSocket(s, udp, udp && a[2] == kVdpProtocol);
}
HLE_STDCALL(closesocket, 1) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(a[0]);
    if (it == g_sockets.end()) return Fail(WSAENOTSOCK);
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i].sock == a[0])
            g_pending.erase(g_pending.begin() + ptrdiff_t(i));
        else
            i++;
    }
    ::closesocket(it->second.s);
    g_sockets.erase(it);
    return 0;
}
HLE_STDCALL(shutdown, 2) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    return ::shutdown(k.s, int(a[1])) == 0 ? 0 : Fail(HostError());
}
HLE_STDCALL(ioctlsocket, 3) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(a[0]);
    if (it == g_sockets.end()) return Fail(WSAENOTSOCK);
    if (a[1] == kGuestFionbio) {
        it->second.nonblocking = R32(a[2]) != 0;
        return 0;
    }
    u_long v = R32(a[2]);
    if (::ioctlsocket(it->second.s, long(a[1]), &v) != 0) return Fail(HostError());
    W32(a[2], uint32_t(v));
    return 0;
}
HLE_STDCALL(setsockopt, 5) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    if (::setsockopt(k.s, int(a[1]), int(a[2]), static_cast<const char*>(HostPtr(a[3])), int(a[4])) == 0) return 0;
    LOG_FIRST(10, "xnet: setsockopt %X level %X option %X ignored (%d)", a[0], a[1], a[2], WSAGetLastError());
    return 0;
}
HLE_STDCALL(getsockopt, 5) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    int len = int(R32(a[4]));
    if (::getsockopt(k.s, int(a[1]), int(a[2]), static_cast<char*>(HostPtr(a[3])), &len) != 0) return Fail(WSAGetLastError());
    W32(a[4], uint32_t(len));
    return 0;
}
HLE_STDCALL(getsockname, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in me{};
    int len = sizeof(me);
    if (::getsockname(k.s, reinterpret_cast<sockaddr*>(&me), &len) != 0) return Fail(HostError());
    std::memcpy(HostPtr(a[1]), &me, sizeof(me));
    W32(a[2], sizeof(me));
    return 0;
}
HLE_STDCALL(bind, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in at = GuestAddr(a[1]);
    if (g_instance && at.sin_addr.s_addr == INADDR_ANY) at.sin_addr.s_addr = g_local;
    if (::bind(k.s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
        int e = HostError();
        LOGW("xnet: bind %X to %s failed (%d)", a[0], Addr(at).c_str(), e);
        return Fail(e);
    }
    LOGI("xnet: bind %X to %s", a[0], Addr(at).c_str());
    sockaddr_in me{};
    int len = sizeof(me);
    ::getsockname(k.s, reinterpret_cast<sockaddr*>(&me), &len);
    std::lock_guard<std::mutex> lock(g_lock);
    if (auto it = g_sockets.find(a[0]); it != g_sockets.end()) it->second.port = me.sin_port;
    return 0;
}
HLE_STDCALL(connect, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in to = GuestAddr(a[1]);
    if (::connect(k.s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) return 0;
    int e = HostError();
    if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
    if (!Wait(k.s, true, 10000)) return Fail(WSAETIMEDOUT);
    int err = 0, len = sizeof(err);
    ::getsockopt(k.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    return err ? Fail(err) : 0;
}
HLE_STDCALL(listen, 2) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    return ::listen(k.s, int(a[1])) == 0 ? 0 : Fail(WSAGetLastError());
}
HLE_STDCALL(accept, 3) {
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        sockaddr_in from{};
        int len = sizeof(from);
        SOCKET s = ::accept(k.s, reinterpret_cast<sockaddr*>(&from), &len);
        if (s != INVALID_SOCKET) {
            if (a[1]) std::memcpy(HostPtr(a[1]), &from, sizeof(from));
            if (a[2]) W32(a[2], sizeof(from));
            LOGI("xnet: accept %X from %s", a[0], Addr(from).c_str());
            return AddSocket(s, false);
        }
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, false, 50);
    }
}
HLE_STDCALL(select, 5) {
    uint32_t guest[3] = {a[1], a[2], a[3]};
    fd_set host[3];
    std::vector<std::pair<uint32_t, SOCKET>> used[3];
    int total = 0;
    for (int i = 0; i < 3; i++) {
        FD_ZERO(&host[i]);
        if (!guest[i]) continue;
        uint32_t n = std::min(R32(guest[i]), kFdSetMax);
        for (uint32_t j = 0; j < n; j++) {
            uint32_t id = R32(guest[i] + 4 + 4 * j);
            Socket k = Find(id);
            if (k.s == INVALID_SOCKET) continue;
            FD_SET(k.s, &host[i]);
            used[i].push_back({id, k.s});
            total++;
        }
    }
    timeval tv{};
    timeval* ptv = nullptr;
    if (a[4]) {
        tv.tv_sec = long(R32(a[4]));
        tv.tv_usec = long(R32(a[4] + 4));
        ptv = &tv;
    }
    int r = 0;
    if (!total) {
        if (ptv) Sleep(DWORD(tv.tv_sec * 1000 + tv.tv_usec / 1000));
    } else {
        r = HostSelect(guest[0] ? &host[0] : nullptr, guest[1] ? &host[1] : nullptr, guest[2] ? &host[2] : nullptr, ptv);
        if (r < 0) return Fail(HostError());
    }
    for (int i = 0; i < 3; i++) {
        if (!guest[i]) continue;
        uint32_t k = 0;
        for (auto& [id, s] : used[i])
            if (FD_ISSET(s, &host[i])) W32(guest[i] + 4 + 4 * k++, id);
        W32(guest[i], k);
    }
    return uint32_t(r);
}

HLE_STDCALL(recvfrom, 6) {
    std::vector<Span> spans{{a[1], a[2]}};
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        int err = 0;
        int n;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            n = Recv(a[0], k.s, spans, a[4], a[5], err);
        }
        if (n >= 0) return uint32_t(n);
        if (err != WSAEWOULDBLOCK || k.nonblocking) return Fail(err);
        Wait(k.s, false, 50);
    }
}
// stream sockets; a reset connection is an error here, not a stale port unreachable reply
HLE_STDCALL(recv, 4) {
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        int n = ::recv(k.s, static_cast<char*>(HostPtr(a[1])), int(a[2]), int(a[3]));
        if (n >= 0) return uint32_t(n);
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, false, 50);
    }
}
HLE_STDCALL(send, 4) {
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        int n = ::send(k.s, static_cast<const char*>(HostPtr(a[1])), int(a[2]), int(a[3]));
        if (n >= 0) {
            g_peer_sent = GetTickCount64();
            return uint32_t(n);
        }
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, true, 50);
    }
}
HLE_STDCALL(sendto, 6) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in to{};
    if (a[4]) to = GuestAddr(a[4]);
    int n = Send(a[0], k.s, static_cast<const uint8_t*>(HostPtr(a[1])), int(a[2]), a[4] ? &to : nullptr);
    return n < 0 ? Fail(HostError()) : uint32_t(n);
}

namespace {

uint32_t SendOverlapped(uint32_t sock, uint32_t bufs, uint32_t count, uint32_t sent, uint32_t to_va, uint32_t ovl) {
    Socket k = Find(sock);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    auto data = Gather(bufs, count);
    sockaddr_in to{};
    if (to_va) to = GuestAddr(to_va);
    int n = Send(sock, k.s, data.data(), int(data.size()), to_va ? &to : nullptr);
    if (n < 0) return Fail(HostError());
    if (sent) W32(sent, uint32_t(n));
    if (ovl) Complete(ovl, 0, uint32_t(n));
    return 0;
}

uint32_t RecvOverlapped(uint32_t sock, uint32_t bufs, uint32_t count, uint32_t received, uint32_t flags, uint32_t from,
                        uint32_t fromlen, uint32_t ovl) {
    PendingRecv p{sock, ovl, from, fromlen, Spans(bufs, count)};
    for (;;) {
        Socket k = Find(sock);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        std::unique_lock<std::mutex> lock(g_lock);
        int err = 0;
        int n = Recv(sock, k.s, p.spans, from, fromlen, err);
        if (n >= 0) {
            if (received) W32(received, uint32_t(n));
            if (flags) W32(flags, 0);
            if (ovl) Complete(ovl, 0, uint32_t(n));
            return 0;
        }
        if (err != WSAEWOULDBLOCK) return Fail(err);
        if (ovl) {
            W32(ovl + kOvlStatus, kOvlPending);
            if (auto o = kernel::FromHandle(R32(ovl + kOvlEvent))) kernel::ResetEvent(*o);
            g_pending.push_back(std::move(p));
            return Fail(WSA_IO_PENDING);
        }
        if (k.nonblocking) return Fail(err);
        lock.unlock();
        Wait(k.s, false, 50);
    }
}

}  // namespace

HLE_STDCALL(WSARecvFrom, 9) { return RecvOverlapped(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); }
HLE_STDCALL(WSARecv, 7) { return RecvOverlapped(a[0], a[1], a[2], a[3], a[4], 0, 0, a[5]); }
HLE_STDCALL(WSASendTo, 9) { return SendOverlapped(a[0], a[1], a[2], a[3], a[5], a[7]); }
HLE_STDCALL(WSASend, 7) { return SendOverlapped(a[0], a[1], a[2], a[3], 0, a[5]); }

HLE_STDCALL(WSAGetOverlappedResult, 5) {
    uint32_t ovl = a[1];
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            auto it = std::find_if(g_pending.begin(), g_pending.end(), [&](const PendingRecv& p) { return p.ovl == ovl; });
            if (it == g_pending.end() || TryComplete(*it)) {
                if (it != g_pending.end()) g_pending.erase(it);
                uint32_t status = R32(ovl + kOvlStatus);
                if (status == kOvlPending) {
                    t_error = WSA_IO_INCOMPLETE;
                    return 0;
                }
                if (a[2]) W32(a[2], R32(ovl + kOvlBytes));
                if (a[4]) W32(a[4], 0);
                if (status) {
                    t_error = status;
                    return 0;
                }
                return 1;
            }
        }
        if (!a[3]) {
            t_error = WSA_IO_INCOMPLETE;
            return 0;
        }
        Sleep(1);
    }
}
HLE_STDCALL(WSACancelOverlappedIO, 1) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i].sock == a[0]) {
            Complete(g_pending[i].ovl, WSA_OPERATION_ABORTED, 0);
            g_pending.erase(g_pending.begin() + ptrdiff_t(i));
        } else {
            i++;
        }
    }
    return 0;
}
