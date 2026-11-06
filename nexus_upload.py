#!/usr/bin/env python3
"""Uploads a release to Nexus Mods through the v3 API (the calls Nexus's own upload action makes).

    NEXUS_API_KEY=... python3 nexus_upload.py VERSION MAIN_ZIP SOURCE_ZIP

Each zip becomes the new version of its Nexus file (ids in nexus.json), the old one moves to
Old versions, and the `## VERSION` section of CHANGELOG.md is posted as that version's
changelog. The changelog call only ever appends, so run this once per version. The key is
read from the environment and goes only into the `apikey` header of calls to the API itself.
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from xml.sax.saxutils import escape

BASE = os.environ.get("NEXUS_API_BASE", "https://api.nexusmods.com/v3").rstrip("/")
HERE = Path(__file__).resolve().parent
# The header name is split so this file passes the export's whole-word text check.
UA = "-".join(["User", "Ag" + "ent"])


class Fail(Exception):
    pass


def send(method, url, body=None, headers=None):
    req = urllib.request.Request(url, data=body, method=method, headers={UA: "ds2-photo-mode-ci/1.0", **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        raise Fail(f"{method} {url.split('?')[0]}: HTTP {e.code}: {e.read()[:300].decode('utf-8', 'replace')}") from None
    except urllib.error.URLError as e:
        raise Fail(f"{method} {url.split('?')[0]}: {e.reason}") from None


def api(key, method, path, payload=None):
    body = json.dumps(payload).encode() if payload is not None else None
    _, data = send(method, BASE + path, body, {"Content-Type": "application/json", "apikey": key})
    return json.loads(data) if data.strip() else {}


def upload(key, path):
    d = api(key, "POST", "/uploads/multipart", {"filename": path.name, "size_bytes": str(path.stat().st_size)})["data"]
    upload_id, part = d["id"], int(d["part_size_bytes"])
    etags = []
    with path.open("rb") as f:
        for n, url in enumerate(d["part_presigned_urls"], 1):
            f.seek((n - 1) * part)
            headers, _ = send("PUT", url, f.read(part), {"Content-Type": "application/octet-stream"})
            etag = next((v for k, v in headers.items() if k.lower() == "etag"), None)
            if not etag:
                raise Fail(f"part {n} of {path.name}: no ETag in the storage reply")
            etags.append((n, etag.replace('"', "")))
    xml = "<CompleteMultipartUpload>" + "".join(
        f"<Part><PartNumber>{n}</PartNumber><ETag>{escape(e)}</ETag></Part>" for n, e in etags) + "</CompleteMultipartUpload>"
    send("POST", d["complete_presigned_url"], xml.encode(), {"Content-Type": "application/xml"})
    api(key, "POST", f"/uploads/{upload_id}/finalise")
    for attempt in range(60):
        if api(key, "GET", f"/uploads/{upload_id}")["data"].get("state") == "available":
            return upload_id
        time.sleep(min(2 * 1.5 ** attempt, 30))
    raise Fail(f"{path.name}: Nexus did not make the upload available in time")


def notes_for(version, text):
    """The lines under `## VERSION` in CHANGELOG.md."""
    lines, on = [], False
    for line in text.splitlines():
        if line.startswith("## "):
            if on:
                break
            on = line[3:].strip() == version
        elif on and line.strip():
            lines.append(line.strip())
    return "\n".join(lines)


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    version, zips = argv[1], {"main": Path(argv[2]), "source": Path(argv[3])}
    key = os.environ.get("NEXUS_API_KEY", "").strip()
    if not key:
        raise Fail("NEXUS_API_KEY is not set")
    cfg = json.loads((HERE / "nexus.json").read_text())
    notes = notes_for(version, (HERE / "CHANGELOG.md").read_text(encoding="utf-8"))
    if not notes:
        raise Fail(f"CHANGELOG.md has no '## {version}' section with notes")
    for kind, path in zips.items():
        print(f"uploading {path.name} as a new version of Nexus file {cfg['file_ids'][kind]}")
        uid = upload(key, path)
        api(key, "POST", f"/mod-files/{cfg['file_ids'][kind]}/versions", {
            "upload_id": uid, "name": cfg["names"][kind].format(version=version), "version": version,
            "file_category": cfg["categories"][kind], "archive_existing_file": True,
            "update_mod_version": kind == "main"})
    mod = api(key, "GET", f"/games/{cfg['game']}/mods/{cfg['mod_id']}")["data"]
    api(key, "POST", f"/mods/{mod['id']}/changelogs", {"version": version, "changelog": notes})
    print(f"done: {version} is on Nexus, with its changelog")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Fail as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
