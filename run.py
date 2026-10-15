#!/usr/bin/env python3
"""One-shot local runner for the AXIS web explorer.

    python run.py                # auto-detects the game dir (../Axis_Win)
    python run.py <game_dir>     # or point it at your AXIS install
    python run.py <game_dir> -p 8123

Builds web/catalog.json from your installed game, serves web/ on localhost,
and opens the browser. Nothing is uploaded; the catalog holds metadata only.
"""
import sys
import os
import subprocess
import webbrowser
import http.server
import socketserver

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, "web")


def find_game(argv):
    for a in argv:
        if not a.startswith("-") and os.path.isdir(a):
            return os.path.abspath(a)
    # default guesses, relative to this repo
    for cand in (os.path.join(HERE, "..", "Axis_Win"),
                 os.path.join(HERE, "..", "axis", "Axis_Win"),
                 os.path.join(HERE, "game")):
        if os.path.isdir(os.path.join(cand, "Data")):
            return os.path.abspath(cand)
    return None


def main():
    port = 8123
    if "-p" in sys.argv:
        port = int(sys.argv[sys.argv.index("-p") + 1])

    game = find_game(sys.argv[1:])
    if not game:
        print("Could not find your AXIS install. Pass it explicitly:\n"
              "    python run.py /path/to/Axis_Win")
        return 1
    print(f"game dir : {game}")

    out_json = os.path.join(WEB, "catalog.json")
    rc = subprocess.call([sys.executable, "-I",
                          os.path.join(HERE, "tools", "build_catalog.py"),
                          game, out_json])
    if rc != 0:
        return rc

    # extract every map's object layout for the 3D viewer
    maps_dir = os.path.join(WEB, "maps")
    os.makedirs(maps_dir, exist_ok=True)
    src_maps = os.path.join(game, "Map")
    index = []
    if os.path.isdir(src_maps):
        import json
        for fn in sorted(os.listdir(src_maps)):
            if not fn.lower().endswith(".bfd"):
                continue
            name = os.path.splitext(fn)[0]
            subprocess.call([sys.executable, "-I",
                             os.path.join(HERE, "tools", "extract_map.py"),
                             os.path.join(src_maps, fn),
                             os.path.join(maps_dir, name + ".json")],
                            stdout=subprocess.DEVNULL)
            index.append(name)
        with open(os.path.join(maps_dir, "index.json"), "w", encoding="utf-8") as f:
            json.dump(index, f)
        print(f"maps     : {len(index)} map layouts -> web/maps/")

    os.chdir(WEB)
    handler = http.server.SimpleHTTPRequestHandler
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", port), handler) as httpd:
        url = f"http://127.0.0.1:{port}/"
        print(f"serving  : {url}  (Ctrl+C to stop)")
        try:
            webbrowser.open(url)
        except Exception:
            pass
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
