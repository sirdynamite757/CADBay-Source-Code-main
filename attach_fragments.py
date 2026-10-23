#!/usr/bin/env python3
import argparse
import os
import re
import sys

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
HEADER_FILES = ["header.html", os.path.join("Body", "Header.html")]
FOOTER_FILES = ["footer.html", os.path.join("Body", "Footer.html")]

AD_PLACEHOLDER = '''
<div id="ad-panel" class="ad-panel">
    <div class="ad-placeholder">
        <strong>Sponsored Content</strong>
        <p>This ad space is reserved for CADBay promotions and featured content.</p>
    </div>
</div>
'''

HEADER_INSERTION = '<div id="header"></div>'
FOOTER_INSERTION = '<div id="footer"></div>'
AD_INSERTION = '<div id="ad-panel"></div>'


def read_first_available(paths):
    for path in paths:
        full_path = os.path.join(BASE_DIR, path)
        if os.path.isfile(full_path):
            with open(full_path, 'r', encoding='utf-8') as handle:
                return handle.read()
    return None


def insert_fragment(html, selector, fragment, fallback_pattern, insert_after=True):
    if selector in html:
        return html.replace(selector, fragment)

    match = re.search(fallback_pattern, html, flags=re.IGNORECASE | re.DOTALL)
    if not match:
        return html

    if insert_after:
        return html[: match.end()] + "\n" + fragment + html[match.end() :]
    return html[: match.start()] + fragment + "\n" + html[match.start() :]


def create_ad_content(html):
    if AD_INSERTION in html:
        return html.replace(AD_INSERTION, AD_PLACEHOLDER)

    # insert after opening <main> if present, otherwise before closing </body>
    main_match = re.search(r"<main[^>]*>", html, flags=re.IGNORECASE)
    if main_match:
        return html[: main_match.end()] + "\n" + AD_PLACEHOLDER + html[main_match.end() :]

    body_close = re.search(r"</body>", html, flags=re.IGNORECASE)
    if body_close:
        return html[: body_close.start()] + "\n" + AD_PLACEHOLDER + html[body_close.start() :]

    return html


def main():
    parser = argparse.ArgumentParser(description="Attach CADBay header, footer and ad placeholder to an HTML file.")
    parser.add_argument('input', help='Input HTML file')
    parser.add_argument('-o', '--output', help='Output HTML file. Defaults to <input>.assembled.html')
    parser.add_argument('--inline', action='store_true', help='Overwrite the input file instead of writing a new one')
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f'Error: input file does not exist: {args.input}', file=sys.stderr)
        sys.exit(1)

    header = read_first_available(HEADER_FILES)
    footer = read_first_available(FOOTER_FILES)

    if not header:
        print('Warning: no header.html found in the repository root or Body/', file=sys.stderr)
        header = ''
    if not footer:
        print('Warning: no footer.html found in the repository root or Body/', file=sys.stderr)
        footer = ''

    with open(args.input, 'r', encoding='utf-8') as handle:
        html = handle.read()

    html = insert_fragment(html, HEADER_INSERTION, header, r"<body[^>]*>", insert_after=True)
    html = insert_fragment(html, FOOTER_INSERTION, footer, r"</body>", insert_after=False)
    html = create_ad_content(html)

    output_file = args.input if args.inline else args.output or f"{os.path.splitext(args.input)[0]}.assembled.html"
    with open(output_file, 'w', encoding='utf-8') as handle:
        handle.write(html)

    print(f'Generated: {output_file}')


if __name__ == '__main__':
    main()
