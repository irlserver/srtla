#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <spdlog/spdlog.h>

#include <iostream>
#include <stdexcept>
#include <string>

#include "connection/connection_registry.h"
#include "protocol/srt_handler.h"
#include "protocol/srtla_handler.h"
#include "quality/load_balancer.h"
#include "quality/metrics_collector.h"
#include "quality/quality_evaluator.h"
#include "receiver_config.h"
#include "utils/network_utils.h"

extern "C" {
#include "common.h"
}

namespace {

constexpr int MAX_EPOLL_EVENTS = 10;

void set_socket_buffers(int socket_fd) {
  int bufsize = RECV_BUF_SIZE;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) !=
      0) {
    spdlog::error("failed to set receive buffer size ({})", bufsize);
    throw std::runtime_error("Failed to set receive buffer size");
  }

  bufsize = SEND_BUF_SIZE;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) !=
      0) {
    spdlog::error("failed to set send buffer size ({})", bufsize);
    throw std::runtime_error("Failed to set send buffer size");
  }
}

} // namespace

int main(int argc, char **argv) {
  argparse::ArgumentParser args("srtla_rec", VERSION);
  args.add_argument("--srtla_port")
      .help("Port to bind the SRTLA socket to")
      .default_value(static_cast<uint16_t>(5000))
      .scan<'d', uint16_t>();
  args.add_argument("--srt_hostname")
      .help("Hostname of the downstream SRT server")
      .default_value(std::string{"127.0.0.1"});
  args.add_argument("--srt_port")
      .help("Port of the downstream SRT server")
      .default_value(static_cast<uint16_t>(4001))
      .scan<'d', uint16_t>();
  args.add_argument("--log_level")
      .help("Set logging level (trace, debug, info, warn, error, critical)")
      .default_value(std::string{"info"});

  try {
    args.parse_args(argc, argv);
  } catch (const std::runtime_error &err) {
    std::cerr << err.what() << std::endl;
    std::cerr << args;
    std::exit(1);
  }

  const uint16_t srtla_port = args.get<uint16_t>("--srtla_port");
  const std::string srt_hostname = args.get<std::string>("--srt_hostname");
  const std::string srt_port = std::to_string(args.get<uint16_t>("--srt_port"));
  const std::string log_level = args.get<std::string>("--log_level");

  if (log_level == "trace") {
    spdlog::set_level(spdlog::level::trace);
  } else if (log_level == "debug") {
    spdlog::set_level(spdlog::level::debug);
  } else if (log_level == "info") {
    spdlog::set_level(spdlog::level::info);
  } else if (log_level == "warn") {
    spdlog::set_level(spdlog::level::warn);
  } else if (log_level == "error") {
    spdlog::set_level(spdlog::level::err);
  } else if (log_level == "critical") {
    spdlog::set_level(spdlog::level::critical);
  } else {
    spdlog::warn("Invalid log level '{}' specified, using 'info' as default",
                 log_level);
    spdlog::set_level(spdlog::level::info);
  }

  struct sockaddr_storage srt_addr {};
  int resolve_result = srtla::utils::NetworkUtils::resolve_srt_address(
      srt_hostname.c_str(), srt_port.c_str(), &srt_addr, RECV_BUF_SIZE,
      SEND_BUF_SIZE);
  if (resolve_result < 0) {
    return EXIT_FAILURE;
  }

  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    spdlog::critical("epoll creation failed");
    return EXIT_FAILURE;
  }

  int srtla_sock = socket(AF_INET6, SOCK_DGRAM, 0);
  if (srtla_sock < 0) {
    spdlog::critical("SRTLA socket creation failed");
    return EXIT_FAILURE;
  }

  int v6only = 0;
  if (setsockopt(srtla_sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
                 sizeof(v6only)) < 0) {
    spdlog::critical("Failed to set IPV6_V6ONLY option");
    return EXIT_FAILURE;
  }

  try {
    set_socket_buffers(srtla_sock);
  } catch (const std::exception &) {
    return EXIT_FAILURE;
  }

  int flags = fcntl(srtla_sock, F_GETFL, 0);
  if (flags == -1 || fcntl(srtla_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
    spdlog::error("failed to set srtla_sock non-blocking");
    return EXIT_FAILURE;
  }

  struct sockaddr_in6 listen_addr {};
  listen_addr.sin6_family = AF_INET6;
  listen_addr.sin6_addr = in6addr_any;
  listen_addr.sin6_port = htons(srtla_port);
  if (bind(srtla_sock, reinterpret_cast<const struct sockaddr *>(&listen_addr),
           sizeof(listen_addr)) < 0) {
    spdlog::critical("SRTLA socket bind failed");
    return EXIT_FAILURE;
  }

  if (srtla::utils::NetworkUtils::epoll_add(epoll_fd, srtla_sock, EPOLLIN,
                                            nullptr) != 0) {
    spdlog::critical("Failed to add the SRTLA sock to the epoll");
    return EXIT_FAILURE;
  }

  srtla::connection::ConnectionRegistry registry;
  srtla::quality::MetricsCollector metrics_collector;
  srtla::protocol::SRTHandler srt_handler(srtla_sock, srt_addr, epoll_fd,
                                          registry);
  srtla::protocol::SRTLAHandler srtla_handler(srtla_sock, registry, srt_handler,
                                              metrics_collector);
  srtla::quality::QualityEvaluator quality_evaluator;
  srtla::quality::LoadBalancer load_balancer;

  spdlog::info("srtla_rec is now running");

  const auto keepalive_callback =
      [&srtla_handler](const srtla::connection::ConnectionPtr &conn,
                       time_t ts) { srtla_handler.send_keepalive(conn, ts); };

  while (true) {
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int eventcnt = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 1000);

    time_t ts = 0;
    if (get_seconds(&ts) != 0) {
      spdlog::error("Failed to get the current time");
      continue;
    }

    std::size_t group_cnt;
    for (int i = 0; i < eventcnt; i++) {
      // Snapshot the current group count before processing. Both
      // srtla_handler.process_packet() and srt_handler.handle_srt_data() may
      // remove ConnectionGroup instances via registry operations (e.g.,
      // registry.find_group_by_id() returning nullptr after removal). If the
      // group count shrinks, events[i].data.ptr pointers from subsequent
      // iterations may reference freed memory. We detect this by comparing
      // registry.groups().size() with group_cnt and break early to avoid
      // iterator/pointer invalidation.
      group_cnt = registry.groups().size();
      if (events[i].data.ptr == nullptr) {
        srtla_handler.process_packets(ts);
      } else {
        auto raw_group = static_cast<srtla::connection::ConnectionGroup *>(
            events[i].data.ptr);
        auto shared_group = registry.find_group_by_id(raw_group->id().data());
        if (shared_group) {
          srt_handler.handle_srt_data(shared_group);
        }
      }

      if (registry.groups().size() < group_cnt) {
        break;
      }
    }

    registry.cleanup_inactive(ts, keepalive_callback);
    for (auto &group : registry.groups()) {
      quality_evaluator.evaluate_group(group, ts);
      load_balancer.adjust_weights(group, ts);
    }
  }

  return 0;
}
