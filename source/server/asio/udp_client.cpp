/*!
    \file udp_client.cpp
    \brief UDP client implementation
    \author Ivan Shynkarenka
    \date 23.12.2016
    \copyright MIT License
*/

#include "server/asio/udp_client.h"

namespace CppServer {
namespace Asio {

UDPClient::UDPClient(std::shared_ptr<Service> service, const std::string& address, int port)
    : _id(CppCommon::UUID::Generate()),
      _service(service),
      _strand(*_service->service()),
      _endpoint(asio::ip::udp::endpoint(asio::ip::address::from_string(address), (unsigned short)port)),
      _socket(*_service->service()),
      _connected(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0),
      _reciving(false),
      _recive_buffer(CHUNK + 1),
      _option_reuse_address(false),
      _option_reuse_port(false),
      _option_multicast(false)
{
    assert((service != nullptr) && "Asio service is invalid!");
    if (service == nullptr)
        throw CppCommon::ArgumentException("Asio service is invalid!");
}

UDPClient::UDPClient(std::shared_ptr<Service> service, const asio::ip::udp::endpoint& endpoint)
    : _id(CppCommon::UUID::Generate()),
      _service(service),
      _strand(*_service->service()),
      _endpoint(endpoint),
      _socket(*_service->service()),
      _connected(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0),
      _reciving(false),
      _recive_buffer(CHUNK + 1),
      _option_reuse_address(false),
      _option_reuse_port(false),
      _option_multicast(false)
{
    assert((service != nullptr) && "Asio service is invalid!");
    if (service == nullptr)
        throw CppCommon::ArgumentException("Asio service is invalid!");
}

bool UDPClient::Connect()
{
    if (IsConnected())
        return false;

    // Post the connect handler
    auto self(this->shared_from_this());
    auto connect_handler = [this, self]()
    {
        if (IsConnected())
            return;

        // Open a client socket
        _socket.open(_endpoint.protocol());
        if (option_reuse_address())
            _socket.set_option(asio::ip::udp::socket::reuse_address(true));
#if (defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)) && !defined(__CYGWIN__)
        if (option_reuse_port())
        {
            typedef asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT> reuse_port;
            _socket.set_option(reuse_port(true));
        }
#endif
        if (option_multicast())
            _socket.bind(_endpoint);
        else
            _socket.bind(asio::ip::udp::endpoint(_endpoint.protocol(), 0));

        // Reset statistic
        _datagrams_sent = 0;
        _datagrams_received = 0;
        _bytes_sent = 0;
        _bytes_received = 0;

        // Update the connected flag
        _connected = true;

        // Call the client connected handler
        onConnected();

        // Try to receive something from the server
        TryReceive();
    };
    if (_service->IsMultithread())
        _strand.post(connect_handler);
    else
        _service->service()->post(connect_handler);

    return true;
}

bool UDPClient::Disconnect(bool dispatch)
{
    if (!IsConnected())
        return false;

    // Dispatch or post the disconnect handler
    auto self(this->shared_from_this());
    auto disconnect_handler = [this, self]()
    {
        if (!IsConnected())
            return;

        // Close the client socket
        _socket.close();

        // Update the connected flag
        _connected = false;

        // Call the client disconnected handler
        onDisconnected();
    };
    if (_service->IsMultithread())
    {
        if (dispatch)
            _strand.dispatch(disconnect_handler);
        else
            _strand.post(disconnect_handler);
    }
    else
    {
        if (dispatch)
            _service->service()->dispatch(disconnect_handler);
        else
            _service->service()->post(disconnect_handler);
    }

    return true;
}

bool UDPClient::Reconnect()
{
    if (!Disconnect())
        return false;

    while (IsConnected())
        CppCommon::Thread::Yield();

    return Connect();
}

void UDPClient::JoinMulticastGroup(const std::string& address)
{
    if (!IsConnected())
        return;

    // Dispatch the join multicast group handler
    auto self(this->shared_from_this());
    auto join_multicast_group_handler = [this, self, address]()
    {
        if (!IsConnected())
            return;

        asio::ip::address muticast_address = asio::ip::address::from_string(address);

        asio::ip::multicast::join_group join(muticast_address);
        _socket.set_option(join);

        // Call the client joined multicast group notification
        onJoinedMulticastGroup(address);
    };
    if (_service->IsMultithread())
        _strand.dispatch(join_multicast_group_handler);
    else
        _service->service()->dispatch(join_multicast_group_handler);
}

void UDPClient::LeaveMulticastGroup(const std::string& address)
{
    if (!IsConnected())
        return;

    // Dispatch the leave multicast group handler
    auto self(this->shared_from_this());
    auto leave_multicast_group_handler = [this, self, address]()
    {
        if (!IsConnected())
            return;

        asio::ip::address muticast_address = asio::ip::address::from_string(address);

        asio::ip::multicast::leave_group leave(muticast_address);
        _socket.set_option(leave);

        // Call the client left multicast group notification
        onLeftMulticastGroup(address);
    };
    if (_service->IsMultithread())
        _strand.dispatch(leave_multicast_group_handler);
    else
        _service->service()->dispatch(leave_multicast_group_handler);
}

bool UDPClient::Send(const void* buffer, size_t size)
{
    // Send the datagram to the server endpoint
    return Send(_endpoint, buffer, size);
}

bool UDPClient::Send(const asio::ip::udp::endpoint& endpoint, const void* buffer, size_t size)
{
    assert((buffer != nullptr) && "Pointer to the buffer should not be equal to 'nullptr'!");
    assert((size > 0) && "Buffer size should be greater than zero!");
    if ((buffer == nullptr) || (size == 0))
        return false;

    if (!IsConnected())
        return false;

    asio::error_code ec;

    // Sent datagram to the server
    size_t sent = _socket.send_to(asio::const_buffer(buffer, size), endpoint, 0, ec);
    if (sent > 0)
    {
        // Update statistic
        ++_datagrams_sent;
        _bytes_sent += sent;

        // Call the datagram sent handler
        onSent(endpoint, sent);
    }

    // Check for error
    if (ec)
    {
        SendError(ec);
        Disconnect(true);
        return false;
    }

    return true;
}

void UDPClient::TryReceive()
{
    if (_reciving)
        return;

    if (!IsConnected())
        return;

    // Async receive with the receive handler
    _reciving = true;
    auto self(this->shared_from_this());
    auto async_receive_handler = make_alloc_handler(_recive_storage, [this, self](std::error_code ec, std::size_t size)
    {
        _reciving = false;

        if (!IsConnected())
            return;

        // Received some data from the client
        if (size > 0)
        {
            // Update statistic
            ++_datagrams_received;
            _bytes_received += size;

            // If the receive buffer is full increase its size
            if (_recive_buffer.size() == size)
                _recive_buffer.resize(2 * size);

            // Call the datagram received handler
            onReceived(_recive_endpoint, _recive_buffer.data(), size);
        }

        // Try to receive again if the session is valid
        if (!ec)
            TryReceive();
        else
        {
            SendError(ec);
            Disconnect(true);
        }
    });
    if (_service->IsMultithread())
        _socket.async_receive_from(asio::buffer(_recive_buffer.data(), _recive_buffer.size()), _recive_endpoint, bind_executor(_strand, async_receive_handler));
    else
        _socket.async_receive_from(asio::buffer(_recive_buffer.data(), _recive_buffer.size()), _recive_endpoint, async_receive_handler);
}

void UDPClient::SendError(std::error_code ec)
{
    // Skip Asio disconnect errors
    if ((ec == asio::error::connection_aborted) ||
        (ec == asio::error::connection_refused) ||
        (ec == asio::error::connection_reset) ||
        (ec == asio::error::eof) ||
        (ec == asio::error::operation_aborted))
        return;

    onError(ec.value(), ec.category().name(), ec.message());
}

} // namespace Asio
} // namespace CppServer
