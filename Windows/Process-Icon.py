import http.client
import io
import ipaddress
import os
import socket
import sys
from urllib.parse import urljoin, urlsplit

from PIL import Image, ImageOps


MAX_BYTES = 5 * 1024 * 1024
Image.MAX_IMAGE_PIXELS = 16_000_000


def download_icon(url):
    for _ in range(4):
        parsed = urlsplit(url)
        if parsed.scheme not in ('https', 'http') or not parsed.hostname or parsed.username is not None:
            raise ValueError('Expected a public HTTP(S) image URL')
        port = parsed.port or (443 if parsed.scheme == 'https' else 80)
        if port not in (80, 443):
            raise ValueError('Unsupported image URL port')
        addresses = socket.getaddrinfo(parsed.hostname, port, type=socket.SOCK_STREAM)
        if not addresses or any(not ipaddress.ip_address(addr[4][0]).is_global for addr in addresses):
            raise ValueError('Image URL must resolve to a public address')

        connection_type = http.client.HTTPSConnection if parsed.scheme == 'https' else http.client.HTTPConnection
        connection = connection_type(parsed.hostname, port, timeout=10)
        # Keep the original hostname for TLS and HTTP. Connect to a validated address.
        address = addresses[0][4][0]
        connection._create_connection = lambda _, timeout, source: socket.create_connection((address, port), timeout, source)
        path = parsed.path or '/'
        if parsed.query:
            path += '?' + parsed.query
        try:
            connection.request('GET', path, headers={'User-Agent': 'Mozilla/5.0'})
            with connection.getresponse() as response:
                location = response.getheader('Location')
                if response.status in (301, 302, 303, 307, 308) and location:
                    url = urljoin(url, location)
                    continue
                if response.status != 200:
                    raise ValueError('Image server returned an error')
                contents = response.read(MAX_BYTES + 1)
                if len(contents) > MAX_BYTES:
                    raise ValueError('Image exceeds size limit')
                return io.BytesIO(contents)
        finally:
            connection.close()
    raise ValueError('Too many redirects')


def save_icon(source, destination):
    with Image.open(source, formats=['PNG', 'JPEG', 'ICO']) as original:
        image = ImageOps.exif_transpose(original).convert('RGBA')
    image = ImageOps.contain(image, (256, 256), Image.Resampling.LANCZOS)
    canvas = Image.new('RGBA', (256, 256))
    canvas.paste(image, ((256 - image.width) // 2, (256 - image.height) // 2))
    canvas.save(destination, format='ICO', sizes=[(size, size) for size in (16, 32, 48, 64, 128, 256)])


if __name__ == '__main__':
    icon_url = os.environ.get('PAKR_ICON_URL', '').strip()
    if icon_url:
        try:
            save_icon(download_icon(icon_url), sys.argv[1])
            print('Custom Windows icon ready')
            sys.exit(0)
        except (OSError, ValueError, http.client.HTTPException, Image.DecompressionBombError):
            print('Icon download or decode failed. Using the default html2.link logo.')
    save_icon('html2link-logo.png', sys.argv[1])
