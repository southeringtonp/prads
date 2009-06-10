#
# NetPacket::IP - Decode and encode IP (Internet Protocol) packets. 
#
# Comments/suggestions to tpot@samba.org
#
# Encoding part by Stephanie Wehner, atrak@itsx.com
#
# $Id: IP.pm,v 1.16 2001/07/29 23:45:00 tpot Exp $
#

package NetPacket::IP;

#
# Copyright (c) 2001 Tim Potter.
#
# This package is free software and is provided "as is" without express 
# or implied warranty.  It may be used, redistributed and/or modified 
# under the terms of the Perl Artistic License (see
# http://www.perl.com/perl/misc/Artistic.html)
#
# Copyright (c) 1995,1996,1997,1998,1999 ANU and CSIRO on behalf of 
# the  participants in the CRC for Advanced Computational Systems
# ('ACSys').
#
# ACSys makes this software and all associated data and documentation
# ('Software') available free of charge.  You may make copies of the 
# Software but you must include all of this notice on any copy.
#
# The Software was developed for research purposes and ACSys does not
# warrant that it is error free or fit for any purpose.  ACSys
# disclaims any liability for all claims, expenses, losses, damages
# and costs any user may incur as a result of using, copying or
# modifying the Software.
#
# Copyright (c) 2001 Stephanie Wehner
#

use strict;
use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
use NetPacket;

our $VERSION = '0.41.1';

BEGIN {
    @ISA = qw(Exporter NetPacket);

# Items to export into callers namespace by default
# (move infrequently used names to @EXPORT_OK below)

    @EXPORT = qw( _FRAME _PARENT CKSUM DATA DEST_IP FLAGS FOFFSET
                  HLEN ID LEN OPTIONS PROTO SRC_IP TOS TTL VER );

    my $i = 0;
    for my $name (@EXPORT) {
        eval "use constant $name => $i; 1";
        $name = "IP_$name";
        $i++;
    }

# Other items we are prepared to export if requested

    @EXPORT_OK = qw(ip_strip
		    IP_PROTO_IP IP_PROTO_ICMP IP_PROTO_IGMP
		    IP_PROTO_IPIP IP_PROTO_TCP IP_PROTO_UDP
		    IP_VERSION_IPv4
		    IP_FLAG_MOREFRAGS IP_FLAG_DONTFRAG IP_FLAG_CONGESTION
                    IP_MAXPACKET
    );

# Tags:

    %EXPORT_TAGS = (
    ALL         => [@EXPORT, @EXPORT_OK],
    protos      => [qw(IP_PROTO_IP IP_PROTO_ICMP IP_PROTO_IGMP IP_PROTO_IPIP
		       IP_PROTO_TCP IP_PROTO_UDP)],
    versions    => [qw(IP_VERSION_IPv4)],
    strip       => [qw(ip_strip)],
    flags       => [qw(IP_FLAG_MOREFRAGS IP_FLAG_DONTFRAG IP_FLAG_CONGESTION)],
);

}

#
# Partial list of IP protocol values from RFC 1700
#

use constant IP_PROTO_IP   => 0;       # Dummy protocol for TCP
use constant IP_PROTO_ICMP => 1;       # Internet Control Message Protocol
use constant IP_PROTO_IGMP => 2;       # Internet Group Management Protocol
use constant IP_PROTO_IPIP => 4;       # IP in IP encapsulation
use constant IP_PROTO_TCP  => 6;       # Transmission Control Protocol
use constant IP_PROTO_UDP  => 17;      # User Datagram Protocol

#
# Partial list of IP version numbers from RFC 1700
#

use constant IP_VERSION_IPv4 => 4;     # IP version 4

#
# Flag values
#

use constant IP_FLAG_MOREFRAGS  => 1;     # More fragments coming
use constant IP_FLAG_DONTFRAG   => 2;     # Don't fragment me
use constant IP_FLAG_CONGESTION => 4;     # Congestion present

# Maximum IP Packet size
use constant IP_MAXPACKET => 65535;

# Convert 32-bit IP address to dotted quad notation

sub to_dotquad {
    my($net) = @_ ;
    my($na, $nb, $nc, $nd);

    $na = $net >> 24 & 255;
    $nb = $net >> 16 & 255;
    $nc = $net >>  8 & 255;
    $nd = $net & 255;

    return ("$na.$nb.$nc.$nd");
}

#
# Decode the packet
#

sub decode {
    my $class = shift;
    my($pkt, $parent, @rest) = @_;
    my $self = {};

    # Class fields

    $self->[_PARENT] = $parent;
    $self->[_FRAME] = $pkt;

    # Decode IP packet

    if (defined($pkt)) {
	my $tmp;

	($tmp, $self->[TOS],$self->[LEN], $self->[ID], $self->[FOFFSET],
	 $self->[TTL], $self->[PROTO], $self->[CKSUM], $self->[SRC_IP],
	 $self->[DEST_IP], $self->[OPTIONS]) = unpack('CCnnnCCnNNa*' , $pkt);

	# Extract bit fields
	
	$self->[VER] = ($tmp & 0xf0) >> 4;
	$self->[HLEN] = $tmp & 0x0f;
	
	$self->[FLAGS] = $self->[FOFFSET] >> 13;
	$self->[FOFFSET] = ($self->[FOFFSET] & 0x1fff) << 3;

	# Decode variable length header options and remaining data in field

	my $olen = $self->[HLEN] - 5;
	$olen = 0, if ($olen < 0);  # Check for bad hlen

	# Option length is number of 32 bit words

        $olen = $olen * 4;

	($self->[OPTIONS], $self->[DATA]) = unpack("a" . $olen .
						   "a*", $self->[OPTIONS]);

    my $length = $self->[HLEN];
    $length = 5 if $length < 5;  # precaution against bad header

    # truncate data to the length given by the header
    $self->[DATA] = substr $self->[DATA], 0, $self->[LEN] - 4 * $length;

	# Convert 32 bit ip addresses to dotted quad notation

	$self->[SRC_IP] = to_dotquad($self->[SRC_IP]);
	$self->[DEST_IP] = to_dotquad($self->[DEST_IP]);
    }

    return bless $self, $class;
}

#
# Strip header from packet and return the data contained in it
#

undef &ip_strip;           # Create ip_strip alias
*ip_strip = \&strip;

sub strip {
    my ($pkt, @rest) = @_;

    my $ip_obj = NetPacket::IP->decode($pkt);
    return $ip_obj->[DATA];
}   

#
# Encode a packet
#

sub encode {

    my $self = shift;
    my ($hdr,$packet,$zero,$tmp,$offset);
    my ($src_ip, $dest_ip);

    # create a zero variable
    $zero = 0;

    # adjust the length of the packet 
    $self->[LEN] = ($self->[HLEN] * 4) + length($self->[DATA]);

    $tmp = $self->[HLEN] & 0x0f;
    $tmp = $tmp | (($self->[VER] << 4) & 0xf0);

    $offset = $self->[FLAGS] << 13;
    $offset = $offset | (($self->[FOFFSET] >> 3) & 0x1fff);

    # convert the src and dst ip
    $src_ip = gethostbyname($self->[SRC_IP]);
    $dest_ip = gethostbyname($self->[DEST_IP]);

    # construct header to calculate the checksum
    $hdr = pack('CCnnnCCna4a4a*', $tmp, $self->[TOS],$self->[LEN], 
         $self->[ID], $offset, $self->[TTL], $self->[PROTO], 
         $zero, $src_ip, $dest_ip, $self->[OPTIONS]);

    $self->[CKSUM] = NetPacket::htons(NetPacket::in_cksum($hdr));

    # make the entire packet
    $packet = pack('CCnnnCCna4a4a*a*', $tmp, $self->[TOS],$self->[LEN], 
         $self->[ID], $offset, $self->[TTL], $self->[PROTO], 
         $self->[CKSUM], $src_ip, $dest_ip, $self->[OPTIONS],
         $self->[DATA]);

    return($packet);

}

#
# Module initialisation
#

1;

# autoloaded methods go after the END token (&& pod) below

__END__

=head1 NAME

C<NetPacket::IP> - Assemble and disassemble IP (Internet Protocol)
packets.

=head1 SYNOPSIS

  use NetPacket::IP;

  $ip_obj = NetPacket::IP->decode($raw_pkt);
  $ip_pkt = NetPacket::IP->encode($ip_obj);
  $ip_data = NetPacket::IP::strip($raw_pkt);

=head1 DESCRIPTION

C<NetPacket::IP> provides a set of routines for assembling and
disassembling packets using IP (Internet Protocol).  

=head2 Methods

=over

=item C<NetPacket::IP-E<gt>decode([RAW PACKET])>

Decode the raw packet data given and return an object containing
instance data.  This method will quite happily decode garbage input.
It is the responsibility of the programmer to ensure valid packet data
is passed to this method.

=item C<NetPacket::IP-E<gt>encode()>

Return an IP packet encoded with the instance data specified. This
will infer the total length of the packet automatically from the 
payload lenth and also adjust the checksum.

=back

=head2 Functions

=over

=item C<NetPacket::IP::strip([RAW PACKET])>

Return the encapsulated data (or payload) contained in the IP
packet.  This data is suitable to be used as input for other
C<NetPacket::*> modules.

This function is equivalent to creating an object using the
C<decode()> constructor and returning the C<data> field of that
object.

=back

=head2 Instance data

The instance data for the C<NetPacket::IP> object consists of
the following fields.

=over

=item ver

The IP version number of this packet.

=item hlen

The IP header length of this packet.

=item flags

The IP header flags for this packet.

=item foffset

The IP fragment offset for this packet.

=item tos

The type-of-service for this IP packet.

=item len

The length (including length of header) in bytes for this packet.

=item id

The identification (sequence) number for this IP packet.

=item ttl

The time-to-live value for this packet.

=item proto

The IP protocol number for this packet.

=item cksum

The IP checksum value for this packet.

=item src_ip

The source IP address for this packet in dotted-quad notation.

=item dest_ip

The destination IP address for this packet in dotted-quad notation.

=item options

Any IP options for this packet.

=item data

The encapsulated data (payload) for this IP packet.

=back

=head2 Exports

=over

=item default

none

=item exportable

IP_PROTO_IP IP_PROTO_ICMP IP_PROTO_IGMP IP_PROTO_IPIP IP_PROTO_TCP
IP_PROTO_UDP IP_VERSION_IPv4

=item tags

The following tags group together related exportable items.

=over

=item C<:protos>

IP_PROTO_IP IP_PROTO_ICMP IP_PROTO_IGMP IP_PROTO_IPIP
IP_PROTO_TCP IP_PROTO_UDP

=item C<:versions>

IP_VERSION_IPv4

=item C<:strip>

Import the strip function C<ip_strip>.

=item C<:ALL>

All the above exportable items.

=back

=back

=head1 EXAMPLE

The following script dumps IP frames by IP address and protocol
to standard output.

  #!/usr/bin/perl -w

  use strict;
  use Net::PcapUtils;
  use NetPacket::Ethernet qw(:strip);
  use NetPacket::IP;

  sub process_pkt {
      my ($user, $hdr, $pkt) = @_;

      my $ip_obj = NetPacket::IP->decode(eth_strip($pkt));
      print("$ip_obj->[SRC_IP]:$ip_obj->[DEST_IP] $ip_obj->[PROTO]\n");
  }

  Net::PcapUtils::loop(\&process_pkt, FILTER => 'ip');

=head1 TODO

=over

=item IP option decoding - currently stored in binary form.

=item Assembly of received fragments

=back

=head1 COPYRIGHT

  Copyright (c) 2001 Tim Potter.

  This package is free software and is provided "as is" without express 
  or implied warranty.  It may be used, redistributed and/or modified 
  under the terms of the Perl Artistic License (see
  http://www.perl.com/perl/misc/Artistic.html)

  Copyright (c) 1995,1996,1997,1998,1999 ANU and CSIRO on behalf of 
  the participants in the CRC for Advanced Computational Systems
  ('ACSys').

  ACSys makes this software and all associated data and documentation
  ('Software') available free of charge.  You may make copies of the 
  Software but you must include all of this notice on any copy.

  The Software was developed for research purposes and ACSys does not
  warrant that it is error free or fit for any purpose.  ACSys
  disclaims any liability for all claims, expenses, losses, damages
  and costs any user may incur as a result of using, copying or
  modifying the Software.

=head1 AUTHOR

Tim Potter E<lt>tpot@samba.orgE<gt>

Stephanie Wehner E<lt>atrak@itsx.comE<gt>

=cut

# any real autoloaded methods go after this line
