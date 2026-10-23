#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 INPUT_HTML

Attach header, footer, and ad placeholder content to an HTML document.
The script overwrites the input file.
EOF
}

if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

input_file="$1"

if [[ ! -f "$input_file" ]]; then
    echo "Error: input file not found: $input_file" >&2
    exit 2
fi

header_file=""
footer_file=""

if [[ -f "header.html" ]]; then
    header_file="header.html"
elif [[ -f "Body/Header.html" ]]; then
    header_file="Body/Header.html"
fi

if [[ -f "footer.html" ]]; then
    footer_file="footer.html"
elif [[ -f "Body/Footer.html" ]]; then
    footer_file="Body/Footer.html"
fi

perl -0777 -CS -e '
use strict;
use warnings;

my ($header_file, $footer_file, $input) = @ARGV;

sub slurp_file {
    my ($path) = @_;
    return "" unless $path && -f $path;
    open my $fh, "<:encoding(UTF-8)", $path or die "Unable to read $path: $!";
    local $/;
    my $content = <$fh>;
    close $fh;
    return $content;
}

my $header = slurp_file($header_file);
my $footer = slurp_file($footer_file);
my $ad_placeholder = <<'AD';
<div id="ad-panel" class="ad-panel">
    <div id="ad-content" style="padding: 0 20px;">Loading ad...</div>
</div>
AD

open my $in, "<:encoding(UTF-8)", $input or die "Unable to read $input: $!";
local $/;
my $html = <$in>;
close $in;

if ($header ne "") {
    if ($html =~ s#<div\s+id="header"\s*>\s*</div>#$header#si) {
        # replaced placeholder
    } elsif ($html =~ s#(<body[^>]*>)#$1\n$header#si) {
        # inserted after body tag
    }
}

if ($footer ne "") {
    if ($html =~ s#<div\s+id="footer"\s*>\s*</div>#$footer#si) {
        # replaced placeholder
    } elsif ($html =~ s#(</body>)#\n$footer\n$1#si) {
        # inserted before closing body
    }
}

if ($html !~ /<div\s+id="ad-panel"/i) {
    if ($html =~ s#(<main[^>]*>)#$1\n$ad_placeholder#si) {
        # inserted after main open tag
    } elsif ($html =~ s#(</body>)#\n$ad_placeholder\n$1#si) {
        # inserted before closing body
    }
}

open my $out, ">:encoding(UTF-8)", $input or die "Unable to write $input: $!";
print $out $html;
close $out;
' "$header_file" "$footer_file" "$input_file"

echo "Updated: $input_file"
