#!/usr/bin/env python3
"""Upload via the existing Schwung Manager custom-module form."""
import argparse
import http.cookiejar
import io
import pathlib
import tarfile
import urllib.parse
import urllib.request
import uuid

def install(base, package, check=False):
    parsed = urllib.parse.urlparse(base)
    if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.path not in ("", "/"):
        raise ValueError("Use a Manager base URL, e.g. http://move.local:7700")
    base = base.rstrip("/")
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                        urllib.request.HTTPCookieProcessor(jar))
    with opener.open(base + "/modules", timeout=10) as response:
        response.read()
    token = next((cookie.value for cookie in jar if cookie.name == "csrf_token"), None)
    if not token:
        raise RuntimeError("Manager did not supply its csrf_token cookie")
    if check:
        print("Connected to Schwung Manager:", base)
        return
    payload = package.read_bytes()
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
        for member in archive.getmembers():
            path = pathlib.PurePosixPath(member.name)
            if not path.parts or path.parts[0] != "o303" or ".." in path.parts or member.issym() or member.islnk():
                raise ValueError("Package must contain only the o303/ directory")
        dsp = archive.extractfile("o303/dsp.so").read()
        if dsp[:6] != b"\x7fELF\x02\x01" or int.from_bytes(dsp[18:20], "little") != 183:
            raise ValueError("Install requires ARM64 dsp.so, never the desktop build")
    boundary = "o303-" + uuid.uuid4().hex
    parts = []
    for name, value in [("csrf_token", token), ("source", "tarball")]:
        parts.append(("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" +
                      name + "\"\r\n\r\n" + value + "\r\n").encode())
    parts.append(("--" + boundary + '\r\nContent-Disposition: form-data; name="file"; ' +
                  'filename="o303-module.tar.gz"\r\nContent-Type: application/gzip\r\n\r\n').encode())
    parts.extend([payload, ("\r\n--" + boundary + "--\r\n").encode()])
    request = urllib.request.Request(base + "/modules/install-custom", data=b"".join(parts),
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary, "X-CSRF-Token": token})
    with opener.open(request, timeout=60) as response:
        response.read()
        flash = urllib.parse.parse_qs(urllib.parse.urlparse(response.url).query).get("flash", [""])[0]
    # Newer Managers append advice ("- restart Move to run the new version ..."),
    # so the confirmation is the prefix.
    if not flash.startswith("Installed o303 from tarball"):
        raise RuntimeError("Manager did not confirm installation: " + flash)
    print(flash)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", nargs="?", default="http://move.local:7700")
    parser.add_argument("--check", action="store_true", help="Check connection only; upload nothing")
    args = parser.parse_args()
    try:
        install(args.url, pathlib.Path(__file__).resolve().parent.parent / "dist/o303-module.tar.gz", args.check)
    except Exception as exc:
        parser.exit(1, str(exc) + "\n")
