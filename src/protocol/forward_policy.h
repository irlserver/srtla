#pragma once

#include <cerrno>

namespace srtla::protocol {

// Classifies a failed send() on a group's SRT socket (the non-blocking,
// connected UDP socket that carries client packets to the SRT server).
//
// Transient: the local send path is momentarily out of room. The typical cause
// is a sender flushing its backlog after an outage, so packets arrive faster
// than the socket drains. Dropping the one packet is safe because SRT sees the
// sequence gap, NAKs it, and the sender retransmits it over the bond. Tearing
// the group down instead turns one recoverable packet into a full stream
// reconnect.
//
// Anything else (e.g. ECONNREFUSED after the SRT server went away) is not
// something another packet can recover from, and still ends the group.
inline bool is_transient_forward_error(int err) {
    switch (err) {
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ENOBUFS:
    case EINTR:
        return true;
    default:
        return false;
    }
}

} // namespace srtla::protocol
