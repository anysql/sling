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

import requests
import datetime

import sling
import sling.net
import sling.util
import sling.flags as flags
import sling.log as log

flags.define("--port",
             help="HTTP port",
             default=8080,
             type=int,
             metavar="PORT")

flags.define("--number",
             help="Checkpoint file for keeping track of new case numbers",
             default=None,
             metavar="FILE")

flags.define("--number_service",
             help="Endpoint for assigning new case numbers",
             default="https://ringgaard.com/newcase",
             metavar="URL")

flags.define("--casedb",
             help="database for shared cases",
             default="case",
             metavar="DB")

flags.parse()

# Convert ISO 8601 time to unix epoch.
def iso2ts(t):
  if t is None: return None
  if t.endswith("Z"): t = t[:-1] + "+00:00"
  return int(datetime.datetime.fromisoformat(t).timestamp())

# Connect to case database.
casedb = sling.Database(flags.arg.casedb, "case.py")

# Initialize HTTP server.
app = sling.net.HTTPServer(flags.arg.port)
app.redirect("/", "/c")

# Add static files.
app.static("/common", "app", internal=True)
app.static("/c/app", "case/app")

# Commons store.
commons = sling.Store()
n_id = commons["id"]
n_modified = commons["modified"]
commons.freeze()

# Checkpoint with next case number.
numbering = None
if flags.arg.number:
  numbering = sling.util.Checkpoint(flags.arg.number)

# Template for main page.
main_page_template = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name=viewport content="width=device-width, initial-scale=1">
<link rel="icon" href="/common/image/appicon.ico" type="image/x-icon" />
<script type="module" src="/c/app/main.js"></script>
</head>
<body style="display: none;">
</body>
</html>""";

@app.route("/c")
def main_page(request):
  return main_page_template

# Frame template for new case.
case_template = """
{
  =c/#
  :casefile
  caseno: #
}
"""

@app.route("/newcase")
def new_case(request):
  if numbering:
    # Get new case number.
    client = request["X-Forwarded-For"]
    caseno = numbering.checkpoint
    numbering.commit(caseno + 1)

    # Log new case requests with IP address.
    log.info("Assign case #%d to client %s" % (caseno, client))

    # Make new case from with the assigned case number.
    store = sling.Store(commons)
    newcase = store.parse(case_template.replace("#", str(caseno)))
    return newcase
  elif flags.arg.number_service:
    # Redirect to remote case numbering service.
    return sling.net.HTTPRedirect(flags.arg.number_service)
  else:
    return 500

@app.route("/share", method="POST")
def share_case(request):
  # Get shared case.
  store = sling.Store(commons)
  casefile = request.frame(store);

  # Get case id.
  caseid = casefile[n_id]
  if caseid is None: return 400;

  # Get modification time.
  modified = casefile[n_modified];
  ts = iso2ts(modified)
  if ts is None: return 400;

  # Store case in database.
  casedb.put(caseid, request.body, version=ts)

  # Log case updates with IP address.
  client = request["X-Forwarded-For"]
  log.info("Share case %s version %d from client %s" % (caseid, ts, client))

# Run HTTP server.
log.info("HTTP server listening on port", flags.arg.port)
app.run()
log.info("Shutdown.")

