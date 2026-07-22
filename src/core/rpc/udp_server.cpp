// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>
#include <boost/asio.hpp>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/rpc/packet.h"
#include "core/rpc/udp_server.h"

namespace Core::RPC {

u16 GetRPCPort() {
    const char* value = std::getenv("AZAHAR_RPC_PORT");
    if (!value) {
        return Settings::values.rpc_server_port.GetValue();
    }
    char* end{};
    const auto port = std::strtoul(value, &end, 10);
    return end != value && *end == '\0' && port > 0 && port <= UINT16_MAX
               ? static_cast<u16>(port)
               : Settings::values.rpc_server_port.GetValue();
}

class UDPServer::Impl {
public:
    Impl(std::function<void(std::unique_ptr<Packet>)> new_request_callback,
         ClientCountHandler client_count_handler_)
        // Use a random high port
        // TODO: Make configurable or increment port number on failure
        : socket(io_context, boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(),
                                                            GetRPCPort())),
          client_timer(io_context), new_request_callback(std::move(new_request_callback)),
          client_count_handler(std::move(client_count_handler_)) {

        StartReceive();
        StartClientTimer();
        worker_thread = std::thread([this] { io_context.run(); });
    }

    ~Impl() {
        io_context.stop();
        worker_thread.join();
    }

private:
    void StartReceive() {
        socket.async_receive_from(boost::asio::buffer(request_buffer), remote_endpoint,
                                  [this](const boost::system::error_code& error, std::size_t size) {
                                      HandleReceive(error, size);
                                  });
    }

    void HandleReceive(const boost::system::error_code& error, std::size_t size) {
        if (error) {
            LOG_WARNING(RPC_Server, "Failed to receive data on UDP socket: {}", error.message());
        } else if (size >= MIN_PACKET_SIZE && size <= MAX_PACKET_SIZE) {
            PacketHeader header;
            std::memcpy(&header, request_buffer.data(), sizeof(header));
            if ((size - MIN_PACKET_SIZE) == header.packet_size) {
                u8* data = request_buffer.data() + MIN_PACKET_SIZE;
                std::function<void(Packet&)> send_reply_callback =
                    std::bind(&Impl::SendReply, this, remote_endpoint, std::placeholders::_1);
                std::unique_ptr<Packet> new_packet =
                    std::make_unique<Packet>(header, data, send_reply_callback);

                TouchClient(remote_endpoint);
                // Send the request to the upper layer for handling
                new_request_callback(std::move(new_packet));
            }
        } else {
            LOG_WARNING(RPC_Server, "Received message with wrong size: {}", size);
        }
        StartReceive();
    }

    void TouchClient(const boost::asio::ip::udp::endpoint& endpoint) {
        const auto now = std::chrono::steady_clock::now();
        PruneClients(now);
        const auto client = std::ranges::find_if(
            clients, [&endpoint](const auto& entry) { return entry.endpoint == endpoint; });
        if (client == clients.end()) {
            clients.push_back({endpoint, now});
            NotifyClientCount();
        } else {
            client->last_seen = now;
        }
    }

    void StartClientTimer() {
        client_timer.expires_after(std::chrono::seconds(1));
        client_timer.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                PruneClients(std::chrono::steady_clock::now());
                StartClientTimer();
            }
        });
    }

    void PruneClients(std::chrono::steady_clock::time_point now) {
        const auto old_size = clients.size();
        std::erase_if(clients, [now](const auto& client) {
            return now - client.last_seen >= std::chrono::seconds(10);
        });
        if (clients.size() != old_size) {
            NotifyClientCount();
        }
    }

    void NotifyClientCount() {
        if (client_count_handler) {
            client_count_handler(static_cast<u32>(clients.size()));
        }
    }

    void SendReply(boost::asio::ip::udp::endpoint endpoint, Packet& reply_packet) {
        std::vector<u8> reply_buffer(MIN_PACKET_SIZE + reply_packet.GetPacketDataSize());
        auto reply_header = reply_packet.GetHeader();

        std::memcpy(reply_buffer.data(), &reply_header, sizeof(reply_header));
        std::memcpy(reply_buffer.data() + (4 * sizeof(u32)), reply_packet.GetPacketData().data(),
                    reply_packet.GetPacketDataSize());

        boost::system::error_code error;
        socket.send_to(boost::asio::buffer(reply_buffer), endpoint, 0, error);

        if (error) {
            LOG_WARNING(RPC_Server, "Failed to send reply: {}", error.message());
        } else {
            LOG_INFO(RPC_Server, "Sent reply version({}) id=({}) type=({}) size=({})",
                     reply_packet.GetVersion(), reply_packet.GetId(), reply_packet.GetPacketType(),
                     reply_packet.GetPacketDataSize());
        }
    }

    std::thread worker_thread;

    boost::asio::io_context io_context;
    boost::asio::ip::udp::socket socket;
    boost::asio::steady_timer client_timer;
    std::array<u8, MAX_PACKET_SIZE> request_buffer;
    boost::asio::ip::udp::endpoint remote_endpoint;

    std::function<void(std::unique_ptr<Packet>)> new_request_callback;
    ClientCountHandler client_count_handler;
    struct Client {
        boost::asio::ip::udp::endpoint endpoint;
        std::chrono::steady_clock::time_point last_seen;
    };
    std::vector<Client> clients;
};

UDPServer::UDPServer(std::function<void(std::unique_ptr<Packet>)> new_request_callback,
                     ClientCountHandler client_count_handler)
    : impl(std::make_unique<Impl>(std::move(new_request_callback),
                                  std::move(client_count_handler))) {}

UDPServer::~UDPServer() = default;

} // namespace Core::RPC
