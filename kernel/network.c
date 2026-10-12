#include "network.h"
#include "rtl8139.h"

extern void net_write(const char *text);
extern void net_putc(char character);
extern void net_number(u32 number);
extern u32 net_ticks(void);
extern void net_idle(void);

enum { FRAME_CAP = 1600, ARP_SLOTS = 4, DHCP_PORT = 68, DNS_PORT = 49153,
       TCP_MSS = 1200, HTTP_LIMIT = 4096, SECOND = 100 };

struct arp_entry { u32 ip, updated; u8 mac[6]; };
static struct arp_entry arp_cache[ARP_SLOTS];
static u8 rx_frame[FRAME_CAP], tx_frame[FRAME_CAP];
static u32 local_ip, subnet_mask, gateway_ip, dns_ip, ip_ident, transaction, config_generation;
static u32 discarded;
static u8 configured, using_dhcp;
static u8 dhcp_phase, dhcp_attempts;
static u32 dhcp_xid, dhcp_sent, offered_ip, dhcp_server, lease_start, lease_ticks;
static u8 dns_pending, dns_done, dns_error;
static u16 dns_id;
static u32 dns_answer, dns_server;
static char dns_name[256];
static u8 ping_pending, ping_done;
static u16 ping_id;
static u32 ping_target;
static u8 echo_reply[1460], echo_pending, echo_attempts;
static u16 echo_length;
static u32 echo_target, echo_hop, echo_generation, echo_sent;
static u8 tcp_active, tcp_state, tcp_error, tcp_outstanding, tcp_attempts;
static u8 tcp_flags, tcp_truncated, tcp_peer_fin;
static u16 tcp_port, tcp_remote_port, tcp_payload_length, tcp_request_length;
static u32 tcp_remote, tcp_sequence, tcp_received, tcp_segment_sequence;
static u32 tcp_sent, tcp_start, tcp_printed, tcp_timewait_start;
static char tcp_request[640];

static void copy_bytes(void *destination, const void *source, u32 length)
{
    u8 *d = destination; const u8 *s = source;
    for (u32 i = 0; i < length; ++i) d[i] = s[i];
}
static void zero_bytes(void *destination, u32 length)
{
    u8 *d = destination;
    for (u32 i = 0; i < length; ++i) d[i] = 0;
}
static int same_bytes(const u8 *a, const u8 *b, u32 length)
{
    for (u32 i = 0; i < length; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static u16 get16(const u8 *p) { return (u16)((u16)p[0] << 8 | p[1]); }
static u32 get32(const u8 *p)
{ return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put16(u8 *p, u16 n) { p[0] = (u8)(n >> 8); p[1] = (u8)n; }
static void put32(u8 *p, u32 n)
{ p[0] = (u8)(n >> 24); p[1] = (u8)(n >> 16); p[2] = (u8)(n >> 8); p[3] = (u8)n; }
static u32 sum_bytes(u32 sum, const u8 *p, u32 length)
{
    for (; length >= 2; length -= 2, p += 2) sum += get16(p);
    if (length) sum += (u16)p[0] << 8;
    return sum;
}
static u16 finish_sum(u32 sum)
{
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return (u16)~sum;
}
static u16 checksum(const u8 *p, u32 length)
{ return finish_sum(sum_bytes(0, p, length)); }
static u16 transport_checksum(u32 src, u32 dst, u8 protocol, const u8 *p, u16 length)
{
    u32 sum = (src >> 16) + (src & 65535) + (dst >> 16) + (dst & 65535);
    return finish_sum(sum_bytes(sum + protocol + length, p, length));
}
static int elapsed(u32 start, u32 duration) { return net_ticks() - start >= duration; }
static void print_ip(u32 ip)
{
    for (u32 i = 0; i < 4; ++i) {
        net_number((ip >> (24 - i * 8)) & 255);
        if (i != 3) net_putc('.');
    }
}
static int parse_ip(const char *s, u32 *result)
{
    u32 ip = 0;
    for (u32 part = 0; part < 4; ++part) {
        u32 value = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (u32)(*s++ - '0');
            if (++digits > 3 || value > 255) return 0;
        }
        if (!digits) return 0;
        ip = (ip << 8) | value;
        if (part != 3) { if (*s++ != '.') return 0; }
    }
    if (*s) return 0;
    *result = ip;
    return 1;
}
static int usable_ip(u32 ip)
{ return (ip >> 24) && (ip >> 24) < 224 && (ip >> 24) != 127; }
static int usable_host(u32 ip, u32 mask)
{
    u32 host_mask = ~mask, host = ip & host_mask;
    return usable_ip(ip) && (host_mask <= 1 || (host && host != host_mask));
}
static u8 *arp_lookup(u32 ip)
{
    for (u32 i = 0; i < ARP_SLOTS; ++i)
        if (arp_cache[i].ip == ip && !elapsed(arp_cache[i].updated, 60 * SECOND))
            return arp_cache[i].mac;
    return 0;
}
static void ethernet_header(u8 *frame, const u8 *mac, u16 type)
{
    copy_bytes(frame, mac, 6); copy_bytes(frame + 6, rtl8139_mac(), 6);
    put16(frame + 12, type);
}
static void send_arp(u16 operation, u32 target, const u8 *mac)
{
    static const u8 broadcast[6] = {255,255,255,255,255,255};
    zero_bytes(tx_frame, 60);
    ethernet_header(tx_frame, operation == 1 ? broadcast : mac, 0x0806);
    u8 *p = tx_frame + 14;
    put16(p, 1); put16(p + 2, 0x0800); p[4] = 6; p[5] = 4;
    put16(p + 6, operation); copy_bytes(p + 8, rtl8139_mac(), 6);
    put32(p + 14, local_ip);
    if (operation == 2) copy_bytes(p + 18, mac, 6);
    put32(p + 24, target); rtl8139_send(tx_frame, 60);
}
static void accept_arp(const u8 *p, u32 length, const u8 *ethernet_source)
{
    if (length < 28 || get16(p) != 1 || get16(p + 2) != 0x0800 ||
        p[4] != 6 || p[5] != 4 || !same_bytes(p + 8, ethernet_source, 6)) return;
    u16 operation = get16(p + 6);
    if (operation != 1 && operation != 2) return;
    u32 sender = get32(p + 14), target = get32(p + 24);
    if (!configured || target != local_ip || !usable_ip(sender) ||
        (p[8] & 1) || (sender & subnet_mask) != (local_ip & subnet_mask)) return;
    u32 slot = 0;
    for (u32 i = 0; i < ARP_SLOTS; ++i) {
        if (arp_cache[i].ip == sender || !arp_cache[i].ip) { slot = i; break; }
        if (net_ticks() - arp_cache[i].updated > net_ticks() - arp_cache[slot].updated) slot = i;
    }
    arp_cache[slot].ip = sender; arp_cache[slot].updated = net_ticks();
    copy_bytes(arp_cache[slot].mac, p + 8, 6);
    if (operation == 1) send_arp(2, sender, p + 8);
}
static u32 next_hop(u32 destination)
{
    if ((destination & subnet_mask) == (local_ip & subnet_mask)) return destination;
    return gateway_ip;
}
static int resolve_arp(u32 destination)
{
    u32 hop = next_hop(destination);
    u32 generation = config_generation;
    if (!hop) { net_write("No gateway configured\n"); return 0; }
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        if (arp_lookup(hop)) return 1;
        send_arp(1, hop, 0);
        u32 start = net_ticks();
        while (!elapsed(start, SECOND)) {
            network_poll();
            if (!configured || generation != config_generation) {
                net_write("Network: IPv4 configuration changed\n"); return 0;
            }
            if (arp_lookup(hop)) return 1; net_idle();
        }
    }
    net_write("ARP: no reply\n"); return 0;
}
/* The transport payload is built at offset 34 before this function is called. */
static int send_ip(u32 destination, u8 protocol, u16 length, int broadcast)
{
    static const u8 broadcast_mac[6] = {255,255,255,255,255,255};
    const u8 *mac = broadcast ? broadcast_mac : arp_lookup(next_hop(destination));
    if (!mac || length > 1460) return 0;
    ethernet_header(tx_frame, mac, 0x0800);
    u8 *ip = tx_frame + 14; zero_bytes(ip, 20);
    ip[0] = 0x45; put16(ip + 2, (u16)(20 + length));
    put16(ip + 4, (u16)++ip_ident); put16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = protocol; put32(ip + 12, local_ip);
    put32(ip + 16, destination); put16(ip + 10, checksum(ip, 20));
    u32 frame_length = 34 + length;
    while (frame_length < 60) tx_frame[frame_length++] = 0;
    return rtl8139_send(tx_frame, frame_length);
}
static int send_udp(u32 destination, u16 source_port, u16 destination_port,
                    const u8 *data, u16 length, int broadcast)
{
    if (length > 1452) return 0;
    u8 *udp = tx_frame + 34;
    copy_bytes(udp + 8, data, length);
    put16(udp, source_port); put16(udp + 2, destination_port);
    put16(udp + 4, (u16)(length + 8)); put16(udp + 6, 0);
    u16 check = transport_checksum(local_ip, destination, 17, udp, (u16)(length + 8));
    put16(udp + 6, check ? check : 0xffff);
    return send_ip(destination, 17, (u16)(length + 8), broadcast);
}
static void dhcp_send(void)
{
    u8 packet[300]; zero_bytes(packet, sizeof(packet));
    packet[0] = 1; packet[1] = 1; packet[2] = 6;
    put32(packet + 4, dhcp_xid); put16(packet + 10, 0x8000);
    if (dhcp_phase == 4) put32(packet + 12, local_ip);
    copy_bytes(packet + 28, rtl8139_mac(), 6); put32(packet + 236, 0x63825363);
    u32 n = 240;
    packet[n++] = 53; packet[n++] = 1; packet[n++] = dhcp_phase == 1 ? 1 : 3;
    packet[n++] = 61; packet[n++] = 7; packet[n++] = 1;
    copy_bytes(packet + n, rtl8139_mac(), 6); n += 6;
    if (dhcp_phase == 2) {
        packet[n++] = 50; packet[n++] = 4; put32(packet + n, offered_ip); n += 4;
        packet[n++] = 54; packet[n++] = 4; put32(packet + n, dhcp_server); n += 4;
    }
    packet[n++] = 55; packet[n++] = 4;
    packet[n++] = 1; packet[n++] = 3; packet[n++] = 6; packet[n++] = 51;
    packet[n++] = 255;
    send_udp(0xffffffffu, 68, 67, packet, (u16)sizeof(packet), 1);
    dhcp_sent = net_ticks(); ++dhcp_attempts;
}
static void dhcp_begin(void)
{
    dhcp_phase = 1; dhcp_attempts = 0; offered_ip = 0; dhcp_server = 0;
    dhcp_xid = ++transaction ^ net_ticks() ^ 0x4f53704eu;
    using_dhcp = 1; dhcp_send();
}
static int valid_mask(u32 mask)
{
    u32 inverse = ~mask;
    return mask && (inverse & (inverse + 1)) == 0;
}
static void accept_dhcp(const u8 *p, u32 length, u32 source)
{
    if ((dhcp_phase != 1 && dhcp_phase != 2 && dhcp_phase != 4) ||
        length < 240 || p[0] != 2 || p[1] != 1 || p[2] != 6 ||
        get32(p + 4) != dhcp_xid || !same_bytes(p + 28, rtl8139_mac(), 6) ||
        get32(p + 236) != 0x63825363) return;
    u8 message = 0;
    u32 server = 0, mask = dhcp_phase == 4 ? subnet_mask : 0;
    u32 gateway = dhcp_phase == 4 ? gateway_ip : 0;
    u32 dns = dhcp_phase == 4 ? dns_ip : 0, lease = 3600;
    for (u32 n = 240; n < length;) {
        u8 code = p[n++]; if (code == 255) break; if (!code) continue;
        if (n >= length) return;
        u32 size = p[n++]; if (size > length - n) return;
        if (code == 53 && size == 1) message = p[n];
        if (code == 54 && size == 4) server = get32(p + n);
        if (code == 1 && size == 4) mask = get32(p + n);
        if (code == 3 && size >= 4) gateway = get32(p + n);
        if (code == 6 && size >= 4) dns = get32(p + n);
        if (code == 51 && size == 4) lease = get32(p + n);
        n += size;
    }
    /* Server identifier selects the lease owner; a relay or a multihomed
     * server may send the reply from a different unicast IPv4 address. */
    if (!usable_ip(server) || !usable_ip(source)) return;
    if (dhcp_phase == 1 && message == 2) {
        if (!usable_ip(get32(p + 16))) return;
        offered_ip = get32(p + 16); dhcp_server = server;
        dhcp_phase = 2; dhcp_attempts = 0; dhcp_send(); return;
    }
    if (dhcp_phase == 1 || server != dhcp_server) return;
    if (message == 6) {
        configured = 0; local_ip = 0; ++config_generation; dhcp_begin(); return;
    }
    if (message != 5 || !valid_mask(mask) || !lease) return;
    u32 address = get32(p + 16);
    if (dhcp_phase == 4 && !address) address = local_ip;
    if (!usable_host(address, mask) || (dhcp_phase == 2 && address != offered_ip)) return;
    if (gateway && (!usable_host(gateway, mask) || gateway == address ||
        (gateway & mask) != (address & mask))) return;
    if (dns && !usable_ip(dns)) return;
    if (local_ip != address || subnet_mask != mask || gateway_ip != gateway || dns_ip != dns) {
        ++config_generation; zero_bytes(arp_cache, sizeof(arp_cache));
    }
    local_ip = address; subnet_mask = mask; gateway_ip = gateway; dns_ip = dns;
    configured = 1;
    if (lease > 21474836) lease = 21474836;
    lease_ticks = lease * SECOND; lease_start = net_ticks(); dhcp_phase = 3;
}
static u8 lower(u8 c) { return c >= 'A' && c <= 'Z' ? (u8)(c + 'a' - 'A') : c; }
static int names_equal(const char *a, const char *b)
{
    for (u32 i = 0; i < 256; ++i) {
        if (lower((u8)a[i]) != lower((u8)b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}
/* Compression pointers are bounded by the packet and a finite jump count. */
static int dns_read_name(const u8 *packet, u32 length, u32 *offset, char *name)
{
    u32 cursor = *offset, end = 0, written = 0, hops = 0;
    while (cursor < length && ++hops <= 128) {
        u8 count = packet[cursor++];
        if ((count & 0xc0) == 0xc0) {
            if (cursor >= length) return 0;
            u32 destination = ((u32)(count & 63) << 8) | packet[cursor++];
            if (!end) end = cursor;
            if (destination >= length) return 0;
            cursor = destination; continue;
        }
        if (count & 0xc0) return 0;
        if (!count) { name[written] = 0; *offset = end ? end : cursor; return 1; }
        if (count > 63 || count > length - cursor || written + count + 1 > 255) return 0;
        if (written) name[written++] = '.';
        for (u32 i = 0; i < count; ++i) {
            u8 c = packet[cursor++];
            if (c <= 32 || c >= 127 || c == '.') return 0;
            name[written++] = (char)lower(c);
        }
    }
    return 0;
}
static void accept_dns(const u8 *p, u32 length, u32 source)
{
    if (!dns_pending || source != dns_server || length < 12 || get16(p) != dns_id ||
        (get16(p + 2) & 0xfa00) != 0x8000 || get16(p + 4) != 1) return;
    u32 n = 12; char name[256], canonical[256];
    if (!dns_read_name(p, length, &n, name) || !names_equal(name, dns_name) ||
        length - n < 4 || get16(p + n) != 1 || get16(p + n + 2) != 1) return;
    n += 4;
    if (get16(p + 2) & 15) { dns_error = 1; dns_done = 1; return; }
    copy_bytes(canonical, dns_name, sizeof(canonical));
    u32 answers = get16(p + 6);
    if (answers > 32) return;
    for (u32 i = 0; i < answers; ++i) {
        if (!dns_read_name(p, length, &n, name) || length - n < 10) return;
        u16 type = get16(p + n), class_id = get16(p + n + 2), size = get16(p + n + 8);
        n += 10; if (size > length - n) return;
        if (class_id == 1 && names_equal(name, canonical)) {
            if (type == 5) {
                u32 offset = n;
                if (!dns_read_name(p, length, &offset, canonical) || offset != n + size) return;
            } else if (type == 1 && size == 4 && usable_ip(get32(p + n))) {
                dns_answer = get32(p + n); dns_done = 1; return;
            }
        }
        n += size;
    }
    dns_error = 1; dns_done = 1;
}
static int make_dns_query(const char *name, u8 *query, u16 *length)
{
    zero_bytes(query, 12); put16(query, dns_id); put16(query + 2, 0x0100); put16(query + 4, 1);
    u32 n = 12, input = 0, output = 0;
    while (name[input]) {
        u32 label = n++, count = 0;
        while (name[input] && name[input] != '.') {
            u8 c = (u8)name[input++];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
            if (++count > 63 || output >= 253) return 0;
            query[n++] = c; dns_name[output++] = (char)lower(c);
        }
        if (!count) return 0;
        query[label] = (u8)count;
        if (name[input] == '.') {
            ++input;
            if (name[input]) { if (output >= 253) return 0; dns_name[output++] = '.'; }
        }
    }
    if (!output) return 0;
    dns_name[output] = 0;
    query[n++] = 0; put16(query + n, 1); put16(query + n + 2, 1);
    *length = (u16)(n + 4); return 1;
}
static int require_config(void)
{
    if (!rtl8139_present()) { net_write("Network: no RTL8139 adapter\n"); return 0; }
    if (!rtl8139_link_up()) { net_write("Network: link down\n"); return 0; }
    if (!configured) { net_write("Network: no IPv4 address; run dhcp or ifconfig\n"); return 0; }
    return 1;
}
static int resolve_name(const char *name, u32 *address)
{
    if (parse_ip(name, address)) {
        if (!usable_ip(*address)) { net_write("Invalid destination IPv4 address\n"); return 0; }
        return 1;
    }
    if (!dns_ip) { net_write("DNS: no server configured\n"); return 0; }
    u8 query[272]; u16 length = 0;
    dns_id = (u16)++transaction;
    if (!make_dns_query(name, query, &length)) { net_write("DNS: invalid hostname\n"); return 0; }
    dns_server = dns_ip;
    if (!resolve_arp(dns_server)) return 0;
    u32 generation = config_generation;
    dns_pending = 1; dns_done = 0; dns_error = 0;
    for (u32 attempt = 0; attempt < 3 && !dns_done; ++attempt) {
        send_udp(dns_server, DNS_PORT, 53, query, length, 0);
        u32 start = net_ticks();
        while (!elapsed(start, SECOND) && !dns_done) {
            network_poll();
            if (!configured || generation != config_generation) {
                dns_pending = 0; net_write("DNS: IPv4 configuration changed\n"); return 0;
            }
            if (!dns_done) net_idle();
        }
    }
    dns_pending = 0;
    if (!dns_done) { net_write("DNS: timed out\n"); return 0; }
    if (dns_error) { net_write("DNS: no IPv4 answer\n"); return 0; }
    *address = dns_answer; return 1;
}
static void accept_icmp(const u8 *p, u16 length, u32 source)
{
    if (length < 8 || checksum(p, length)) return;
    if (ping_pending && p[0] == 0 && !p[1] && source == ping_target &&
        get16(p + 4) == ping_id && get16(p + 6) == 1) ping_done = 1;
    if (configured && p[0] == 8 && !p[1] && length <= 1460) {
        if (arp_lookup(next_hop(source))) {
            copy_bytes(tx_frame + 34, p, length); tx_frame[34] = 0;
            put16(tx_frame + 36, 0); put16(tx_frame + 36, checksum(tx_frame + 34, length));
            send_ip(source, 1, length, 0);
        } else if (!echo_pending && next_hop(source)) {
            copy_bytes(echo_reply, p, length); echo_reply[0] = 0;
            put16(echo_reply + 2, 0); put16(echo_reply + 2, checksum(echo_reply, length));
            echo_length = length; echo_target = source; echo_hop = next_hop(source);
            echo_generation = config_generation; echo_pending = 1; echo_attempts = 1;
            echo_sent = net_ticks(); send_arp(1, echo_hop, 0);
        }
    }
}
static int sequence_before(u32 a, u32 b) { return (int)(a - b) < 0; }
static int tcp_transmit(u8 flags, u32 sequence, const u8 *payload, u16 length)
{
    u8 *p = tx_frame + 34; u16 header = flags & 2 ? 24 : 20;
    zero_bytes(p, header); put16(p, tcp_port); put16(p + 2, tcp_remote_port);
    put32(p + 4, sequence); put32(p + 8, tcp_received);
    p[12] = (u8)((header / 4) << 4); p[13] = flags; put16(p + 14, TCP_MSS * 2);
    if (flags & 2) { p[20] = 2; p[21] = 4; put16(p + 22, TCP_MSS); }
    if (length) copy_bytes(p + header, payload, length);
    put16(p + 16, transport_checksum(local_ip, tcp_remote, 6, p, (u16)(header + length)));
    return send_ip(tcp_remote, 6, (u16)(header + length), 0);
}
static void tcp_track(u8 flags, u16 length)
{
    tcp_segment_sequence = tcp_sequence; tcp_flags = flags; tcp_payload_length = length;
    tcp_outstanding = 1; tcp_attempts = 0; tcp_sent = net_ticks();
    tcp_transmit(flags, tcp_sequence, (const u8 *)tcp_request, length);
    tcp_sequence += length + ((flags & 3) ? 1 : 0);
}
static void tcp_ack(void) { tcp_transmit(0x10, tcp_sequence, 0, 0); }
static void tcp_finish(void)
{
    if (!tcp_outstanding && tcp_state == 2) {
        tcp_track(0x11, 0); tcp_state = tcp_peer_fin ? 4 : 3;
    }
}
static void tcp_closed(void)
{
    tcp_active = 0; tcp_state = 5; tcp_timewait_start = net_ticks();
}
static void accept_tcp(const u8 *p, u16 length, u32 source)
{
    if ((!tcp_active && tcp_state != 5) || source != tcp_remote || length < 20 || get16(p) != tcp_remote_port ||
        get16(p + 2) != tcp_port || transport_checksum(source, local_ip, 6, p, length)) return;
    u32 header = (u32)(p[12] >> 4) * 4;
    if (header < 20 || header > length) return;
    u8 flags = p[13]; u32 sequence = get32(p + 4), ack = get32(p + 8);
    u16 data_length = (u16)(length - header), trimmed = 0;
    if (!tcp_active) {
        if (!elapsed(tcp_timewait_start, SECOND * 2) && (flags & 0x15) == 0x11 &&
            sequence + data_length + 1 == tcp_received) tcp_ack();
        return;
    }
    if (tcp_state == 1) {
        if (!(flags & 0x10) || ack != tcp_sequence) return;
        if (flags & 4) { tcp_error = 1; tcp_active = 0; return; }
        if ((flags & 3) != 2 || length != header) return;
        tcp_received = sequence + 1; tcp_outstanding = 0; tcp_state = 2;
        tcp_ack(); tcp_track(0x18, tcp_request_length); return;
    }
    if (sequence != tcp_received) {
        if (flags & 4) return;
        if (!sequence_before(sequence, tcp_received) || tcp_received - sequence > data_length) {
            tcp_ack(); return;
        }
        trimmed = (u16)(tcp_received - sequence);
        data_length = (u16)(data_length - trimmed);
        tcp_ack();
    }
    if (flags & 4) { tcp_error = 1; tcp_active = 0; return; }
    if (flags & 2 || !(flags & 0x10) || sequence_before(tcp_sequence, ack)) return;
    if (tcp_outstanding && ack == tcp_sequence) {
        tcp_outstanding = 0;
        if (tcp_state == 4) { tcp_closed(); return; }
    }
    if (tcp_peer_fin && (data_length || (flags & 1))) { tcp_ack(); return; }
    if (data_length) {
        for (u32 i = 0; i < data_length; ++i) {
            if (tcp_printed < HTTP_LIMIT) {
                u8 c = p[header + trimmed + i];
                if (c == '\n' || c == '\t' || (c >= 32 && c <= 126)) net_putc((char)c);
                else if (c != '\r') net_putc('.');
                ++tcp_printed;
            } else tcp_truncated = 1;
        }
        tcp_received += data_length;
    }
    if (flags & 1) {
        ++tcp_received; tcp_peer_fin = 1; tcp_ack();
        if (tcp_state == 3) {
            tcp_state = 4;
            if (!tcp_outstanding) tcp_closed();
        } else tcp_finish();
    } else {
        if (data_length) tcp_ack();
        if (tcp_truncated || tcp_peer_fin) tcp_finish();
    }
}
static void accept_ipv4(const u8 *p, u32 length)
{
    if (length < 20 || p[0] >> 4 != 4) { ++discarded; return; }
    u32 header = (u32)(p[0] & 15) * 4;
    u16 total = get16(p + 2);
    if (header < 20 || header > length || total < header || total > length ||
        checksum(p, header) || (get16(p + 6) & 0x3fff) || !p[8]) { ++discarded; return; }
    u32 source = get32(p + 12), destination = get32(p + 16);
    const u8 *data = p + header; u16 size = (u16)(total - header);
    if (destination != local_ip && destination != 0xffffffffu &&
        !(using_dhcp && p[9] == 17 && size >= 8 &&
          get16(data) == 67 && get16(data + 2) == DHCP_PORT)) return;
    if (p[9] == 17 && size >= 8) {
        u16 source_port = get16(data), destination_port = get16(data + 2), udp_length = get16(data + 4);
        if (udp_length < 8 || udp_length > size ||
            (get16(data + 6) && transport_checksum(source, destination, 17, data, udp_length))) {
            ++discarded; return;
        }
        if (source_port == 67 && destination_port == DHCP_PORT)
            accept_dhcp(data + 8, udp_length - 8, source);
        if (configured && destination == local_ip && source_port == 53 && destination_port == DNS_PORT)
            accept_dns(data + 8, udp_length - 8, source);
    } else if (configured && destination == local_ip && usable_ip(source)) {
        if (p[9] == 1) accept_icmp(data, size, source);
        if (p[9] == 6) accept_tcp(data, size, source);
    }
}
void network_poll(void)
{
    if (!rtl8139_present()) return;
    u32 generation = config_generation;
    for (u32 count = 0; count < 16; ++count) {
        u32 length = rtl8139_receive(rx_frame, sizeof(rx_frame));
        if (!length) break;
        if (length < 14 || length > sizeof(rx_frame)) { ++discarded; continue; }
        static const u8 broadcast[6] = {255,255,255,255,255,255};
        if (!same_bytes(rx_frame, rtl8139_mac(), 6) && !same_bytes(rx_frame, broadcast, 6)) continue;
        u16 type = get16(rx_frame + 12);
        if (type == 0x0806) accept_arp(rx_frame + 14, length - 14, rx_frame + 6);
        if (type == 0x0800) accept_ipv4(rx_frame + 14, length - 14);
    }
    if (using_dhcp && configured && lease_ticks && elapsed(lease_start, lease_ticks)) {
        configured = 0; local_ip = 0; ++config_generation; dhcp_begin();
    }
    if (dhcp_phase == 3 && elapsed(lease_start, lease_ticks / 2)) {
        dhcp_phase = 4; dhcp_attempts = 0;
        dhcp_xid = ++transaction ^ net_ticks() ^ 0x4f53704eu; dhcp_send();
    }
    if ((dhcp_phase == 1 || dhcp_phase == 2 || dhcp_phase == 4) && elapsed(dhcp_sent, SECOND * 2)) {
        if (dhcp_attempts < 4) dhcp_send();
        else if (dhcp_phase == 4) { dhcp_attempts = 0; dhcp_sent = net_ticks(); }
        else dhcp_phase = 0;
    }
    if (echo_pending) {
        if (!configured || echo_generation != config_generation || !rtl8139_link_up()) {
            echo_pending = 0;
        } else if (arp_lookup(echo_hop)) {
            copy_bytes(tx_frame + 34, echo_reply, echo_length);
            send_ip(echo_target, 1, echo_length, 0); echo_pending = 0;
        } else if (elapsed(echo_sent, SECOND)) {
            if (echo_attempts >= 3) echo_pending = 0;
            else { send_arp(1, echo_hop, 0); ++echo_attempts; echo_sent = net_ticks(); }
        }
    }
    if (tcp_active && (!configured || generation != config_generation)) {
        tcp_active = 0; tcp_error = 3;
    } else if (tcp_active && (elapsed(tcp_start, SECOND * 15) ||
        (tcp_outstanding && elapsed(tcp_sent, SECOND) && tcp_attempts >= 4))) {
        tcp_transmit(0x14, tcp_sequence, 0, 0); tcp_active = 0; tcp_error = 2;
    } else if (tcp_active && tcp_outstanding && elapsed(tcp_sent, SECOND)) {
        tcp_transmit(tcp_flags, tcp_segment_sequence, (const u8 *)tcp_request, tcp_payload_length);
        ++tcp_attempts; tcp_sent = net_ticks();
    }
}
void network_initialize(void)
{
    if (rtl8139_initialize()) dhcp_begin();
}
void network_status(void)
{
    if (!rtl8139_present()) { net_write("Network: no RTL8139 adapter\n"); return; }
    net_write("Adapter: RTL8139\nLink: "); net_write(rtl8139_link_up() ? "up\nMAC: " : "down\nMAC: ");
    static const char hex[] = "0123456789abcdef";
    const u8 *mac = rtl8139_mac();
    for (u32 i = 0; i < 6; ++i) {
        net_putc(hex[mac[i] >> 4]); net_putc(hex[mac[i] & 15]); if (i != 5) net_putc(':');
    }
    net_write("\nIPv4: "); print_ip(local_ip);
    net_write("\nMask: "); print_ip(subnet_mask);
    net_write("\nGateway: "); print_ip(gateway_ip);
    net_write("\nDNS: "); print_ip(dns_ip);
    net_write("\nConfiguration: ");
    net_write(configured ? (using_dhcp ? "DHCP\n" : "static\n") :
              ((dhcp_phase == 1 || dhcp_phase == 2) ? "DHCP pending\n" : "unconfigured\n"));
    net_write("Packets RX/TX/errors: "); net_number(rtl8139_rx_count()); net_putc('/');
    net_number(rtl8139_tx_count()); net_putc('/'); net_number(rtl8139_error_count());
    net_write("\nDiscarded IPv4 packets: "); net_number(discarded); net_putc('\n');
}
void network_dhcp(void)
{
    if (!rtl8139_present()) { net_write("Network: no RTL8139 adapter\n"); return; }
    if (!rtl8139_link_up()) { net_write("Network: link down\n"); return; }
    configured = 0; local_ip = 0; ++config_generation; dhcp_begin();
    u32 start = net_ticks();
    while (dhcp_phase && dhcp_phase != 3 && !elapsed(start, SECOND * 18)) { network_poll(); net_idle(); }
    net_write(configured ? "DHCP: lease acquired\n" : "DHCP: timed out\n");
}
void network_ping(const char *target)
{
    if (!require_config()) return;
    if (!resolve_name(target, &ping_target) || !resolve_arp(ping_target)) return;
    ping_id = (u16)++transaction; ping_pending = 1; ping_done = 0;
    u32 generation = config_generation;
    u32 first = net_ticks();
    for (u32 attempt = 0; attempt < 3 && !ping_done; ++attempt) {
        u8 *p = tx_frame + 34; zero_bytes(p, 32);
        p[0] = 8; put16(p + 4, ping_id); put16(p + 6, 1);
        for (u32 i = 8; i < 32; ++i) p[i] = (u8)i;
        put16(p + 2, checksum(p, 32)); send_ip(ping_target, 1, 32, 0);
        u32 start = net_ticks();
        while (!elapsed(start, SECOND) && !ping_done) {
            network_poll();
            if (!configured || generation != config_generation) {
                ping_pending = 0; net_write("Ping: IPv4 configuration changed\n"); return;
            }
            if (!ping_done) net_idle();
        }
    }
    ping_pending = 0;
    if (!ping_done) { net_write("Ping: timed out\n"); return; }
    net_write("Reply from "); print_ip(ping_target); net_write(": time=");
    net_number((net_ticks() - first) * 10); net_write(" ms\n");
}
void network_dns(const char *hostname)
{
    u32 address;
    if (!require_config() || !resolve_name(hostname, &address)) return;
    net_write(hostname); net_write(" = "); print_ip(address); net_putc('\n');
}
static int append_text(char *destination, u32 *length, const char *text)
{
    while (*text) {
        if (*length >= sizeof(tcp_request) - 1) return 0;
        destination[(*length)++] = *text++;
    }
    return 1;
}
void network_http(const char *url)
{
    if (!require_config()) return;
    const char *prefix = "http://";
    for (u32 i = 0; i < 7; ++i) {
        if (!url[i] || url[i] != prefix[i]) {
            net_write("HTTP: use http://host[:port]/path; HTTPS/TLS is unsupported\n"); return;
        }
    }
    url += 7; char host[256], authority[264], path[512];
    u32 host_length = 0, authority_length = 0, path_length = 0;
    while (*url && *url != '/' && *url != ':') {
        if (host_length >= sizeof(host) - 1) { net_write("HTTP: hostname too long\n"); return; }
        host[host_length++] = *url; authority[authority_length++] = *url++;
    }
    host[host_length] = 0;
    if (!host_length) { net_write("HTTP: missing hostname\n"); return; }
    tcp_remote_port = 80;
    if (*url == ':') {
        authority[authority_length++] = *url++; u32 port = 0, digits = 0;
        while (*url >= '0' && *url <= '9') {
            if (++digits > 5 || authority_length >= sizeof(authority) - 1) break;
            port = port * 10 + (u32)(*url - '0'); authority[authority_length++] = *url++;
        }
        if (!digits || digits > 5 || !port || port > 65535 || (*url && *url != '/')) {
            net_write("HTTP: invalid port\n"); return;
        }
        tcp_remote_port = (u16)port;
    }
    authority[authority_length] = 0;
    if (!*url) path[path_length++] = '/';
    while (*url) {
        u8 c = (u8)*url++;
        if (c <= 32 || c >= 127 || path_length >= sizeof(path) - 1) {
            net_write("HTTP: invalid or too long path\n"); return;
        }
        if (c == '#') break;
        path[path_length++] = (char)c;
    }
    path[path_length] = 0;
    u32 request_length = 0;
    if (!append_text(tcp_request, &request_length, "GET ") ||
        !append_text(tcp_request, &request_length, path) ||
        !append_text(tcp_request, &request_length, " HTTP/1.0\r\nHost: ") ||
        !append_text(tcp_request, &request_length, authority) ||
        !append_text(tcp_request, &request_length, "\r\nConnection: close\r\nUser-Agent: OSp\r\n\r\n")) {
        net_write("HTTP: request too long\n"); return;
    }
    if (!resolve_name(host, &tcp_remote) || !resolve_arp(tcp_remote)) return;
    tcp_port = (u16)(49154 + (++transaction % 16000));
    tcp_sequence = net_ticks() * 4096 + transaction * 997 + 0x4f530000;
    tcp_received = 0; tcp_state = 1; tcp_error = 0; tcp_active = 1;
    tcp_printed = 0; tcp_truncated = 0; tcp_peer_fin = 0; tcp_start = net_ticks();
    tcp_request_length = (u16)request_length;
    net_write("HTTP response:\n");
    tcp_track(2, 0);
    while (tcp_active) { network_poll(); if (tcp_active) net_idle(); }
    net_putc('\n');
    if (tcp_truncated) net_write("HTTP: output limited to 4096 bytes\n");
    net_write(tcp_error == 1 ? "HTTP: connection reset\n" :
              (tcp_error == 2 ? "HTTP: timed out\n" :
               (tcp_error == 3 ? "HTTP: IPv4 configuration changed\n" : "HTTP: connection closed\n")));
}
void network_configure(char *arguments)
{
    char *parts[4]; u32 values[4];
    for (u32 i = 0; i < 4; ++i) {
        while (*arguments == ' ') ++arguments;
        if (!*arguments) { net_write("Usage: ifconfig IP MASK GATEWAY DNS\n"); return; }
        parts[i] = arguments;
        while (*arguments && *arguments != ' ') ++arguments;
        if (*arguments) *arguments++ = 0;
        if (!parse_ip(parts[i], values + i)) { net_write("Invalid IPv4 configuration\n"); return; }
    }
    while (*arguments == ' ') ++arguments;
    if (*arguments || !valid_mask(values[1]) || !usable_host(values[0], values[1]) ||
        (values[2] && (!usable_host(values[2], values[1]) || values[2] == values[0] ||
          (values[2] & values[1]) != (values[0] & values[1]))) ||
        (values[3] && !usable_ip(values[3]))) { net_write("Invalid IPv4 configuration\n"); return; }
    using_dhcp = 0; dhcp_phase = 0; configured = 1; local_ip = values[0]; subnet_mask = values[1];
    gateway_ip = values[2]; dns_ip = values[3]; zero_bytes(arp_cache, sizeof(arp_cache));
    ++config_generation;
    net_write("Static IPv4 configuration applied\n");
}
