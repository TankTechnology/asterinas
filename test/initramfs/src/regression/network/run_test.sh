#!/bin/sh

# SPDX-License-Identifier: MPL-2.0

set -e

# Keep this entry point usable both from the regression dispatcher (which
# changes into the test directory) and when invoked directly by a QEMU gate.
cd "$(dirname "$0")"

./tcp_server &
sleep 0.2
./tcp_client

./udp_server &
sleep 0.2
./udp_client

./unix_server &
sleep 0.2
./unix_client

./listen_backlog
./msg_peek
./msg_trunc
./privileged_ports
./send_buf_full
./sendmmsg
./socketpair
./sockoption
./sockoption_unix
./tcp_err
./tcp_poll
./tcp_reuseaddr
./tcp_wrapped_buffer_io
./ipv6_dual_stack
./ipv6_udp
./ipv6_dual_stack_udp
./udp_broadcast
./udp_err
./unix_datagram_err
./unix_seqpacket_err
./unix_scm_rights_acyclic
./unix_stream_err

./netlink_route
./rtnl_err
./uevent_err
