/*
** This file is a part of PRADS.
**
** Copyright (C) 2009, Redpill Linpro
** Copyright (C) 2009, Edward Fjellskål <edward.fjellskaal@redpill-linpro.com>
** Copyright (C) 2009, Kacper Wysocki   <kacper.wysocki@redpill-linpro.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
*/

/*  I N C L U D E S  *********************************************************/
#include "common.h"
#include "prads.h"
#include "sys_func.h"
#include "assets.h"
#include "cxt.h"
#include "ipfp/ipfp.h"
#include "servicefp/servicefp.h"

/*  G L O B A L E S  *********************************************************/
uint64_t cxtrackerid;
time_t timecnt, tstamp;
pcap_t *handle;
connection *bucket[BUCKET_SIZE];
connection *cxtbuffer = NULL;
asset *passet[BUCKET_SIZE];
port_t *lports[255];
signature *sig_serv_tcp = NULL;
signature *sig_serv_udp = NULL;
signature *sig_client_tcp = NULL;
signature *sig_client_udp = NULL;
char src_s[INET6_ADDRSTRLEN], dst_s[INET6_ADDRSTRLEN];
static char *dev, *dpath;
char *chroot_dir;
char *group_name, *user_name, *true_pid_name;
char *pidfile = "prads.pid";
char *pidpath = "/var/run";
int verbose, inpacket, gameover, use_syslog, intr_flag, s_check;
uint64_t hash;
// default source net owns everything
char *s_net = "0.0.0.0/0,::/0";
int nets = 1;
//char *s_net = "87.238.44.0/255.255.255.0,87.238.45.0/26,87.238.44.60/32";
struct fmask { 
    int type;
    struct in6_addr addr;
    struct in6_addr mask;
};
struct fmask network[MAX_NETS];
//struct in6_addr netmask[MAX_NETS];

// static strings for comparison
struct tagbstring tUNKNOWN = bsStatic("unknown");
bstring UNKNOWN = & tUNKNOWN;


/*  I N T E R N A L   P R O T O T Y P E S  ***********************************/
static void usage();

/* F U N C T I O N S  ********************************************************/

/* does this ip belong to our network? do we care about the packet?
 *
 * unfortunately pcap sends us packets in host order
 * Return value: boolean
 */
static inline int filter_packet(const int af, const struct in6_addr ip_s)
{
    uint32_t ip;
    int i, our = 0;
    char output[MAX_NETS];
    switch (af) {
        case AF_INET:
        {
            ip = ip_s.s6_addr32[0];
            for (i = 0; i < MAX_NETS && i < nets; i++) {
                if (network[i].type != AF_INET)
                    continue;
#if DEBUG == 2
                inet_ntop(af, &network[i].addr.s6_addr32[0], output, MAX_NETS);
                vlog(0x2, "Filter: %s\n", output);
                inet_ntop(af, &network[i].mask.s6_addr32[0], output, MAX_NETS);
                vlog(0x2, "mask: %s\n", output);
                inet_ntop(af, &ip, output, MAX_NETS);
                vlog(0x2, "ip: %s\n", output);
#endif
                if((ip & network[i].mask.s6_addr32[0])
                    == network[i].addr.s6_addr32[0]) {
                    our = 1;
                    break;
                }
            }
        }
        break;
        case AF_INET6:
        {
            /* 32-bit comparison of ipv6 nets.
             * can do better here by using 64-bit or SIMD instructions
             *
             *
             * PS: use same code for ipv4 */

            for (i = 0; i < MAX_NETS && i < nets; i++) {
                if(network[i].type != AF_INET6)
                    continue;
#if DEBUG == 2
                inet_ntop(af, &network[i].addr, output, MAX_NETS);
                dlog("net:  %s\n", output);
                inet_ntop(af, &network[i].mask, output, MAX_NETS);
                dlog("mask: %s\n", output);
                inet_ntop(af, &ip_s, output, MAX_NETS);
                dlog("ip: %s\n", output);
#endif
                if (network[i].type == AF_INET6
                    && (ip_s.s6_addr32[0] & network[i].mask.s6_addr32[0])
                    == network[i].addr.s6_addr32[0]
                    && (ip_s.s6_addr32[1] & network[i].mask.s6_addr32[1])
                    == network[i].addr.s6_addr32[1]
                    && (ip_s.s6_addr32[2] & network[i].mask.s6_addr32[2])
                    == network[i].addr.s6_addr32[2]
                    && (ip_s.s6_addr32[3] & network[i].mask.s6_addr32[3])
                    == network[i].addr.s6_addr32[3]) {
                    our = 1;
                    break;
                }
            }
        }
        break;
        default:
        fprintf(stderr,
            "non-ip packets of type %d aren't filtered by netmask yet\n", af);
            our = 1;
    }
#ifdef DEBUG
    if (af == AF_INET6){
        inet_ntop(af, &ip_s, output, MAX_NETS);
    }else{
        inet_ntop(af, &ip, output, MAX_NETS);
    }
    if (our){
        vlog(0x2, "Address %s is in our network.\n", output);
    } else {
        vlog(0x2, "Address %s is not our network.\n", output);
    }
#endif
    return our;
}

void got_packet(u_char * useless, const struct pcap_pkthdr *pheader,
                const u_char * packet)
{
    int our = 1;
    tstamp = pheader->ts.tv_sec;
    if (intr_flag != 0) {
        // printf("[*] Checking interrupt...\n"); 
        check_interupt();
    }
    inpacket = 1;
    s_check = 0;                // do we need to ?
    u_short p_bytes;

    // unwrap ethernet
    ether_header *eth_hdr;
    eth_hdr = (ether_header *) (packet);
    u_short eth_type;
    eth_type = ntohs(eth_hdr->eth_ip_type);
    int eth_header_len;
    eth_header_len = ETHERNET_HEADER_LEN;

    /*
     * while (ETHERNET_TYPE_X) check for infinit vlan tags
     */
    if (eth_type == ETHERNET_TYPE_8021Q) {
        vlog(0x3, "[*] ETHERNET TYPE 8021Q\n"); 
        eth_type = ntohs(eth_hdr->eth_8_ip_type);
        eth_header_len += 4;
    } else if (eth_type ==
               (ETHERNET_TYPE_802Q1MT | ETHERNET_TYPE_802Q1MT2 |
                ETHERNET_TYPE_802Q1MT3 | ETHERNET_TYPE_8021AD)) {
        vlog(0x3, "[*] ETHERNET TYPE 802Q1MT\n"); 
        eth_type = ntohs(eth_hdr->eth_82_ip_type);
        eth_header_len += 8;
    }

    if (eth_type == ETHERNET_TYPE_IP) {
        vlog(0x3, "[*] Got IPv4 Packet...\n");
        ip4_header *ip4;
        ip4 = (ip4_header *) (packet + eth_header_len);
        p_bytes = (ip4->ip_len - (IP_HL(ip4) * 4));
        struct in6_addr ip_src, ip_dst;
        ip_src.s6_addr32[0] = ip4->ip_src;
        ip_src.s6_addr32[1] = 0;
        ip_src.s6_addr32[2] = 0;
        ip_src.s6_addr32[3] = 0;
        ip_dst.s6_addr32[0] = ip4->ip_dst;
        ip_dst.s6_addr32[1] = 0;
        ip_dst.s6_addr32[2] = 0;
        ip_dst.s6_addr32[3] = 0;

        /*
         * not our network?
         */
        our = filter_packet(AF_INET, ip_src);
        if (ip4->ip_p == IP_PROTO_TCP) {
            tcp_header *tcph;
            tcph =
                (tcp_header *) (packet + eth_header_len +
                                (IP_HL(ip4) * 4));
            vlog(0x3, "[*] IPv4 PROTOCOL TYPE TCP:\n");
            

            s_check =
                cx_track(ip_src, tcph->src_port, ip_dst,
                         tcph->dst_port, ip4->ip_p, p_bytes,
                         tcph->t_flags, tstamp, AF_INET);
            if (!our)
                goto packet_end;

            if (TCP_ISFLAGSET(tcph, (TF_SYN))
                && !TCP_ISFLAGSET(tcph, (TF_ACK))) {
                // Redundant - fp_tcp4 & update_asset_service will do this!
                //update_asset(AF_INET,ip_src);
                /*
                 * Paranoia!
                 */
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_tcp4(ip4, tcph, end_ptr, TF_SYN, ip_src);
                vlog(0x3, "[*] - Got a SYN from a CLIENT: dst_port:%d\n",ntohs(tcph->dst_port));
                update_asset_service(ip_src,
                                     tcph->dst_port,
                                     ip4->ip_p,
                                     UNKNOWN,
                                     UNKNOWN, AF_INET, CLIENT);
            } else if (TCP_ISFLAGSET(tcph, (TF_SYN))
                       && TCP_ISFLAGSET(tcph, (TF_ACK))) {
                vlog(0x3, "[*] Got a SYNACK from a SERVER: src_port:%d\n",ntohs(tcph->src_port));
                //update_asset(AF_INET,ip_src);

                /*
                 * Paranoia!
                 */
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_tcp4(ip4, tcph, end_ptr, TF_SYNACK, ip_src);
                update_asset_service(ip_src,
                                     tcph->src_port,
                                     ip4->ip_p,
                                     UNKNOWN,
                                     UNKNOWN, AF_INET, SERVICE);

            } else if (TCP_ISFLAGSET(tcph, (TF_FIN))) {
                /*
                 * This is for test and phun (RST/FIN etc)
                 */
                //update_asset(AF_INET,ip_src);
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_tcp4(ip4, tcph, end_ptr, TF_FIN, ip_src);

            } else if (TCP_ISFLAGSET(tcph, (TF_RST))) {
                /*
                 * This is for test and phun (RST/FIN etc)
                 */
                //update_asset(AF_INET,ip_src);
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_tcp4(ip4, tcph, end_ptr, TF_RST, ip_src);
            }

            if (s_check != 0) {
                //printf("[*] - CHECKING TCP PACKAGE\n");
                //update_asset(AF_INET,ip_src);
                if (TCP_ISFLAGSET(tcph, (TF_ACK))
                    && !TCP_ISFLAGSET(tcph, (TF_ACK))
                    && !TCP_ISFLAGSET(tcph, (TF_RST))
                    && !TCP_ISFLAGSET(tcph, (TF_FIN))) {
                    //printf("[*] Got a STRAY-ACK: src_port:%d\n",ntohs(tcph->src_port));
                    /*
                     * Paranoia!
                     */
                    const uint8_t *end_ptr;
                    if (pheader->len <= SNAPLENGTH) {
                        end_ptr = (packet + pheader->len);
                    } else {
                        end_ptr = (packet + SNAPLENGTH);
                    }
                    fp_tcp4(ip4, tcph, end_ptr, TF_ACK, ip_src);
                }
                char *payload;
                payload =
                    (char *)(packet + eth_header_len +
                             (IP_HL(ip4) * 4) + (TCP_OFFSET(tcph) * 4));
                if (s_check == 2) {
                    service_tcp4(ip4, tcph, payload,
                                 (pheader->caplen -
                                  (TCP_OFFSET(tcph)) * 4 - eth_header_len));
                }
                /*
                 * if (s_check == 1)
                 */
                else {
                    client_tcp4(ip4, tcph, payload,
                                (pheader->caplen -
                                 (TCP_OFFSET(tcph)) * 4 - eth_header_len));
                }
            } else {
                //printf("[*] - NOT CHECKING TCP PACKAGE\n");
            }
            goto packet_end;
        } else if (ip4->ip_p == IP_PROTO_UDP) {
            udp_header *udph;
            udph =
                (udp_header *) (packet + eth_header_len +
                                (IP_HL(ip4) * 4));
            /*
             * printf("[*] IPv4 PROTOCOL TYPE UDP:\n");
             */

            s_check =
                cx_track(ip_src, udph->src_port, ip_dst,
                         udph->dst_port, ip4->ip_p, p_bytes, 0,
                         tstamp, AF_INET);
            if (!our)
                goto packet_end;

            if (s_check != 0) {
                //printf("[*] - CHECKING UDP PACKAGE\n");
                //update_asset(AF_INET,ip_src);
                char *payload;
                payload =
                    (char *)(packet + eth_header_len +
                             (IP_HL(ip4) * 4) + UDP_HEADER_LEN);
                service_udp4(ip4, udph, payload,
                             (pheader->caplen -
                              UDP_HEADER_LEN -
                              (IP_HL(ip4) * 4) - eth_header_len));

                /*
                 * Paranoia!
                 */
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_udp4(ip4, udph, end_ptr, ip_src);
            } else {
                //printf("[*] - NOT CHECKING UDP PACKAGE\n");
            }
            goto packet_end;
        } else if (ip4->ip_p == IP_PROTO_ICMP) {
            icmp_header *icmph;
            icmph =
                (icmp_header *) (packet + eth_header_len +
                                 (IP_HL(ip4) * 4));
            /*
             * printf("[*] IP PROTOCOL TYPE ICMP\n");
             */

            s_check =
                cx_track(ip_src, icmph->s_icmp_id, ip_dst,
                         icmph->s_icmp_id, ip4->ip_p, p_bytes,
                         0, tstamp, AF_INET);
            if (!our)
                goto packet_end;

            if (s_check != 0) {
                /*
                 * printf("[*] - CHECKING ICMP PACKAGE\n"); 
                 * * Paranoia! 
                 */
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_icmp4(ip4, icmph, end_ptr, ip_src);
                //update_asset(AF_INET,ip_src);
                /*
                 * service_icmp(*ip4,*tcph) // could look for icmp spesific data in package abcde...
                 */
            } else {
                /*
                 * printf("[*] - NOT CHECKING ICMP PACKAGE\n");
                 */
            }
            goto packet_end;
        } else {
            printf("[*] IPv4 PROTOCOL TYPE OTHER: %d\n", ip4->ip_p);

            s_check =
                cx_track(ip_src, 0, ip_dst, 0, ip4->ip_p,
                         p_bytes, 0, tstamp, AF_INET);
            if (!our)
                goto packet_end;

            if (s_check != 0) {
                /*
                 * printf("[*] - CHECKING OTHER PACKAGE\n");
                 */
                update_asset(AF_INET, ip_src);
                /* service_other(*ip4,*tcph)
                 * fp_other(ip, ttl, ipopts, len, id, ipflags, df);
                 */
            } else {
                /*
                 * printf("[*] - NOT CHECKING OTHER PACKAGE\n");
                 */
            }
            goto packet_end;
        }
    } else if (eth_type == ETHERNET_TYPE_IPV6) {
        ip6_header *ip6;
        ip6 = (ip6_header *) (packet + eth_header_len);
        our = filter_packet(AF_INET6, ip6->ip_src);
        dlog("Got %s IPv6 Packet...\n", (our?"our":"foregin"));

        if (ip6->next == IP_PROTO_TCP) {
            tcp_header *tcph;
            tcph =
                (tcp_header *) (packet + eth_header_len + IP6_HEADER_LEN);
            /*
             * printf("[*] IPv6 PROTOCOL TYPE TCP:\n");
             */

            s_check =
                cx_track(ip6->ip_src, tcph->src_port,
                         ip6->ip_dst, tcph->dst_port,
                         ip6->next, ip6->len, tcph->t_flags,
                         tstamp, AF_INET6);
            if (!our)
                goto packet_end;

            if (TCP_ISFLAGSET(tcph, (TF_SYN))
                && !TCP_ISFLAGSET(tcph, (TF_ACK))) {
                /*
                 * Paranoia!
                 */
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_tcp6(ip6, tcph, end_ptr, TF_SYN, ip6->ip_src);
                /*
                 * printf("[*] - Got a SYN from a CLIENT: dst_port:%d\n",ntohs(tcph->dst_port));
                 */
            } else if (TCP_ISFLAGSET(tcph, (TF_SYN))
                       && TCP_ISFLAGSET(tcph, (TF_ACK))) {
                /*
                 * printf("[*] - Got a SYNACK from a SERVER: src_port:%d\n",ntohs(tcph->src_port));
                 */
                /*
                 * Paranoia!
                 */
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_tcp6(ip6, tcph, end_ptr, TF_SYNACK, ip6->ip_src);
            }
            if (s_check != 0) {
                /*
                 * printf("[*] - CHECKING TCP PACKAGE\n");
                 */
                //update_asset(AF_INET6,ip6->ip_src);
                if (TCP_ISFLAGSET(tcph, (TF_ACK))
                    && !TCP_ISFLAGSET(tcph, (TF_SYN))) {
                    /*
                     * Paranoia!
                     */
                    const uint8_t *end_ptr;
                    if (pheader->len <= SNAPLENGTH) {
                        end_ptr = (packet + pheader->len);
                    } else {
                        end_ptr = (packet + SNAPLENGTH);
                    }
                    fp_tcp6(ip6, tcph, end_ptr, TF_ACK, ip6->ip_src);
                }
                char *payload;
                payload =
                    //(char *)(packet + eth_header_len + sizeof(ip6_header));
                    (char *)(packet + eth_header_len + IP6_HEADER_LEN + (TCP_OFFSET(tcph)*4));
                    //(char *)(packet + eth_header_len + IP6_HEADER_LEN);
                if (s_check == 2) {
                    /*
                     * printf("[*] - CHECKING TCP SERVER PACKAGE\n");
                     */
                    service_tcp6(ip6, tcph, payload,
                                 (pheader->caplen - (TCP_OFFSET(tcph)*4) -
                                  IP6_HEADER_LEN - eth_header_len));
                } else {
                    /*
                     * printf("[*] - CHECKING TCP CLIENT PACKAGE\n");
                     */
                    client_tcp6(ip6, tcph, payload,
                                (pheader->caplen - (TCP_OFFSET(tcph)*4) -
                                 IP6_HEADER_LEN - eth_header_len));
                }
            } else {
                /*
                 * printf("[*] - NOT CHECKING TCP PACKAGE\n");
                 */
            }
            goto packet_end;
            return;
        } else if (ip6->next == IP_PROTO_UDP) {
            udp_header *udph;
            udph =
                (udp_header *) (packet + eth_header_len + IP6_HEADER_LEN);
            /*
             * printf("[*] IPv6 PROTOCOL TYPE UDP:\n");
             */

            s_check =
                cx_track(ip6->ip_src, udph->src_port,
                         ip6->ip_dst, udph->dst_port,
                         ip6->next, ip6->len, 0, tstamp, AF_INET6);
            if (s_check != 0) {
                /*
                 * printf("[*] - CHECKING UDP PACKAGE\n");
                 */
                //update_asset(AF_INET6,ip6->ip_src);
                /*
                 * fp_udp(ip6, ttl, ipopts, len, id, ipflags, df);
                 */
                char *payload;
                payload =
                    (char *)(packet + eth_header_len + IP6_HEADER_LEN + UDP_HEADER_LEN);
                service_udp6(ip6, udph, payload,
                             (pheader->caplen - UDP_HEADER_LEN -
                              IP6_HEADER_LEN - eth_header_len));
            } else {
                /*
                 * printf("[*] - NOT CHECKING UDP PACKAGE\n");
                 */
            }
            goto packet_end;
        } else if (ip6->next == IP6_PROTO_ICMP) {
            icmp6_header *icmph;
            icmph =
                (icmp6_header *) (packet + eth_header_len +
                                  IP6_HEADER_LEN);
            /*
             * printf("[*] IPv6 PROTOCOL TYPE ICMP\n");
             */

            /*
             * DO change ip6->hop_lmt to 0 or something!
             */
            s_check = cx_track(ip6->ip_src, 0, ip6->ip_dst,
                               0, ip6->next, ip6->len, 0,
                               tstamp, AF_INET6);
            if (s_check != 0) {
                /*
                 * printf("[*] - CHECKING ICMP PACKAGE\n");
                 */
                //update_asset(AF_INET6,ip6->ip_src);
                /*
                 * service_icmp(*ip6,*tcph)
                 */

                /*
                 * Paranoia!
                 */
                const uint8_t *end_ptr;
                if (pheader->len <= SNAPLENGTH) {
                    end_ptr = (packet + pheader->len);
                } else {
                    end_ptr = (packet + SNAPLENGTH);
                }
                fp_icmp6(ip6, icmph, end_ptr, ip6->ip_src);
            } else {
                /*
                 * printf("[*] - NOT CHECKING ICMP PACKAGE\n");
                 */
            }
            goto packet_end;
        } else {
            printf("[*] IPv6 PROTOCOL TYPE OTHER: %d\n", ip6->next);
            /*
             * s_check = cx_track(ip6->ip_src, 0, ip6->ip_dst, 0,
             * ip6->next, ip6->len, 0, tstamp, AF_INET6);
             * if (s_check != 0) { 
             * printf("[*] - CHECKING OTHER PACKAGE\n"); 
             * update_asset(AF_INET6,ip6->ip_src); 
             * service_other(*ip4,*tcph) 
             * fp_other(ip, ttl, ipopts, len, id, ipflags, df); 
             * }else{ 
             * printf("[*] - NOT CHECKING OTHER PACKAGE\n"); 
             * } 
             */
            goto packet_end;
        }
    } else if (eth_type == ETHERNET_TYPE_ARP) {
        /*
         * printf("[*] Got ARP Packet...\n");
         */
        ether_arp *arph;
        arph = (ether_arp *) (packet + eth_header_len);

        if (ntohs(arph->ea_hdr.ar_op) == ARPOP_REPLY) {
            struct in6_addr ip_addr;
            memcpy(&ip_addr.s6_addr32[0], arph->arp_spa,
                   sizeof(u_int8_t) * 4);
            if (filter_packet(AF_INET, ip_addr)) {
                update_asset_arp(arph->arp_sha, ip_addr);
            }
            /*
             * arp_check(eth_hdr,tstamp);
             */
        } else {
            /*
             * printf("ARP TYPE: %d\n",ntohs(arph->ea_hdr.ar_op));
             */
        }
        goto packet_end;
    }
    /*
     * printf("[*] ETHERNET TYPE : %x\n", eth_hdr->eth_ip_type);
     */
  packet_end:
#ifdef DEBUG
    if (!our) vlog(0x3, "Not our network packet. Tracked, but not logged.\n");
#endif
    inpacket = 0;
    return;
}

/* parse strings of the form ip/cidr or ip/mask like:
 * "10.10.10.10/255.255.255.128,10.10.10.10/25" and 
 * "dead:be:eef2:1aa::b5ff:fe96:37a2/64,..."
 *
 * an IPv6 address is 8 x 4 hex digits. missing digits are padded with zeroes.
 */
void parse_nets(const char *s_net, struct fmask *network)
{
    /* f -> for processing
     * p -> frob pointer
     * t -> to pointer */
    char *f, *p, *t, *snet;
    char output[MAX_NETS];
    int type, len, i = 0;
    uint32_t mask, network4, netmask4;
    struct in6_addr network6, netmask6;

    // snet is a mutable copy of the args,freed @ nets_end
    len = strlen(s_net);
    snet = calloc(1, len);
    strncpy(snet, s_net, len);
    f = snet;
    while (f && 0 != (p = strchr(f, '/'))) {
        // convert network address
        *p = '\0';
        if (NULL != (t = strchr(f, ':'))) {
            type = AF_INET6;
            if (!inet_pton(type, f, &network6)) {
                perror("parse_nets6");
                goto nets_end;
            }
            printf("Network6 %-36s \t -> %08x:%08x:%08x:%08x\n",
                   f,
                   network6.s6_addr32[0],
                   network6.s6_addr32[1],
                   network6.s6_addr32[2],
                   network6.s6_addr32[3]
                   );
        } else {
            type = AF_INET;
            if (!inet_pton(type, f, &network4)) {
                perror("parse_nets");
                goto nets_end;
            }
            printf("Network4 %16s \t-> %010p\n", f, network4);
        }
        // convert netmask
        f = p + 1;
        p = strchr(f, ',');
        if (p) {
            *p = '\0';
        }

        // parse netmask into host order
        if (type == AF_INET && (t = strchr(f, '.'))-f < 4 && t > f) {
            // dotted quads
            inet_pton(type, f, &netmask4);
            printf("mask 4 %s \t-> %010p\n", f, netmask4);
        } else if (type == AF_INET6 && NULL != (t = strchr(f, ':'))) {
            // full ipv6 netmasĸ
            printf("mask 6 %s\n", f);
            inet_pton(type, f, &netmask6);
        } else {
            // cidr form
            sscanf(f, "%u", &mask);
            printf("cidr  %u \t-> ", mask);
            if (type == AF_INET) {
                uint32_t shift = 32 - mask;
                if (mask)
                    netmask4 = ntohl( ((unsigned int)-1 >> shift)<< shift);
                else
                    netmask4 = 0;

                printf("%010p\n", netmask4);
            } else if (type == AF_INET6) {
                //mask = 128 - mask;
                int j = 0;
                memset(&netmask6, 0, sizeof(struct in6_addr));

                while (mask > 8) {
                    netmask6.s6_addr[j++] = 0xff;
                    mask -= 8;
                }
                if (mask > 0) {
                    netmask6.s6_addr[j] = -1 << (8 - mask);
                }
                inet_ntop(type, &netmask6.s6_addr32[0], output, MAX_NETS);
                printf("mask: %s\n", output);
                // pcap packets are in host order.
                netmask6.s6_addr32[0] = ntohl(netmask6.s6_addr32[0]);
                netmask6.s6_addr32[1] = ntohl(netmask6.s6_addr32[1]);
                netmask6.s6_addr32[2] = ntohl(netmask6.s6_addr32[2]);
                netmask6.s6_addr32[3] = ntohl(netmask6.s6_addr32[3]);

            }
        }

        // poke in the gathered information
        switch (type) {
            case AF_INET:
                network[i].addr.s6_addr32[0] = network4;
                network[i].mask.s6_addr32[0] = netmask4;
                network[i].type = type;
                break;

            case AF_INET6:
                network[i].addr = network6;
                network[i].mask = netmask6;
                network[i].type = type;
                break;

            default:
                fprintf(stderr, "parse_nets: invalid address family!\n");
                goto nets_end;
        }

        nets = ++i;

        if (i > MAX_NETS) {
            elog("Max networks reached, stopped parsing at %lu nets.\n", i-1);
            goto nets_end;
        }


        // continue parsing at p, which might point to another network range
        f = p;
        if(p) f++;
    }
nets_end:
    free(snet);
    return;
}

static void usage()
{
    printf("USAGE:\n");
    printf(" $ prads [options]\n");
    printf("\n");
    printf(" OPTIONS:\n");
    printf("\n");
    printf(" -i             : network device (default: eth0)\n");
    printf(" -b             : berkeley packet filter\n");
    printf(" -d             : path to logdir\n");
    printf(" -u             : user\n");
    printf(" -g             : group\n");
    printf(" -D             : enables daemon mode\n");
    printf(" -h             : this help message\n");
    printf(" -v             : verbose\n");
    printf
        (" -a             : home nets (eg: '87.238.44.0/25,10.0.0.0/255.0.0.0')\n\n");
}

int main(int argc, char *argv[])
{
    printf("%08x =? %08x, endianness: %s\n\n", 0xdeadbeef, ntohl(0xdeadbeef), (0xdead == ntohs(0xdead)?"big":"little") );
    int ch, fromfile, setfilter, version, drop_privs_flag, daemon_flag;
    int use_syslog = 0;
    struct in_addr addr;
    struct bpf_program cfilter = {0};
    char *bpff, errbuf[PCAP_ERRBUF_SIZE], *user_filter;
    char *net_ip_string;
    bpf_u_int32 net_mask;
    ch = fromfile = setfilter = version = drop_privs_flag =
        daemon_flag = 0;
    dev = "eth0";
    bpff = "";
    dpath = "/tmp";
    cxtbuffer = NULL;
    cxtrackerid = 0;
    inpacket = gameover = intr_flag = 0;
    timecnt = time(NULL);

    signal(SIGTERM, game_over);
    signal(SIGINT, game_over);
    signal(SIGQUIT, game_over);
    signal(SIGALRM, set_end_sessions);

    while ((ch = getopt(argc, argv, "b:d:Dg:hi:p:P:u:va:")) != -1)
        switch (ch) {
        case 'a':
            s_net = strdup(optarg);
            break;
        case 'i':
            dev = strdup(optarg);
            break;
        case 'b':
            bpff = strdup(optarg);
            break;
        case 'v':
            verbose = 1;
            break;
        case 'd':
            dpath = strdup(optarg);
            break;
        case 'h':
            usage();
            exit(0);
            break;
        case 'D':
            daemon_flag = 1;
            break;
        case 'u':
            user_name = strdup(optarg);
            drop_privs_flag = 1;
            break;
        case 'g':
            group_name = strdup(optarg);
            drop_privs_flag = 1;
            break;
        case 'p':
            pidfile = strdup(optarg);
            break;
        case 'P':
            pidpath = strdup(optarg);
            break;
        default:
            exit(1);
            break;
        }

    if (getuid()) {
        printf("[*] You must be root..\n");
        return (1);
    }

    parse_nets(s_net, network);
    printf("[*] Running prads %s\n", VERSION);
    load_servicefp_file(1, "../etc/tcp-service.sig");
    load_servicefp_file(2, "../etc/udp-service.sig");
    load_servicefp_file(3, "../etc/tcp-clients.sig");
    //load_servicefp_file(4,"../etc/udp-client.sig");
    add_known_port(17,1194,bfromcstr("@openvpn"));
    add_known_port(17,123,bfromcstr("@ntp"));

    errbuf[0] = '\0';
    /*
     * look up an availible device if non specified
     */
    if (dev == 0x0)
        dev = pcap_lookupdev(errbuf);
    printf("[*] Device: %s\n", dev);

    if ((handle = pcap_open_live(dev, SNAPLENGTH, 1, 500, errbuf)) == NULL) {
        printf("[*] Error pcap_open_live: %s \n", errbuf);
        exit(1);
    } else if ((pcap_compile(handle, &cfilter, bpff, 1, net_mask)) == -1) {
        printf("[*] Error pcap_compile user_filter: %s\n",
               pcap_geterr(handle));
        exit(1);
    }

    pcap_setfilter(handle, &cfilter);

    /*
     * B0rk if we see an error...
     */
    if (strlen(errbuf) > 0) {
        printf("[*] Error errbuf: %s \n", errbuf);
        exit(1);
    }

    if (daemon_flag) {
        if (!is_valid_path(pidpath))
            printf
                ("[*] PID path \"%s\" is bad, check privilege.", pidpath);
        openlog("prads", LOG_PID | LOG_CONS, LOG_DAEMON);
        printf("[*] Daemonizing...\n\n");
        daemonize(NULL);
    }

    if (drop_privs_flag) {
        printf("[*] Dropping privs...\n\n");
        drop_privs();
    }
    bucket_keys_NULL();
    alarm(CHECK_TIMEOUT);

    printf("[*] Sniffing...\n\n");
    pcap_loop(handle, -1, got_packet, NULL);

    pcap_close(handle);
    return (0);
}
