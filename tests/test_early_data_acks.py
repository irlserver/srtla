"""Exercise one local SRTLA receiver without a downstream SRT dependency."""
import argparse
import selectors
import socket
import struct
import time


def exchange(sock, peer, message, kind):
    sock.sendto(message, peer)
    deadline = time.monotonic() + 2
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError(f"no packet with type {kind:#06x}")
        sock.settimeout(left)
        packet = sock.recv(2048)
        if packet[:2] == struct.pack("!H", kind):
            return packet


def drain_acks(sockets, wait):
    acks = [[] for _ in sockets]
    deadline = time.monotonic() + wait
    with selectors.DefaultSelector() as pending:
        for index, sock in enumerate(sockets):
            pending.register(sock, selectors.EVENT_READ, index)
        while (left := deadline - time.monotonic()) > 0:
            for event, _ in pending.select(left):
                packet = event.fileobj.recv(2048)
                if packet[:2] == b"\x91\0":
                    if len(packet) < 8 or len(packet) % 4 or packet[2:4] != b"\0\0":
                        raise AssertionError(f"invalid SRTLA ACK: {packet.hex()}")
                    acks[event.data].append(list(struct.unpack("!" + "I" * ((len(packet) - 4) // 4), packet[4:])))
    return acks


def test(port, early):
    peer = ("127.0.0.1", port)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as first, \
         socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as second, \
         socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as unknown, \
         socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
        sink.bind(("127.0.0.1", port + 1))
        both = [first, second]
        for sock in both:
            sock.bind(("127.0.0.1", 0))
            sock.settimeout(2)
        group = exchange(first, peer, struct.pack("!H", 0x9200) + bytes(range(256)), 0x9201)
        for sock in both:
            exchange(sock, peer, group, 0x9202)
        unknown.sendto(struct.pack("!II", 999, 0) + bytes(1308), peer)
        if drain_acks([first, second, unknown], 0.1) != [[], [], []]:
            raise AssertionError("DATA from an unknown address generated an ACK")
        for sequence in range(1, 14):
            if sequence == 4:
                exchange(first, peer, group, 0x9202)
            for index, sock in enumerate(both):
                # Include unmarked and marked DATA on both links.
                packet = struct.pack("!II", sequence + index * 100, 0x04000000 if sequence % 2 else 0)
                sock.sendto(packet + bytes(1316 - len(packet)), peer)
            actual = drain_acks(both, 0.1)
            for index, batches in enumerate(actual):
                offset = index * 100
                if early and sequence <= 3:
                    expected = [[offset + sequence]]
                elif sequence == (13 if early else 10):
                    expected = [list(range(offset + sequence - 9, offset + sequence + 1))]
                else:
                    expected = []
                if batches != expected:
                    raise AssertionError((sequence, index, batches, expected))
    print("PASS: two sockets, exact ACK boundaries and marked/unmarked sequences")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int)
    parser.add_argument("--early", action="store_true")
    args = parser.parse_args()
    test(args.port, args.early)
