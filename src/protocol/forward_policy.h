#pragma once

#include <cerrno>

namespace srtla::protocol {

// Classifies a failed send() on a group's SRT socket (the non-blocking,
// connected UDP socket that carries client packets to the SRT server).
//
// Transient: the local send path is momentarily out of room. The typical cause
// is a sender flushing its backlog after an outage, so packets arrive faster
// than the socket drains. Dropping the one packet keeps the stream alive: the
// SRT receiver NAKs the gap as soon as a later packet arrives, and the sender
// retransmits it over the bond. Tearing the group down instead turns one
// recoverable packet into a full stream reconnect.
//
// Recovery needs that later packet. In live mode with periodic NAK reports
// (libsrt's default) the sender does no blind retransmission, so a packet
// dropped as the very last one of a stream is not recovered. In a live stream
// that case does not arise mid-stream: a full buffer means more packets are
// queued right behind the dropped one. It is the same loss any network hop
// could cause, and before this change that packet was lost together with the
// whole group.
//
// Anything else (e.g. ECONNREFUSED after the SRT server went away) is not
// something another packet can recover from, and still ends the group.
// Returns true when a failed forward should drop the packet and keep the group.
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
