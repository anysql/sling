# Copyright 2021 Ringgaard Research ApS
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http:#www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Add photos to photo database."""

import json
import requests
import os
import re
import sys
import traceback
import urllib.parse
import sling
import sling.flags as flags

flags.define("--id",
             default=None,
             help="item id photo updates")

flags.define("--photodb",
             help="database for photo profiles",
             default="vault/photo",
             metavar="DB")

flags.define("--imgurkeys",
             default="local/keys/imgur.json",
             help="Imgur API key file")

flags.define("--caption",
             default=None,
             help="photo caption")

flags.define("--fixedcaption",
             default=None,
             help="override photo caption")

flags.define("--captionless",
             help="no photo caption",
             default=False,
             action="store_true")

flags.define("--perimagecaption",
             help="use individual captions for images in albums",
             default=False,
             action="store_true")

flags.define("--numbering",
             help="photo numbering for albums",
             default=False,
             action="store_true")

flags.define("--source",
             default=None,
             help="photo source")

flags.define("--nsfw",
             help="mark photos as nsfw",
             default=False,
             action="store_true")

flags.define("--overwrite",
             help="overwrite existing photos",
             default=False,
             action="store_true")

flags.define("-r", "--remove",
             help="remove photos",
             default=False,
             action="store_true")

flags.define("-x", "--exclude",
             help="exclude photos",
             action="append",
             nargs="+",
             metavar="URL")

flags.define("--delete",
             default=None,
             help="delete photos with match description",
             metavar="DESCRIPTION")

flags.define("--other",
             default=None,
             help="add photos from other profile")

flags.define("--truncate",
             help="truncate after first deleted photo",
             default=False,
             action="store_true")

flags.define("--check",
             help="check that photo exists before adding",
             default=False,
             action="store_true")

flags.define("--video",
             help="allow video clips",
             default=False,
             action="store_true")

flags.define("--albums",
             help="add albums from posting",
             default=False,
             action="store_true")

flags.define("--dryrun",
             help="do not update database",
             default=False,
             action="store_true")

flags.define("--batch",
             default=None,
             help="batch file for bulk import")

flags.define("--cont",
             help="continue on errors in batch mode",
             default=False,
             action="store_true")

flags.define("url",
             nargs="*",
             help="photo URLs",
             metavar="URL")

flags.parse()

# Sanity Check for (missing) profile id.
if flags.arg.id and flags.arg.id.startswith("http"):
  raise Exception("invalid id: " + flags.arg.id)

# Get excluded photos.
excluded = set()
if flags.arg.exclude != None:
  for url in flags.arg.exclude:
    excluded.add(url[0])

photodb = sling.Database(flags.arg.photodb, "photodb")
session = requests.Session()

store = sling.Store()
n_media = store["media"]
n_is = store["is"]
n_legend = store["P2096"]
n_stated_in = store["P248"]
n_has_quality = store["P1552"]
n_nsfw = store["Q2716583"]

# Get API keys for Imgur.
imgurkeys = None
if os.path.exists(flags.arg.imgurkeys):
  with open(flags.arg.imgurkeys, "r") as f:
    imgurkeys = json.load(f)

# Read item photo profile from database.
def read_profile(itemid):
  if itemid is None: return None
  data = photodb[itemid]
  if data is None: return None
  profile = store.parse(data)
  return profile

# Write item photo profile to database.
def write_profile(itemid, profile):
  data = profile.data(binary=True)
  photodb[itemid] = data

# Add photo to profile.
def add_photo(profile, url, caption=None, source=None, nsfw=False):
  # Check if photo should be excluded.
  if url in excluded:
    print("Skip excluded photo:", url)
    return 0

  # Check if photo exists.
  if flags.arg.check:
    r = session.head(url)
    if r.status_code // 100 == 3:
      redirect = r.headers['Location']
      if redirect.endswith("/removed.png"):
        print("Skip removed photo:", url, r.status_code)
        return 0

      # Check if redirect exists.
      r = session.head(redirect)
      if r.status_code != 200:
        print("Skip missing redirect:", url, r.status_code)
        return 0

      # Use redirected url.
      url = redirect
    elif r.status_code != 200:
      print("Skip missing photo:", url, r.status_code)
      return 0

    # Check content length.
    if url.startswith("https://i.reddituploads.com/"):
      length = r.headers.get("Content-Length")
      if length == 0:
        print("Skip empty photo:", url)
        return 0

    # Check content type.
    ct = r.headers.get("Content-Type")
    if ct != None and not ct.startswith("image/"):
      print("Skip non-image content:", url, ct)
      return 0

  # Check if photo is already in the profile.
  alturl = None
  if url.startswith("https://imgur.com/"):
    alturl = "https://i.imgur.com/" + url[18:]
  elif url.startswith("https://i.imgur.com/"):
    alturl = "https://imgur.com/" + url[20:]
  for media in profile(n_media):
    media = store.resolve(media)
    if media == url or media == alturl:
      print("Skip existing photo", url)
      return 0

  print("Add", url,
        caption if caption != None else "",
        "NSFW" if nsfw else "")

  # Add media to profile.
  slots = [(n_is, url)]
  if flags.arg.fixedcaption: caption = flags.arg.fixedcaption
  if caption and not flags.arg.captionless: slots.append((n_legend, caption))
  if source: slots.append((n_stated_in, store[source]))
  if nsfw: slots.append((n_has_quality, n_nsfw))
  if len(slots) == 1:
    profile.append(n_media, url)
  else:
    frame = store.frame(slots)
    profile.append(n_media, frame)

  return 1

# Add Imgur album.
def add_imgur_album(profile, albumid, caption, isnsfw=False):
  print("Imgur album", albumid)
  auth = {'Authorization': "Client-ID " + imgurkeys["clientid"]}
  r = session.get("https://api.imgur.com/3/album/" + albumid, headers=auth)
  if r.status_code == 404:
    print("Skipping missing album", albumid)
    return 0
  if r.status_code == 403:
    print("Skipping unaccessible album", albumid)
    return 0
  r.raise_for_status()
  reply = r.json()["data"]
  #print(json.dumps(reply, indent=2))

  serial = 1
  total = len(reply["images"])
  album_title = reply["title"]
  if album_title is None:
     album_title = caption
  elif caption is not None and caption.startswith(album_title):
    album_title = caption

  count = 0
  for image in reply["images"]:
    link = image["link"]

    # Remove query parameters.
    qs = link.find("?")
    if qs != -1: link = link[:qs]

    # Skip anmated GIFs.
    if (not flags.arg.video and image["animated"]):
      print("Skipping animated image", link);
      continue

    # Image caption.
    if flags.arg.perimagecaption:
      title = image["title"]
      if title is None:
        title = image["description"]
    else:
      title = None
      
    if title is None and album_title != None:
      if flags.arg.numbering:
        title = album_title + " (%d/%d)" % (serial, total)
      else:
        title = album_title
    if title != None:
      title = title.replace("\n", " ").strip()

    # NSFW flag.
    nsfw = isnsfw or reply["nsfw"] or image["nsfw"]

    # Add media frame to profile.
    if add_photo(profile, link, title, None, nsfw): count += 1
    serial += 1
  return count

# Add Imgur image.
def add_imgur_image(profile, imageid, isnsfw=False):
  print("Imgur image", imageid)
  auth = {'Authorization': "Client-ID " + imgurkeys["clientid"]}
  r = session.get("https://api.imgur.com/3/image/" + imageid, headers=auth)
  if r.status_code == 404:
    print("Skipping missing image", imageid)
    return 0
  r.raise_for_status()
  reply = r.json()["data"]
  #print(json.dumps(reply, indent=2))

  # Photo URL.
  link = reply["link"]
  qs = link.find("?")
  if qs != -1: link = link[:qs]

  # Skip anmated GIFs.
  if (not flags.arg.video and reply["animated"]):
    print("Skipping animated image", link);
    return 0

  # Image caption.
  caption = reply["title"]
  if caption is None:
    caption = reply["description"]
  if caption != None:
    caption = caption.replace("\n", " ").strip()

  # NSFW flag.
  nsfw = isnsfw or reply["nsfw"] or reply["nsfw"]

  # Add media frame to profile.
  return add_photo(profile, link, caption, None, nsfw)

# Add Reddit gallery.
def add_reddit_gallery(profile, galleryid, caption, isnsfw=False):
  print("Redit posting", galleryid)
  r = session.get("https://api.reddit.com/api/info/?id=t3_" + galleryid,
                  headers = {"User-agent": "SLING Bot 1.0"})
  r.raise_for_status()
  children = r.json()["data"]["children"]
  if len(children) == 0:
    print("Skipping empty post", galleryid);
    return 0
  reply = children[0]["data"]
  #print(json.dumps(reply, indent=2))

  if not flags.arg.albums and reply["is_self"]:
    print("Skipping self post", galleryid);
    return 0
  if reply["removed_by_category"] != None:
    print("Skipping deleted post", galleryid);
    return 0

  if reply["is_video"]:
    print("Skipping video", galleryid);
    return 0

  mediadata = reply.get("media_metadata")
  if mediadata is None:
    url = reply.get("url")
    if url is None:
      print("Skipping empty gallery", galleryid);
      return 0

    title = reply["title"]
    if title is None: title = caption
    if flags.arg.captionless: title = None
    nsfw = isnsfw or reply["over_18"]

    count = 0
    if flags.arg.albums:
      # Fetch albums from text.
      selftext = reply["selftext"]
      for m in re.finditer("\[(.+)\]\((https?://imgur.com/a/\w+)\)", selftext):
        print("Add album", m[2], m[1])
        count += add_media(profile, m[2], m[1], nsfw)
    else:
      # Add posting media to profile.
      count = add_media(profile, url, title, nsfw)

    return count

  # Get gallery items.
  gallery = reply.get("gallery_data")
  if gallery is None:
    print("Skipping missing gallery data in", galleryid);
    return 0
  items = gallery.get("items")
  if items is None:
    print("Skipping missing gallery items in", galleryid);
    return 0

  count = 0
  serial = 1
  for item in items:
    mediaid = item["media_id"]
    media = mediadata[mediaid].get("s")
    if media is None:
      print("Skipping missing media in gallery", mediaid);
      continue
    link = media.get("u")
    if link is None:
      print("Skipping missing image in gallery", mediaid);
      continue

    m = re.match("https://preview.redd.it/(\w+\.\w+)\?", link)
    if m != None: link = "https://i.redd.it/" + m.group(1)

    # Image caption.
    title = reply["title"]
    if title is None:
      title = caption
    elif caption is not None and caption.startswith(title):
      title = caption
    if flags.arg.captionless: title = None

    if title != None and flags.arg.numbering:
      title = "%s (%d/%d)" % (title, serial, len(items))

    # NSFW flag.
    nsfw = isnsfw or reply["over_18"]

    # Add media frame to profile.
    if add_photo(profile, link, title, None, nsfw): count += 1
    serial += 1
  return count

# Add media.
def add_media(profile, url, caption, nsfw):
  # Trim url.
  url = url.replace("/i.imgur.com/", "/imgur.com/")
  url = url.replace("/www.imgur.com/", "/imgur.com/")
  url = url.replace("/m.imgur.com/", "/imgur.com/")

  url = url.replace("/www.reddit.com/", "/reddit.com/")
  url = url.replace("/old.reddit.com/", "/reddit.com/")

  if url.startswith("http://reddit.com"): url = "https" + url[4:]
  if url.startswith("http://imgur.com"): url = "https" + url[4:]

  m = re.match("(https://imgur\.com/.+)[\?#].*", url)
  if m == None: m = re.match("(https?://reddit\.com/.+)[\?#].*", url)
  if m == None: m = re.match("(https?://i\.redd\.it/.+)[\?#].*", url)
  if m == None: m = re.match("(https?://i\.redditmedia\.com/.+)[\?#].*", url)
  if m != None:
    url = m.group(1)
    if url.endswith("/new"): url = url[:-4]

  m = re.match("(https://imgur\.com/.+\.jpe?g)-\w+", url)
  if m != None: url = m.group(1)

  # Discard videos.
  if not flags.arg.video:
    if url.endswith(".gif") or \
       url.endswith(".gifv") or \
       url.endswith(".mp4") or \
       url.endswith(".webm") or \
       url.startswith("https://gfycat.com/") or \
       url.startswith("https://redgifs.com/") or \
       url.startswith("https://v.redd.it/"):
      print("Skipping video", url)
      return 0

  # Imgur album.
  m = re.match("https://imgur\.com/a/(\w+)", url)
  if m != None:
    albumid = m.group(1)
    return add_imgur_album(profile, albumid, caption, nsfw)

  # Imgur gallery.
  m = re.match("https://imgur\.com/gallery/(\w+)", url)
  if m != None:
    galleryid = m.group(1)
    return  add_imgur_album(profile, galleryid, caption, nsfw)
  m = re.match("https?://imgur\.com/\w/\w+/(\w+)", url)
  if m != None:
    galleryid = m.group(1)
    return  add_imgur_album(profile, galleryid, caption, nsfw)

  # Single-image imgur.
  m = re.match("https://imgur\.com/(\w+)$", url)
  if m != None:
    imageid = m.group(1)
    return add_imgur_image(profile, imageid, nsfw)

  # Reddit gallery.
  m = re.match("https://reddit\.com/gallery/(\w+)", url)
  if m != None:
    galleryid = m.group(1)
    return add_reddit_gallery(profile, galleryid, caption, nsfw)

  # Reddit posting.
  m = re.match("https://reddit\.com/r/\w+/comments/(\w+)/", url)
  if m != None:
    galleryid = m.group(1)
    return add_reddit_gallery(profile, galleryid, caption, nsfw)

  # DR image scaler.
  m = re.match("https://asset.dr.dk/ImageScaler/\?(.+)", url)
  if m != None:
    q = urllib.parse.parse_qs(m.group(1))
    url = "https://%s/%s" % (q["server"][0], q["file"][0])

  # Add media to profile.
  return add_photo(profile, url, caption, flags.arg.source, nsfw)

# Bulk load photos from batch file.
def bulk_load(batch):
  profiles = {}
  updated = set()
  fin = open(batch)
  num_new = 0
  num_photos = 0
  for line in fin:
    # Get id, url, and nsfw fields.
    tab = line.find('\t')
    if tab != -1: line = line[tab + 1:].strip()
    line = line.strip()
    if len(line) == 0: continue
    fields = line.split()
    id = fields[0]
    url = fields[1]
    nsfw = len(fields) >= 3 and fields[2] == "NSFW"

    # Get profile or create a new one.
    profile = profiles.get(id)
    if profile is None:
      profile = read_profile(id)
      if profile is None:
        profile = store.frame({})
        num_new += 1
      profiles[id] = profile
      print("*** PROFILE %s, %d existing photos ***************************" %
            (id, profile.count(n_media)))

    # Add media to profile.
    try:
      n = add_media(profile, url, flags.arg.caption, nsfw or flags.arg.nsfw)
      if n > 0:
        num_photos += n
        updated.add(id)
    except KeyboardInterrupt as error:
      sys.exit()
    except:
      if not flags.arg.cont: raise
      print("Error processing", url, "for", id)
      traceback.print_exc(file=sys.stdout)

  fin.close()

  # Write updated profiles.
  store.coalesce()
  for id in updated:
    profile = profiles[id]
    if flags.arg.dryrun:
      print(profile.count(n_media), "photos;", id, "not updated")
      print(profile.data(pretty=True))
    elif updated:
      print("Write", id, profile.count(n_media), "photos")
      write_profile(id, profile)

  print(len(profiles), "profiles,",
        num_new, "new,",
        len(updated), "updated,",
        num_photos, "photos")

if flags.arg.batch:
  # Bulk import.
  bulk_load(flags.arg.batch)
else:
  # Read existing photo profile for item.
  profile = read_profile(flags.arg.id)
  num_added = 0
  num_removed = 0
  if profile is None:
    profile = store.frame({})
  else:
    print(profile.count(n_media), "exisiting photos")

  # Delete all existing maedia on overwrite mode.
  if flags.arg.overwrite:
    num_removed += profile.count(n_media)
    del profile[n_media]

  if flags.arg.other:
    # Add photos from other profile.
    other = read_profile(flags.arg.other)
    for media in other(n_media):
      if type(media) is sling.Frame:
        url = media[n_is]
        caption = media[n_legend]
        nsfw = media[n_has_quality] == n_nsfw
        num_added += add_media(profile, url, caption, nsfw)
      else:
        num_added += add_media(profile, media, None, False)

  if flags.arg.remove or flags.arg.delete:
    # Remove media matching urls.
    keep = []
    truncating = False
    for media in profile(n_media):
      link = store.resolve(media)

      remove = False
      if truncating:
        remove = True
      elif link in flags.arg.url:
        remove = True
      elif flags.arg.delete and type(media) is sling.Frame:
        caption = str(media[n_legend])
        if caption and flags.arg.delete in caption: remove = True

      if remove:
        print("Remove", link)
        if flags.arg.truncate: truncating = True
        num_removed += 1
      else:
        keep.append((n_media, media))
    del profile[n_media]
    profile.extend(keep)
  else:
    # Fetch photo urls.
    for url in flags.arg.url:
      num_added += add_media(profile, url, flags.arg.caption, flags.arg.nsfw)

  # Write profile.
  if flags.arg.dryrun:
    print(profile.count(n_media), "photos;", flags.arg.id, "not updated")
    print(profile.data(pretty=True))
  elif num_added > 0 or num_removed > 0:
    print("Write", flags.arg.id,
          profile.count(n_media), "photos,",
          num_removed, "removed,",
          num_added, "added")
    store.coalesce()
    write_profile(flags.arg.id, profile)

