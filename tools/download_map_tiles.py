#!/usr/bin/env python3
"""Downloads web map tiles for an area into the layout the watch's Map app reads.

Copy the resulting `maps` folder to the root of the SD card, so tiles end up at
/maps/<zoom>/<x>/<y>.png (or .jpg). Tiles must be 256 px, PNG or JPEG.

Examples:
    # 10 km around a point, zoom 10-16, from the public-domain USGS Topo map (US only, no key; note
    # this server puts {y} before {x}):
    python3 tools/download_map_tiles.py --center 39.7392,-104.9903 --radius-km 10 --zoom 10-16 \\
        --url 'https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}'

    # A bounding box (west,south,east,north):
    python3 tools/download_map_tiles.py --bbox -105.05,39.70,-104.95,39.78 --zoom 12-15 --url '...'

Respect the tile provider's terms. Most commercial providers forbid bulk downloading on their free
plans (MapTiler Cloud outright; Thunderforest below its Small Business plan), and the OpenStreetMap
Foundation's servers (tile.openstreetmap.org) forbid it and block tools like this one (they return
an "Access blocked" image instead of the map), so this script refuses to use them. Use a source
that allows offline use, such as USGS Topo, or tiles you rendered yourself.
"""
import argparse
import math
import os
import sys
import time
import urllib.error
import urllib.request

OSM_HOST = "tile.openstreetmap.org"
USER_AGENT = "WatchApp-map-tiles/1.0 (T-Watch Ultra offline map)"


def tile_xy(lat, lon, z):
    lat = max(-85.05112878, min(85.05112878, lat))
    n = 2 ** z
    x = (lon + 180) / 360 * n
    s = math.sin(math.radians(lat))
    y = (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n
    return int(min(n - 1, max(0, x))), int(min(n - 1, max(0, y)))


def parse_zoom(text):
    lo, _, hi = text.partition("-")
    lo, hi = int(lo), int(hi or lo)
    if not 0 <= lo <= hi <= 19:
        raise argparse.ArgumentTypeError("zoom must be N or N-M within 0-19")
    return range(lo, hi + 1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    area = p.add_mutually_exclusive_group(required=True)
    area.add_argument("--bbox", help="west,south,east,north in degrees")
    area.add_argument("--center", help="lat,lon in degrees (use with --radius-km)")
    p.add_argument("--radius-km", type=float, default=3.0)
    p.add_argument("--zoom", type=parse_zoom, required=True, help="zoom level or range, e.g. 12-16")
    p.add_argument("--url", required=True, help="tile URL template with {z}, {x}, {y}")
    p.add_argument("--out", default="maps", help="output folder (default: maps)")
    p.add_argument("--delay", type=float, default=0.1, help="seconds between requests")
    p.add_argument("--yes", action="store_true", help="don't ask before downloading")
    args = p.parse_args()

    if args.bbox:
        west, south, east, north = (float(v) for v in args.bbox.split(","))
    else:
        lat, lon = (float(v) for v in args.center.split(","))
        dlat = args.radius_km / 111.32
        dlon = args.radius_km / (111.32 * max(0.01, math.cos(math.radians(lat))))
        west, south, east, north = lon - dlon, lat - dlat, lon + dlon, lat + dlat

    tiles = []
    for z in args.zoom:
        x0, y0 = tile_xy(north, west, z)
        x1, y1 = tile_xy(south, east, z)
        tiles += [(z, x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]

    if OSM_HOST in args.url:
        sys.exit(f"{OSM_HOST} forbids offline downloads and blocks this tool; use a provider that allows "
                 "offline use (see --help).")
    print(f"{len(tiles)} tiles for zoom {args.zoom.start}-{args.zoom.stop - 1}")
    if not args.yes and input("Download? [y/N] ").strip().lower() != "y":
        return

    fetched = skipped = failed = 0
    for i, (z, x, y) in enumerate(tiles, 1):
        url = args.url.format(z=z, x=x, y=y)
        existing = [e for e in ("png", "jpg") if os.path.exists(os.path.join(args.out, str(z), str(x), f"{y}.{e}"))]
        if existing:
            skipped += 1
            continue
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = resp.read()
        except urllib.error.URLError as e:
            failed += 1
            print(f"  {z}/{x}/{y}: {e}")
            continue
        if data[:8] == b"\x89PNG\r\n\x1a\n":
            ext = "png"
        elif data[:3] == b"\xff\xd8\xff":
            ext = "jpg"
        else:
            failed += 1
            print(f"  {z}/{x}/{y}: not a PNG or JPEG tile")
            continue
        folder = os.path.join(args.out, str(z), str(x))
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, f"{y}.{ext}"), "wb") as f:
            f.write(data)
        fetched += 1
        print(f"\r  {i}/{len(tiles)}", end="", flush=True)
        time.sleep(args.delay)
    print(f"\nDone: {fetched} downloaded, {skipped} already there, {failed} failed. "
          f"Copy '{args.out}' to the SD card root as /maps.")


if __name__ == "__main__":
    main()
