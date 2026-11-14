#include "java/JavaNetwork.h"

#ifdef NETCAST_PLATFORM

#include <atomic>
#include <cerrno>
#include <cstring>
#include <istream>
#include <ostream>
#include <streambuf>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "platform/Log.h"

namespace JavaNetwork
{
namespace
{

class PosixSocket final : public Socket
{
public:
	~PosixSocket() override { releaseSocket(); }

	bool connect(const std::string &host, int port) override
	{
		releaseSocket();
		closing.store(false);
		readInterrupted.store(false);
		receivedBytes.store(0);
		sentBytes.store(0);
		remoteAddress = host + ":" + std::to_string(port);
		if (port < 1 || port > 65535)
			return false;

		addrinfo hints;
		std::memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo *res = nullptr;
		const std::string portText = std::to_string(port);
		if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &res) != 0 || res == nullptr)
		{
			MC_LOG_INFO("netcast", "net: cannot resolve %s\n", host.c_str());
			return false;
		}

		const int s = ::socket(res->ai_family, res->ai_socktype, 0);
		if (s < 0)
		{
			freeaddrinfo(res);
			return false;
		}
		fd.store(s);
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		const int flags = fcntl(s, F_GETFL, 0);
		fcntl(s, F_SETFL, flags | O_NONBLOCK);
		int ret = ::connect(s, res->ai_addr, res->ai_addrlen);
		freeaddrinfo(res);
		if (ret < 0)
		{
			if (errno != EINPROGRESS)
			{
				close();
				return false;
			}
			fd_set ws;
			FD_ZERO(&ws);
			FD_SET(s, &ws);
			timeval tv;
			tv.tv_sec = 6;
			tv.tv_usec = 0;
			if (select(s + 1, nullptr, &ws, nullptr, &tv) <= 0)
			{
				MC_LOG_INFO("netcast", "net: connect timeout %s\n", remoteAddress.c_str());
				close();
				return false;
			}
			int soError = 0;
			socklen_t len = sizeof(soError);
			if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soError, &len) < 0 || soError != 0)
			{
				MC_LOG_INFO("netcast", "net: connect failed %s (%d)\n", remoteAddress.c_str(), soError);
				close();
				return false;
			}
		}
		fcntl(s, F_SETFL, flags);
		MC_LOG_INFO("netcast", "net: connected %s\n", remoteAddress.c_str());
		return true;
	}

	int read(char *buffer, int length) override
	{
		const int s = fd.load();
		if (s < 0 || buffer == nullptr || length <= 0 || closing.load())
			return -1;
		while (!closing.load() && !readInterrupted.load())
		{
			fd_set rs;
			FD_ZERO(&rs);
			FD_SET(s, &rs);
			timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 100000;
			const int ready = select(s + 1, &rs, nullptr, nullptr, &tv);
			if (ready < 0)
			{
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (ready == 0)
				continue;
			const ssize_t n = ::recv(s, buffer, length, 0);
			if (n > 0)
			{
				receivedBytes.fetch_add((std::size_t)n, std::memory_order_relaxed);
				return (int)n;
			}
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			return -1;
		}
		return -1;
	}

	bool write(const char *buffer, int length) override
	{
		const int s = fd.load();
		if (s < 0 || buffer == nullptr || closing.load())
			return false;
		int offset = 0;
		while (offset < length)
		{
			if (closing.load())
				return false;
			const ssize_t n = ::send(s, buffer + offset, length - offset, MSG_NOSIGNAL);
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			if (n <= 0)
				return false;
			sentBytes.fetch_add((std::size_t)n, std::memory_order_relaxed);
			offset += (int)n;
		}
		return true;
	}

	bool flush() override { return fd.load() >= 0 && !closing.load(); }
	void interruptRead() override { readInterrupted.store(true); }

	void close() override { releaseSocket(); }

	std::string getRemoteSocketAddress() const override { return remoteAddress; }
	std::size_t getReceivedByteCount() const override { return receivedBytes.load(std::memory_order_relaxed); }
	std::size_t getSentByteCount() const override { return sentBytes.load(std::memory_order_relaxed); }

private:
	void releaseSocket()
	{
		closing.store(true);
		readInterrupted.store(true);
		const int s = fd.exchange(-1);
		if (s >= 0)
		{
			::shutdown(s, SHUT_RDWR);
			::close(s);
		}
	}

	std::atomic<int> fd{-1};
	std::atomic_bool closing{true};
	std::atomic_bool readInterrupted{false};
	std::atomic<std::size_t> receivedBytes{0};
	std::atomic<std::size_t> sentBytes{0};
	std::string remoteAddress;
};

class SocketInputBuffer final : public std::streambuf
{
public:
	explicit SocketInputBuffer(Socket &value) : socket(value) { setg(buffer, buffer, buffer); }

protected:
	int_type underflow() override
	{
		if (gptr() < egptr())
			return traits_type::to_int_type(*gptr());
		int count = socket.read(buffer, sizeof(buffer));
		if (count <= 0)
			return traits_type::eof();
		setg(buffer, buffer, buffer + count);
		return traits_type::to_int_type(*gptr());
	}

private:
	Socket &socket;
	char buffer[512];
};

class SocketOutputBuffer final : public std::streambuf
{
public:
	explicit SocketOutputBuffer(Socket &value) : socket(value)
	{
		setp(buffer, buffer + sizeof(buffer));
	}
	~SocketOutputBuffer() override { sync(); }

protected:
	std::streamsize xsputn(const char *data, std::streamsize length) override
	{
		std::streamsize written = 0;
		while (written < length)
		{
			std::streamsize space = epptr() - pptr();
			if (space == 0)
			{
				if (!flushBuffer())
					return written;
				space = epptr() - pptr();
			}

			const std::streamsize remaining = length - written;
			const std::streamsize count = remaining < space ? remaining : space;
			std::memcpy(pptr(), data + written, static_cast<std::size_t>(count));
			pbump(static_cast<int>(count));
			written += count;
		}
		return written;
	}

	int_type overflow(int_type value) override
	{
		if (traits_type::eq_int_type(value, traits_type::eof()))
			return traits_type::not_eof(value);
		if (!flushBuffer())
			return traits_type::eof();
		*pptr() = traits_type::to_char_type(value);
		pbump(1);
		return value;
	}

	int sync() override
	{
		return flushBuffer() && socket.flush() ? 0 : -1;
	}

private:
	bool flushBuffer()
	{
		const std::streamsize count = pptr() - pbase();
		if (count > 0 && !socket.write(pbase(), static_cast<int>(count)))
			return false;
		pbump(-static_cast<int>(count));
		return true;
	}

	Socket &socket;
	char buffer[5120];
};

class SocketInputStream final : public std::istream
{
public:
	explicit SocketInputStream(Socket &socket) : std::istream(nullptr), buffer(socket)
	{
		rdbuf(&buffer);
	}
private:
	SocketInputBuffer buffer;
};

class SocketOutputStream final : public std::ostream
{
public:
	explicit SocketOutputStream(Socket &socket) : std::ostream(nullptr), buffer(socket)
	{
		rdbuf(&buffer);
	}
private:
	SocketOutputBuffer buffer;
};

}

std::unique_ptr<Socket> createSocket() { return std::make_unique<PosixSocket>(); }
std::unique_ptr<std::istream> createInputStream(Socket &socket) { return std::make_unique<SocketInputStream>(socket); }
std::unique_ptr<std::ostream> createOutputStream(Socket &socket) { return std::make_unique<SocketOutputStream>(socket); }

bool readUrl(const std::string &, std::vector<unsigned char> &) { return false; }
int getResponseCode(const std::string &) { return -1; }
bool postUrl(const std::string &, const std::string &, const std::string &,
             std::vector<unsigned char> &) { return false; }

}

#endif
