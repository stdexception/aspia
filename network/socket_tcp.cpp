/*
* PROJECT:         Aspia Remote Desktop
* FILE:            network/socket_tcp.cpp
* LICENSE:         See top-level directory
* PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
*/

#include "network/socket_tcp.h"

#include "base/exception.h"
#include "base/logging.h"

namespace aspia {

static volatile LONG _socket_ref_count = 0;

static const int kWriteTimeout = 10000;
static const size_t kMaxHostNameLength = 64;

SocketTCP::SocketTCP() :
    ref_(true)
{
    // ���� �� ������ ������ ��� �� ���� �������.
    if (InterlockedIncrement(&_socket_ref_count) == 1)
    {
        WSADATA data = { 0 };

        static const BYTE kMajorVersion = 2;
        static const BYTE kMinorVersion = 2;

        // �������������� ���������� �������.
        if (WSAStartup(MAKEWORD(kMajorVersion, kMinorVersion), &data) != 0)
        {
            LOG(ERROR) << "WSAStartup() failed: " << WSAGetLastError();
            throw Exception("Unable to initialize socket library.");
        }

        if (HIBYTE(data.wVersion) != kMinorVersion || LOBYTE(data.wVersion) != kMajorVersion)
        {
            LOG(ERROR) << "Wrong version sockets library: " << data.wVersion;
            throw Exception("Unable to initialize socket library.");
        }
    }

    // ������� �����.
    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET)
    {
        LOG(ERROR) << "socket() failed: " << WSAGetLastError();
        throw Exception("Unable to create network socket.");
    }
}

// �����������, ������� ���������� ��� ������ ��������� �����������.
SocketTCP::SocketTCP(SOCKET sock) :
    sock_(sock),
    ref_(false)
{
    EnableNagles(false);
    SetWriteTimeout(kWriteTimeout);
}

SocketTCP::~SocketTCP()
{
    Shutdown();

    if (ref_)
    {
        // ��������� ���������� ����������� ������ � ���� �� ������ �� ��������.
        if (InterlockedDecrement(&_socket_ref_count) == 0)
        {
            // ���������������� ���������� �������.
            if (WSACleanup() == SOCKET_ERROR)
            {
                DLOG(ERROR) << "WSACleanup() failed: " << WSAGetLastError();
            }
        }
    }
}

void SocketTCP::Connect(const std::string &address, int port)
{
    struct sockaddr_in dest_addr = { 0 };

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    dest_addr.sin_addr.s_addr = inet_addr(address.c_str());
    if (dest_addr.sin_addr.s_addr == INADDR_NONE)
    {
        HOSTENT *host = gethostbyname(address.c_str());

        if (!host)
        {
            LOG(WARNING) << "gethostbyname() failed: " << WSAGetLastError();
            throw Exception("Unable to handle the host name.");
        }

        ((uint32_t *)&dest_addr.sin_addr)[0] = ((uint32_t **)host->h_addr_list)[0][0];
    }

    u_long non_blocking = 1;

    if (ioctlsocket(sock_, FIONBIO, &non_blocking) == SOCKET_ERROR)
    {
        LOG(ERROR) << "ioctlsocket() failed: " << WSAGetLastError();
        throw Exception("Unable to set non-blocking mode for socket.");
    }

    // �������� ������������.
    int ret = connect(sock_,
                      reinterpret_cast<const struct sockaddr*>(&dest_addr),
                      sizeof(dest_addr));
    if (ret == SOCKET_ERROR)
    {
        int err = WSAGetLastError();

        // ���� ������������� ����� �� ����� ��������� ����������� �� ����� ������.
        if (err == WSAEWOULDBLOCK)
        {
            fd_set write_fds;

            FD_ZERO(&write_fds);
            FD_SET(sock_, &write_fds);

            struct timeval timeout = { 0 };

            timeout.tv_sec = 5; // 5 seconds.
            timeout.tv_usec = 0;

            // ���� ���������� ��������.
            if (select(0, NULL, &write_fds, NULL, &timeout) == SOCKET_ERROR ||
                !FD_ISSET(sock_, &write_fds))
            {
                LOG(WARNING) << "select() failed: " << WSAGetLastError();
                throw Exception("Unable to establish connection (timeout).");
            }
        }
        else
        {
            LOG(WARNING) << "connect() failed: " << err;
            throw Exception("Unable to establish connection.");
        }
    }

    non_blocking = 0;

    if (ioctlsocket(sock_, FIONBIO, &non_blocking) == SOCKET_ERROR)
    {
        LOG(ERROR) << "ioctlsocket() failed: " << WSAGetLastError();
        throw Exception("Unable to set blocking mode for socket.");
    }

    EnableNagles(false);
    SetWriteTimeout(kWriteTimeout);
}

void SocketTCP::Bind(int port)
{
    struct sockaddr_in local_addr = { 0 };

    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock_,
             reinterpret_cast<const struct sockaddr*>(&local_addr),
             sizeof(local_addr)) == SOCKET_ERROR)
    {
        LOG(ERROR) << "bind() failed: " << WSAGetLastError();
        throw Exception("Unable to execute binding command.");
    }
}

void SocketTCP::Listen()
{
    if (listen(sock_, SOMAXCONN) == SOCKET_ERROR)
    {
        LOG(ERROR) << "listen() failed: " << WSAGetLastError();
        throw Exception("Unable to execute listen command.");
    }
}

std::unique_ptr<Socket> SocketTCP::Accept()
{
    struct sockaddr_in addr;
    int len = sizeof(addr);

    SOCKET client = accept(sock_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    if (client == INVALID_SOCKET)
    {
        LOG(ERROR) << "accept() failed: " << WSAGetLastError();
        throw Exception("Unable to accept client connection or server stopped.");
    }

    return std::unique_ptr<Socket>(new SocketTCP(client));
}

void SocketTCP::SetWriteTimeout(int timeout)
{
    DWORD value = timeout;

    if (setsockopt(sock_,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&value),
                   sizeof(value)) == SOCKET_ERROR)
    {
        LOG(ERROR) << "setsockopt() failed: " << WSAGetLastError();
        throw Exception("Unable to set write timeout for socket.");
    }
}

void SocketTCP::EnableNagles(bool enable)
{
    DWORD value = enable ? 0 : 1;

    if (setsockopt(sock_,
                   IPPROTO_TCP,
                   TCP_NODELAY,
                   reinterpret_cast<const char*>(&value),
                   sizeof(value)) == SOCKET_ERROR)
    {
        LOG(ERROR) << "setsockopt() failed: " << WSAGetLastError();
        throw Exception("Unable to enable or disable Nagles algorithm for socket.");
    }
}

void SocketTCP::Shutdown()
{
    LockGuard<Lock> guard(&shutdown_lock_);

    if (sock_ != INVALID_SOCKET)
    {
        shutdown(sock_, SD_BOTH);
        closesocket(sock_);

        sock_ = INVALID_SOCKET;
    }
}

void SocketTCP::Reader(char *buf, int len)
{
    while (len)
    {
        int read = recv(sock_, buf, len, 0);

        if (read <= 0)
        {
            LOG(WARNING) << "recv() failed: " << WSAGetLastError();
            throw Exception("Unable to read data from network.");
        }

        buf += read;
        len -= read;
    }
}

void SocketTCP::Writer(const char *buf, int len)
{
    while (len)
    {
        int written = send(sock_, buf, len, 0);

        if (written <= 0)
        {
            LOG(WARNING) << "send() failed: " << WSAGetLastError();
            throw Exception("Unable to write data to network.");
        }

        buf += written;
        len -= written;
    }
}

void SocketTCP::WriteMessage(const uint8_t *buf, uint32_t len)
{
    // ���������� ������ ������ � ���� ������.
    Writer(reinterpret_cast<const char*>(&len), sizeof(len));
    Writer(reinterpret_cast<const char*>(buf), len);
}

uint32_t SocketTCP::ReadMessageSize()
{
    uint32_t size = 0;

    Reader(reinterpret_cast<char*>(&size), sizeof(size));

    return size;
}

void SocketTCP::ReadMessage(uint8_t *buf, uint32_t len)
{
    Reader(reinterpret_cast<char*>(buf), len);
}

} // namespace aspia
