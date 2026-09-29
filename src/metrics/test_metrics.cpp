// Self-check for the Prometheus exposition. Run: ./build/test_metrics
#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "prometheus.h"

using namespace srtla;

namespace {

bool has(const std::string &body, const std::string &line) {
    return body.find(line) != std::string::npos;
}

struct sockaddr_storage make_addr(const char *ip, uint16_t port) {
    struct sockaddr_storage ss {};
    auto *in = reinterpret_cast<struct sockaddr_in *>(&ss);
    in->sin_family = AF_INET;
    in->sin_port = htons(port);
    inet_pton(AF_INET, ip, &in->sin_addr);
    return ss;
}

} // namespace

int main() {
    connection::ConnectionRegistry registry;
    utils::AuthRateLimiter rate_limiter;
    metrics::Exporter exporter(registry, rate_limiter);
    exporter.start("127.0.0.1", 0, -1, /*detailed=*/false); // port 0: no listener, tier 1 only

    // Counters exist at zero before anything happens, so rate() has a baseline.
    std::string body = exporter.render(1000);
    assert(has(body, "\nsrtla_packets_received_total 0\n"));
    assert(has(body, "\nsrtla_forward_dropped_total 0\n"));
    assert(has(body, "\nsrtla_forward_errors_total 0\n"));
    assert(has(body, "\nsrtla_groups 0\n"));
    assert(has(body, "\nsrtla_connections 0\n"));
    assert(has(body, "# TYPE srtla_groups_removed_total counter\n"));
    assert(has(body, "srtla_groups_removed_total{reason=\"evicted\"} 0\n"));
    // One HELP per metric name, even with several label variants.
    assert(body.find("# HELP srtla_groups_removed_total") ==
           body.rfind("# HELP srtla_groups_removed_total"));

    // Must be a unix timestamp, not the monotonic clock the receiver runs on,
    // or "time() - start" reads as decades of uptime.
    size_t at = body.find("\nsrtla_start_time_seconds ") + 26;
    assert(std::stoll(body.substr(at, body.find('\n', at) - at)) > 1700000000LL);

    metrics::inc(metrics::PACKETS_RECEIVED, 7);
    metrics::inc(metrics::GROUPS_REMOVED_EVICTED);
    assert(has(exporter.render(1000), "\nsrtla_packets_received_total 7\n"));
    assert(has(exporter.render(1000), "srtla_groups_removed_total{reason=\"evicted\"} 1\n"));

    char client_id[SRTLA_ID_LEN] = {};
    auto group = std::make_shared<connection::ConnectionGroup>(client_id, 900);
    auto conn = std::make_shared<connection::Connection>(make_addr("10.0.0.7", 5000), 990);
    conn->stats().bytes_received = 4242;
    conn->stats().rtt_ms = 31;
    group->add_connection(conn);
    group->mark_established();
    registry.add_group(group);

    body = exporter.render(1000);
    assert(has(body, "\nsrtla_groups 1\n"));
    assert(has(body, "\nsrtla_groups_established 1\n"));
    assert(has(body, "\nsrtla_connections 1\n"));
    assert(!has(body, "srtla_conn_bytes_received_total")); // tier 2 is off

    exporter.start("127.0.0.1", 0, -1, /*detailed=*/true);
    body = exporter.render(1000);
    assert(has(body, "srtla_conn_bytes_received_total{group=\"0\",remote=\"10.0.0.7:5000\"} 4242\n"));
    assert(has(body, "srtla_conn_rtt_ms{group=\"0\",remote=\"10.0.0.7:5000\"} 31\n"));
    assert(has(body, "srtla_conn_idle_seconds{group=\"0\",remote=\"10.0.0.7:5000\"} 10\n"));
    assert(has(body, "srtla_conn_weight_percent{group=\"0\",remote=\"10.0.0.7:5000\"} 100\n"));

    std::cout << "test_metrics: ok" << std::endl;
    return 0;
}
