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

    # export mesh geometry from Shape.dat for the 3D mesh viewer
    shape_dat = os.path.join(game, "Data", "Shape.dat")
    if os.path.isfile(shape_dat):
        subprocess.call([sys.executable, "-I",
                         os.path.join(HERE, "tools", "export_shapes.py"),
                         shape_dat, os.path.join(WEB, "shapes")])

    # export bind-pose skeletons from Skeleton.dat for the skeleton viewer
    skel_dat = os.path.join(game, "Data", "Skeleton.dat")
    if os.path.isfile(skel_dat):
        subprocess.call([sys.executable, "-I",
                         os.path.join(HERE, "tools", "parse_skeleton.py"),
                         skel_dat, os.path.join(WEB, "skeletons")])

    # export recorded match replays (AutoPlay/*.ape) for the replay viewer
    ap_dir = os.path.join(game, "AutoPlay")
    if os.path.isdir(ap_dir):
        import json
        rep_dir = os.path.join(WEB, "replays")
        os.makedirs(rep_dir, exist_ok=True)
        rindex = []
        for fn in sorted(os.listdir(ap_dir)):
            if not fn.lower().endswith(".ape"):
                continue
            base = os.path.splitext(fn)[0]
            rc = subprocess.call([sys.executable, "-I",
                                  os.path.join(HERE, "tools", "parse_ape.py"),
                                  os.path.join(ap_dir, fn),
                                  os.path.join(rep_dir, base + ".json")],
                                 stdout=subprocess.DEVNULL)
            if rc == 0:
                rindex.append({"name": base, "file": base + ".json"})
        if rindex:
            with open(os.path.join(rep_dir, "index.json"), "w", encoding="utf-8") as f:
                json.dump(rindex, f)
            print(f"replays  : {len(rindex)} recorded matches -> web/replays/")

    # decode textures (PTX -> PNG) for the gallery
    if os.path.isfile(os.path.join(game, "AXIS.EXE")):
        subprocess.call([sys.executable, "-I",
                         os.path.join(HERE, "tools", "export_textures.py"),
                         game, os.path.join(WEB, "textures")],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # serve web/ normally, plus /game/<path> from the user's install (read-only, local)
    # so the browser can decode textures/meshes live from their own game data.
    class Handler(http.server.SimpleHTTPRequestHandler):
        def translate_path(self, path):
            p = path.split("?", 1)[0].split("#", 1)[0]
            if p.startswith("/game/"):
                rel = p[len("/game/"):].lstrip("/")
                parts = [x for x in rel.split("/") if x not in ("", ".", "..")]
                return os.path.join(game, *parts)
            return super().translate_path(path)

        def log_message(self, *a):
            pass

        def __init__(self, *a, **k):
            super().__init__(*a, directory=WEB, **k)

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", port), Handler) as httpd:
        url = f"http://127.0.0.1:{port}/home.html"
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
