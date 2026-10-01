#!/usr/bin/env python3
"""Validate the upstream radio playlist and optionally update the firmware catalog."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import re
import ssl
import subprocess
import tempfile
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Optional


PLAYLIST_URL = (
    "https://raw.githubusercontent.com/imDazui/Tvlist-awesome-m3u-m3u8/"
    "master/m3u/%E5%B9%BF%E6%92%AD%E7%94%B5%E5%8F%B0202506.m3u"
)
LOGO_TREE_URL = "https://api.github.com/repos/fanmingming/live/git/trees/main?recursive=1"
USER_AGENT = "ESP32-network-radio-catalog-validator/4.7"
BASE_STATION_COUNT = 113
SUPPORTED_AUDIO_TYPES = (
    "audio/", "application/ogg",
)


@dataclass
class ProbeResult:
  name: str
  url: str
  accepted: bool
  reason: str
  final_url: str = ""
  media_type: str = ""
  attempts: int = 0


def fetch(url: str, timeout: float, limit: int = 131072) -> tuple[bytes, str, str]:
  request = urllib.request.Request(
      url,
      headers={"User-Agent": USER_AGENT, "Accept": "*/*", "Icy-MetaData": "0"},
  )
  context = ssl.create_default_context()
  with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
    status = getattr(response, "status", 200)
    if status < 200 or status >= 300:
      raise ValueError(f"HTTP {status}")
    body = response.read(limit)
    return body, response.headers.get_content_type().lower(), response.geturl()


def fetch_with_retry(url: str, timeout: float, limit: int) -> tuple[bytes, str, str]:
  last_error: Optional[Exception] = None
  for attempt in range(3):
    try:
      return fetch(url, timeout, limit)
    except Exception as error:
      last_error = error
      time.sleep(0.5 * (attempt + 1))
  assert last_error is not None
  raise last_error


def fetch_logo(url: str) -> bytes:
  completed = subprocess.run(
      ["curl", "-L", "--fail", "--retry", "2", "--connect-timeout", "5",
       "--max-time", "20", "-sS", "-A", USER_AGENT, url],
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
      timeout=25,
      check=False,
  )
  if completed.returncode != 0:
    raise ValueError(completed.stderr.decode(errors="replace").strip()[:240])
  if len(completed.stdout) > 2 * 1024 * 1024:
    raise ValueError("logo exceeds 2 MiB")
  return completed.stdout


def looks_like_html(body: bytes) -> bool:
  sample = body[:1024].lstrip().lower()
  return sample.startswith((b"<!doctype html", b"<html", b"<?xml"))


def transport_stream_has_audio(body: bytes) -> bool:
  for offset in range(0, len(body) - 12):
    if body[offset] != 0x47:
      continue
    payload_start = (body[offset + 1] & 0x40) != 0
    adaptation = (body[offset + 3] >> 4) & 0x03
    if not payload_start or adaptation not in (1, 3):
      continue
    cursor = offset + 4
    if adaptation == 3:
      cursor += 1 + body[cursor]
    if cursor >= len(body):
      continue
    cursor += 1 + body[cursor]
    if cursor + 12 >= len(body) or body[cursor] != 0x02:
      continue
    section_length = ((body[cursor + 1] & 0x0F) << 8) | body[cursor + 2]
    section_end = min(len(body), cursor + 3 + section_length - 4)
    program_info_length = ((body[cursor + 10] & 0x0F) << 8) | body[cursor + 11]
    stream = cursor + 12 + program_info_length
    while stream + 5 <= section_end:
      stream_type = body[stream]
      if stream_type in (0x03, 0x04, 0x0F, 0x11, 0x81):
        return True
      info_length = ((body[stream + 3] & 0x0F) << 8) | body[stream + 4]
      stream += 5 + info_length
  return False


def looks_like_audio(body: bytes) -> bool:
  if body.startswith((b"ID3", b"OggS", b"fLaC", b"RIFF")):
    return True
  for offset in range(min(len(body) - 1, 4096)):
    first, second = body[offset], body[offset + 1]
    if first == 0xFF and (second & 0xE0) == 0xE0:
      return True
  return (len(body) > 376 and body[0] == 0x47 and body[188] == 0x47 and
          transport_stream_has_audio(body))


def playlist_lines(body: bytes) -> list[str]:
  text = body.decode("utf-8-sig", errors="replace")
  return [line.strip() for line in text.splitlines() if line.strip()]


def validate_hls(body: bytes, base_url: str, timeout: float) -> tuple[str, str]:
  lines = playlist_lines(body)
  if not lines or lines[0] != "#EXTM3U":
    raise ValueError("invalid HLS manifest")
  if any("#EXT-X-KEY:" in line and "METHOD=NONE" not in line for line in lines):
    raise ValueError("encrypted HLS is unsupported")

  candidates: list[tuple[int, str]] = []
  for index, line in enumerate(lines):
    if line.startswith("#EXT-X-STREAM-INF:") and index + 1 < len(lines):
      attributes = line.upper()
      if "CODECS=" in attributes and "MP4A" not in attributes and "MP3" not in attributes:
        continue
      match = re.search(r"BANDWIDTH=(\d+)", attributes)
      candidates.append((int(match.group(1)) if match else 0, lines[index + 1]))
  if candidates:
    _, child = min(candidates)
    child_url = urllib.parse.urljoin(base_url, child)
    child_body, _, child_final = fetch(child_url, timeout)
    return validate_hls(child_body, child_final, timeout)

  media = next((line for line in lines if not line.startswith("#")), "")
  if not media:
    raise ValueError("HLS has no media URI")
  media_url = urllib.parse.urljoin(base_url, media)
  if media_url.lower().split("?", 1)[0].endswith(".m3u8"):
    child_body, _, child_final = fetch(media_url, timeout)
    return validate_hls(child_body, child_final, timeout)
  segment, media_type, final_url = fetch(media_url, timeout, 65536)
  if len(segment) < 1024 or looks_like_html(segment) or not looks_like_audio(segment):
    raise ValueError("HLS media segment is empty or not media")
  return media_type or "hls-segment", final_url


def probe_once(name: str, url: str, timeout: float) -> tuple[str, str]:
  body, media_type, final_url = fetch(url, timeout)
  if looks_like_html(body):
    raise ValueError("response is HTML")
  if body.lstrip().startswith(b"#EXTM3U"):
    validate_hls(body, final_url, timeout)
    return "hls", final_url
  if len(body) < 16384:
    raise ValueError(f"direct stream returned only {len(body)} bytes")
  if not (media_type.startswith(SUPPORTED_AUDIO_TYPES) or looks_like_audio(body)):
    raise ValueError(f"unsupported content type {media_type or 'unknown'}")
  return media_type or "detected-audio", final_url


def probe(item: tuple[str, str], attempts: int, timeout: float) -> ProbeResult:
  name, url = item
  last_error = "unknown error"
  for attempt in range(1, attempts + 1):
    try:
      media_type, final_url = probe_once(name, url, timeout)
      return ProbeResult(name, url, True, "ok", final_url, media_type, attempt)
    except Exception as error:  # Each candidate must receive a recorded result.
      last_error = str(error).replace("\n", " ")[:240]
      if attempt < attempts:
        time.sleep(0.25 * attempt)
  return ProbeResult(name, url, False, last_error, attempts=attempts)


def read_m3u(text: str) -> list[tuple[str, str]]:
  entries: list[tuple[str, str]] = []
  name = ""
  for raw_line in text.lstrip("\ufeff").splitlines():
    line = raw_line.strip()
    if line.startswith("#EXTINF:"):
      name = line.split(",", 1)[-1].strip()
    elif line and not line.startswith("#"):
      entries.append((clean_name(name), line))
      name = ""
  return entries


def clean_name(name: str) -> str:
  name = unicodedata.normalize("NFKC", name).replace("沉阳", "沈阳")
  name = re.sub(r"\s+", " ", name).strip()
  name = re.sub(r"\s*\(2\)$", "", name)
  encoded = name.encode("utf-8")
  while len(encoded) > 48:
    name = name[:-1].rstrip()
    encoded = name.encode("utf-8")
  return name


def name_key(name: str) -> str:
  value = unicodedata.normalize("NFKC", name).casefold()
  value = re.sub(r"[\s·•—–_()（）,.，-]+", "", value)
  return value


def source_score(url: str) -> int:
  parsed = urllib.parse.urlparse(url)
  host = parsed.hostname or ""
  score = 3 if parsed.scheme == "https" else 0
  if any(part in host for part in ("cnr.cn", "cri.cn", "rthk", "akamaized.net")):
    score += 5
  if parsed.path.lower().endswith(".m3u8"):
    score += 1
  if any(part in host for part in ("xmcdn.com", "qingting.fm")):
    score -= 1
  return score


def c_quote(value: str) -> str:
  return value.replace("\\", "\\\\").replace('"', '\\"')


def parse_existing(sketch: str) -> tuple[list[dict[str, str]], dict[str, str]]:
  station_section = sketch.split("constexpr BuiltinStation kBuiltinStations[] = {", 1)[1].split("};", 1)[0]
  icon_section = sketch.split("constexpr IconStation kIconStations[] = {", 1)[1].split("};", 1)[0]
  stations = [
      {"name": name, "url": url, "logo": ""}
      for name, url in re.findall(r'\{"([^"]+)","([^"]+)"\}', station_section)
  ]
  icons = {
      url: logo
      for _, url, logo in re.findall(r'\{"([^"]+)","([^"]+)","([^"]+)"\}', icon_section)
  }
  for station in stations:
    station["logo"] = icons.get(station["url"], "")
  return stations, icons


def logo_catalog() -> dict[str, tuple[str, str]]:
  raw, _, _ = fetch_with_retry(LOGO_TREE_URL, 30, 4 * 1024 * 1024)
  tree = json.loads(raw)
  candidates: dict[str, list[tuple[str, str]]] = {}
  for entry in tree.get("tree", []):
    path = entry.get("path", "")
    if entry.get("type") != "blob" or not path.startswith("radio/"):
      continue
    filename = path.split("/", 1)[1]
    stem, extension = Path(filename).stem, Path(filename).suffix.lower()
    if extension not in (".png", ".jpg", ".jpeg", ".webp"):
      continue
    key = name_key(stem)
    raw_url = "https://raw.githubusercontent.com/fanmingming/live/main/" + urllib.parse.quote(path)
    candidates.setdefault(key, []).append((filename, raw_url))
  return {key: values[0] for key, values in candidates.items() if len(values) == 1}


def image_extension(data: bytes) -> str:
  if data.startswith(b"\x89PNG\r\n\x1a\n"):
    return ".png"
  if data.startswith(b"\xff\xd8\xff"):
    return ".jpg"
  if data.startswith(b"RIFF") and data[8:12] == b"WEBP":
    return ".webp"
  raise ValueError("unsupported logo image")


def optimise_logo(data: bytes, extension: str) -> bytes:
  if len(data) <= 128 * 1024:
    return data
  with tempfile.NamedTemporaryFile(suffix=extension) as source, \
       tempfile.NamedTemporaryFile(suffix=extension) as output:
    source.write(data)
    source.flush()
    completed = subprocess.run(
        ["sips", "-Z", "256", source.name, "--out", output.name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
      return data
    resized = Path(output.name).read_bytes()
    return resized if 0 < len(resized) < len(data) else data


def apply_catalog(root: Path, results: list[ProbeResult]) -> tuple[int, int]:
  sketch_path = root / "sources/esp32-network-radio/esp32-network-radio.ino"
  logo_dir = root / "sources/esp32-network-radio/data/logos"
  sketch = sketch_path.read_text(encoding="utf-8")
  stations, _ = parse_existing(sketch)
  if len(stations) < BASE_STATION_COUNT:
    raise RuntimeError("firmware catalog no longer contains the 113-station base set")
  stations = stations[:BASE_STATION_COUNT]
  by_name = {name_key(item["name"]): index for index, item in enumerate(stations)}
  by_url = {item["url"]: index for index, item in enumerate(stations)}

  grouped: dict[str, list[ProbeResult]] = {}
  for result in results:
    if result.accepted:
      grouped.setdefault(name_key(result.name), []).append(result)
  selected = [max(group, key=lambda item: source_score(item.url)) for group in grouped.values()]
  selected.sort(key=lambda item: next(i for i, value in enumerate(results) if value is item))

  for result in selected:
    key = name_key(result.name)
    if result.url in by_url:
      continue
    if key in by_name:
      index = by_name[key]
      if source_score(result.url) > source_score(stations[index]["url"]):
        old_url = stations[index]["url"]
        stations[index]["url"] = result.url
        by_url.pop(old_url, None)
        by_url[result.url] = index
      continue
    if len(stations) >= 512:
      raise RuntimeError("validated catalog exceeds the 512-station capacity")
    by_name[key] = len(stations)
    by_url[result.url] = len(stations)
    stations.append({"name": result.name, "url": result.url, "logo": ""})

  remote_logos = logo_catalog()
  logo_dir.mkdir(parents=True, exist_ok=True)
  downloaded = 0
  existing_by_name = {name_key(item["name"]): item["logo"] for item in stations if item["logo"]}
  logo_jobs: list[tuple[dict[str, str], tuple[str, str]]] = []
  for station in stations:
    key = name_key(station["name"])
    if station["logo"]:
      continue
    if key in existing_by_name:
      station["logo"] = existing_by_name[key]
      continue
    match = remote_logos.get(key)
    if not match:
      continue
    logo_jobs.append((station, match))

  def download_logo(job: tuple[dict[str, str], tuple[str, str]]) -> tuple[dict[str, str], bytes, str]:
    station, match = job
    data = fetch_logo(match[1])
    extension = image_extension(data)
    return station, optimise_logo(data, extension), extension

  with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
    futures = {executor.submit(download_logo, job): job for job in logo_jobs}
    for future in concurrent.futures.as_completed(futures):
      station, _ = futures[future]
      try:
        station, data, extension = future.result()
      except Exception as error:
        print(f"logo rejected for {station['name']}: {error}", file=sys.stderr)
        continue
      filename = hashlib.sha256(data).hexdigest()[:24] + extension
      target = logo_dir / filename
      if not target.exists():
        target.write_bytes(data)
        downloaded += 1
      station["logo"] = filename

  tracked_logo_names = {
      Path(line).name
      for line in subprocess.run(
          ["git", "ls-files", str(logo_dir)], text=True,
          stdout=subprocess.PIPE, check=False
      ).stdout.splitlines()
  }
  optimised_names: dict[str, str] = {}
  obsolete_names: set[str] = set()
  for station in stations:
    filename = station["logo"]
    if filename in optimised_names:
      station["logo"] = optimised_names[filename]
      continue
    path = logo_dir / filename
    if not filename or not path.exists() or path.stat().st_size <= 128 * 1024:
      continue
    data = path.read_bytes()
    extension = image_extension(data)
    optimised = optimise_logo(data, extension)
    new_name = hashlib.sha256(optimised).hexdigest()[:24] + extension
    new_path = logo_dir / new_name
    if not new_path.exists():
      new_path.write_bytes(optimised)
    optimised_names[filename] = new_name
    station["logo"] = new_name
    if filename not in tracked_logo_names and filename != new_name:
      obsolete_names.add(filename)
  referenced_names = {station["logo"] for station in stations if station["logo"]}
  for filename in obsolete_names - referenced_names:
    (logo_dir / filename).unlink(missing_ok=True)
  for path in logo_dir.iterdir():
    if path.is_file() and path.name not in referenced_names and path.name not in tracked_logo_names:
      path.unlink()

  station_lines = [
      f'  {{"{c_quote(item["name"])}","{c_quote(item["url"])}"}},'
      for item in stations
  ]
  icon_lines = [
      f'  {{"{c_quote(item["name"])}","{c_quote(item["url"])}","{c_quote(item["logo"])}"}},'
      for item in stations if item["logo"]
  ]
  sketch = re.sub(
      r'(constexpr BuiltinStation kBuiltinStations\[\] = \{\n).*?(\n\};)',
      lambda match: match.group(1) + "\n".join(station_lines) + match.group(2),
      sketch,
      count=1,
      flags=re.S,
  )
  sketch = re.sub(
      r'(constexpr IconStation kIconStations\[\] = \{\n).*?(\n\};)',
      lambda match: match.group(1) + "\n".join(icon_lines) + match.group(2),
      sketch,
      count=1,
      flags=re.S,
  )
  sketch = re.sub(
      r'static_assert\(kBuiltinStationCount == \d+,',
      f'static_assert(kBuiltinStationCount == {len(stations)},',
      sketch,
      count=1,
  )
  sketch_path.write_text(sketch, encoding="utf-8")
  return len(stations), downloaded


def write_reports(root: Path, results: list[ProbeResult]) -> None:
  build = root / "build"
  build.mkdir(exist_ok=True)
  with (build / "station-validation.csv").open("w", newline="", encoding="utf-8-sig") as output:
    writer = csv.DictWriter(output, fieldnames=asdict(results[0]).keys())
    writer.writeheader()
    writer.writerows(asdict(result) for result in results)
  (build / "station-validation.json").write_text(
      json.dumps([asdict(result) for result in results], ensure_ascii=False, indent=2) + "\n",
      encoding="utf-8",
  )


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--apply", action="store_true", help="update the sketch and download matched logos")
  parser.add_argument("--attempts", type=int, default=3)
  parser.add_argument("--timeout", type=float, default=12.0)
  parser.add_argument("--workers", type=int, default=32)
  parser.add_argument("--use-report", action="store_true", help="reuse build/station-validation.json")
  parser.add_argument("--expected-count", type=int, default=0,
                      help="fail if the upstream entry count differs")
  args = parser.parse_args()
  root = Path(__file__).resolve().parent.parent
  report_path = root / "build/station-validation.json"
  if args.use_report:
    results = [ProbeResult(**item) for item in json.loads(report_path.read_text(encoding="utf-8"))]
  else:
    playlist, _, _ = fetch_with_retry(PLAYLIST_URL, 30, 2 * 1024 * 1024)
    entries = read_m3u(playlist.decode("utf-8-sig"))
    if args.expected_count and len(entries) != args.expected_count:
      raise RuntimeError(
          f"upstream playlist changed: expected {args.expected_count} entries, found {len(entries)}"
      )
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
      results = list(executor.map(
          lambda item: probe(item, args.attempts, args.timeout), entries
      ))
    write_reports(root, results)
  accepted = sum(result.accepted for result in results)
  print(f"validated={len(results)} accepted={accepted} rejected={len(results) - accepted}")
  if args.apply:
    count, downloaded = apply_catalog(root, results)
    print(f"catalog={count} new_logos={downloaded}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
