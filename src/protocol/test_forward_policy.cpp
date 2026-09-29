// Self-check for the forward-error policy. Run: ./build/test_forward_policy
#include <cassert>
#include <cerrno>
#include <iostream>

#include "forward_policy.h"

using srtla::protocol::is_transient_forward_error;

// Checks which send() errors drop the packet (true) and which end the group (false).
int main() {
    // Send buffer momentarily full: drop the packet, keep the group.
    assert(is_transient_forward_error(EAGAIN));
    assert(is_transient_forward_error(EWOULDBLOCK));
    assert(is_transient_forward_error(ENOBUFS));
    assert(is_transient_forward_error(EINTR));

    // The SRT server is gone or the socket is broken: end the group as before.
    assert(!is_transient_forward_error(ECONNREFUSED));
    assert(!is_transient_forward_error(EBADF));
    assert(!is_transient_forward_error(ENOTCONN));
    assert(!is_transient_forward_error(EMSGSIZE));
    assert(!is_transient_forward_error(0));

    std::cout << "test_forward_policy: OK\n";
    return 0;
}
