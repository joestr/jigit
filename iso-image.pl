#!/usr/bin/perl -w
#
# iso-image.pl
#
# (C) 2004 Steve McIntyre <steve@einval.com>
#
# CGI wrapper to work with mkimage
#
# Parse the parameters and call mkimage as appropriate, paying 
# particular attention to byte ranges
#
# GPL v2
#
# v 0.1

use strict;
use File::Basename;
use Socket;

# Configure these for your system
my $mkimage = "/usr/local/bin/jigit-mkimage";
my $logfile = "/var/log/jigdo-logs/log.$$";
my $template_dir = "/mirror/jigdo";
my $matches = "-q -m Debian=/mirror/debian -m Non-US=/mirror/debian-non-US";

my $size;
my $image_name;
open LOGFILE, ">> $logfile" || die "Unable to open logfile!\n";

# Log an error to the user
sub user_error ($) {
    my $my_name = "iso_image.pl";
    print "Status: 400 Invalid Request\n";
    print "Content-type: text/html\n\n";
    print "$my_name: @_<p>Abort.\n";
}

# Log a message to the logfile and stop
sub log_die ($) {
    print LOGFILE @_;
    die @_;
}

# Convert the .iso filename into a .jigdo
sub jigdo_name ($) {
    my ($jigdo_name) = @_;
    $jigdo_name =~ s/\.iso$/\.jigdo/g;
    $jigdo_name = $template_dir . "/" . $jigdo_name;
    return $jigdo_name;
}

# Convert the .iso filename into a .template
sub template_name ($) {
    my ($template_name) = @_;
    $template_name =~ s/\.iso$/\.template/g;
    $template_name = $template_dir . "/" . $template_name;
    return $template_name;
}

# Grab the image size out of the template file
sub image_size ($) {
    my ($image_name) = @_;
    my $image_size;
    my $cmdline = "$mkimage -l " . $logfile . " -z -t " . template_name($image_name);

    open (FH, '-|', $cmdline);
    $image_size = <FH>;
    close FH;
    return $image_size;
}

# We have no range headers; simply generate the full image
sub produce_full_image ($$) {
    my ($image_name, $size) = @_;
    my $output_name = basename($image_name);
    my $cmdline;
    my $err = 0;

    $cmdline = "$mkimage -l " . $logfile;
    $cmdline = $cmdline . " -t " . template_name($image_name);
    $cmdline = $cmdline . " -j " . jigdo_name($image_name);    
    $cmdline = $cmdline . " " . $matches;

    print "Status: 200 OK\n";
    print "Content-Type: application/octet-stream\n";
    print "Content-Disposition: inline;filename=$output_name\n";
    print "Content-Length: $size\n\n";

    $err = system($cmdline) >> 8;
    if ($err) {
        log_die ("Failed to rebuild image; error $err\n");
    }
}

# Generate the desired range
sub produce_range ($$$) {
    my ($image_name, $start, $end) = @_;
    my $output_name = basename($image_name);
    my $cmdline;
    my $err = 0;

    $cmdline = "$mkimage -l " . $logfile;
    $cmdline = $cmdline . " -t " . template_name($image_name);
    $cmdline = $cmdline . " -j " . jigdo_name($image_name);    
    $cmdline = $cmdline . " -s " . $start;
    $cmdline = $cmdline . " -e " . $end;
    $cmdline = $cmdline . " " . $matches;

#    print "X-output: cmdline $cmdline\n";
    $err = system($cmdline) >> 8;
    if ($err) {
        log_die ("Failed to rebuild image; error $err\n");
    }
}

# Calculate start, end and length from the supplied range
sub parse_range ($$) {
    my ($range, $size) = @_;
    my @offsets;
    my $content_length = 0;

    if (length($range) == 1 && $range =~ m/-/g ) {
        $offsets[0] = 0;
        $offsets[1] = $size - 1;
    } else {
        @offsets = split(/-/, $range, 2);
    }

    if (!defined($offsets[0]) || !length($offsets[0])) {
        $offsets[0] = -1;
    }
    if (!defined($offsets[1]) || !length($offsets[1])) {
        $offsets[1] = $size - 1;
    }
    if ($offsets[0] == -1) {
        $offsets[0] = $size - $offsets[1];
        $offsets[1] = $size - 1;
    }

    # Make sure we have been given numbers
    $offsets[0] = int($offsets[0]);
    $offsets[1] = int($offsets[1]);

    # Check they're valid
    if ($offsets[0] < 0 || $offsets[0] >= $size) {
        print "Range start $offsets[0] invalid!\n";
        log_die "Range start $offsets[0] invalid!\n";
    }
    if ($offsets[1] < 0 || $offsets[1] >= $size) {
        print "Range end $offsets[1] invalid!\n";
        log_die "Range end $offsets[1] invalid!\n";
    }
    if ($offsets[0] > $offsets[1]) {
        print "Range start $offsets[0] after end $offsets[1]!\n";
        log_die "Range start $offsets[0] after end $offsets[1]!\n";
    }
    $content_length = $offsets[1] + 1 - $offsets[0];
    return ($offsets[0], $offsets[1], $content_length);
}

# We've been asked for ranges. Calculate which ones, then start generating them
sub produce_ranges ($$$) {
    my ($image_name, $size, $ranges) = @_;
    my $output_name = basename($image_name);
    my $cmdline;
    my $err = 0;
    my @range_array;
    my ($start, $end, $range, $length);
    
    chomp $ranges;
    $ranges =~ s/^.*\=//g;
    @range_array = split (/,/, $ranges);
    if (scalar(@range_array) == 1) {
        ($start, $end, $length) = parse_range($range_array[0], $size);
        print "Status: 206 Partial content\n";
        print "Content-Type: application/octet-stream; filename=$output_name\n";
        print "Content-Range: bytes $start-$end/$size\n";
        print "Content-Length: $length\n\n";
        $err = produce_range($image_name, $start, $end);
    } else {
        print "Status: 206 Partial content\n";
        print "Content-Type: multipart/byteranges; boundary=THIS_STRING_SEPARATES\n\n";
        for my $range (@range_array) {
            ($start, $end, $length) = parse_range($range, $size);
            print "--THIS_STRING_SEPARATES\n";
            print "Content-Type: application/octet-stream; filename=$output_name\n";
            print "Content-Range: bytes $start-$end/$size\n\n";
            $err = produce_range($image_name, $start, $end);
        }
        print "--THIS_STRING_SEPARATES\n";
    }
}

################################################################################
#
# All starts here
#

my $remote_host = $ENV{'REMOTE_HOST'};
my $remote_addr = $ENV{'REMOTE_ADDR'};
my $ultimate = $ENV{'HTTP_X_FORWARDED_FOR'};
my $iaddr;
my $ult_name;

# Sanity checking
if (!defined($ARGV[0])) {
    user_error("You must specify an image name to download.");
    log_die "No file specified...!\n";
}

if (!defined($remote_host) || !length($remote_host)) {
    $iaddr = inet_aton($remote_addr);
    $remote_host = gethostbyaddr($iaddr, AF_INET);
}

print LOGFILE "Connection made from $remote_addr ($remote_host)\n";
if (defined($ultimate) && length($ultimate)) {
    $iaddr = inet_aton($ultimate);
    $ult_name = gethostbyaddr($iaddr, AF_INET);
    print LOGFILE "Proxy for:         $ultimate ($ult_name)\n";
}

print LOGFILE "Asking for $ENV{SCRIPT_NAME}/$ENV{QUERY_STRING}\n";
print LOGFILE "Generating $ARGV[0]:\n";
print LOGFILE scalar localtime;
print LOGFILE "\n\n\n";

$image_name = $ARGV[0];

if (! -f template_name($image_name)) {
    user_error("No template file found to match image name \"$image_name\"");
    log_die ("Couldn't find template file for image $image_name\n");
}

if (! -f jigdo_name($image_name)) {
    user_error("No jigdo file file found to match image name \"$image_name\"");
    log_die ("Couldn't find jigdo file for image $image_name\n");
}

$size = image_size($image_name);
chomp $size;

if (defined($ENV{HTTP_RANGE})) {
	# We have range(s) specified. Parse what we've been given and call
	# mkimage for each range
    produce_ranges($image_name, $size, $ENV{HTTP_RANGE});
} else {
	# If we don't have a Range: header, simply return the whole image
    produce_full_image($image_name, $size);
}

print LOGFILE "Done\n";
print LOGFILE scalar localtime;
print LOGFILE "\n\n\n";
exit 0;
