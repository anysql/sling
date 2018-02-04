#!/usr/bin/python2.7
# Copyright 2017 Google Inc.
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

"""Run SLING processing"""

import sling
import sling.flags as flags
import sling.log as log
import sling.task.corpora as corpora
import sling.task.wiki as wiki
import sling.task.workflow as workflow

# Command-line flags.
flags.define("--download_wikidata",
             help="download wikidata dump",
             default=False,
             action='store_true')

flags.define("--download_wikipedia",
             help="download wikipedia dump(s)",
             default=False,
             action='store_true')

flags.define("--import_wikidata",
             help="convert wikidata to sling format",
             default=False,
             action='store_true')

flags.define("--import_wikipedia",
             help="convert wikidata dump(s) to sling format",
             default=False,
             action='store_true')

flags.define("--parse_wikipedia",
             help="parse wikipedia(s)",
             default=False,
             action='store_true')

flags.define("--extract_names",
             help="extract names for items",
             default=False,
             action='store_true')

flags.define("--build_kb",
             help="build knowledge base",
             default=False,
             action='store_true')

flags.define("--build_nametab",
             help="build name table",
             default=False,
             action='store_true')

flags.define("--build_phrasetab",
             help="build name table",
             default=False,
             action='store_true')

flags.define("--dryrun",
             help="build worflows but do not run them",
             default=False,
             action='store_true')

flags.define("--build_all",
             help="build all workflows",
             default=False,
             action='store_true')

flags.define("--monitor",
             help="port number for task monitor (0 means no monitor)",
             default=6767,
             type=int,
             metavar="PORT")

flags.define("--logdir",
             help="directory where workflow logs are stored",
             default="local/logs",
             metavar="DIR")

flags.define("--refresh",
             help="refresh frequency for workflow status",
             default=10,
             type=int,
             metavar="SECS")

def run_workflow(wf):
  # In dryrun mode the workflow is just dumped without running it.
  if flags.arg.dryrun:
    print wf.wf.dump()
    return

  # Start workflow.
  log("start workflow")
  wf.wf.start()

  # Monitor workflow until it completes.
  if flags.arg.refresh == 0:
    wf.wf.wait()
  else:
    wf.wf.monitor(flags.arg.refresh)

def download_corpora():
  # Download wikidata dump.
  if flags.arg.download_wikidata:
    if flags.arg.dryrun:
      log("wikidata dump: " + corpora.wikidata_url())
    else:
      log("Download wikidata dump")
      corpora.download_wikidata()

  # Download wikipedia dumps.
  if flags.arg.download_wikipedia:
    for language in flags.arg.languages:
      if flags.arg.dryrun:
        log(language + " wikipedia dump: " + corpora.wikipedia_url(language))
      else:
        log("Download " + language + " wikipedia dump")
        corpora.download_wikipedia(language)

def import_wiki():
  if flags.arg.import_wikidata or flags.arg.import_wikipedia:
    wf = wiki.WikiWorkflow("wiki-import")
    # Import wikidata.
    if flags.arg.import_wikidata:
      log("Import wikidata")
      wf.wikidata()

    # Import wikipedia(s).
    if flags.arg.import_wikipedia:
      for language in flags.arg.languages:
        log("Import " + language + " wikipedia")
        wf.wikipedia(language=language)

    run_workflow(wf)

def parse_wikipedia():
  # Convert wikipedia pages to SLING documents.
  if flags.arg.parse_wikipedia:
    for language in flags.arg.languages:
      log("Parse " + language + " wikipedia")
      wf = wiki.WikiWorkflow("wikipedia-parsing")
      wf.parse_wikipedia(language=language)
      run_workflow(wf)

def build_knowledge_base():
  # Extract item names from wikidata and wikipedia.
  if flags.arg.extract_names:
    for language in flags.arg.languages:
      log("Extract " + language + " names")
      wf = wiki.WikiWorkflow("name-extraction")
      wf.extract_names(language=language)
      run_workflow(wf)

  # Build knowledge base repository.
  if flags.arg.build_kb:
    log("Build knowledge base repository")
    wf = wiki.WikiWorkflow("knowledge-base")
    wf.build_knowledge_base()
    run_workflow(wf)

  # Build name table.
  if flags.arg.build_nametab:
    log("Build name table")
    wf = wiki.WikiWorkflow("name-table")
    wf.build_name_table()
    run_workflow(wf)

  # Build phrase table.
  if flags.arg.build_phrasetab:
    log("Build phrase table")
    wf = wiki.WikiWorkflow("phrase-table")
    wf.build_phrase_table()
    run_workflow(wf)

if __name__ == '__main__':
  # Parse command-line arguments.
  flags.parse()

  # Check for complete rebuild.
  if flags.arg.build_all:
    flags.arg.import_wikidata = True
    flags.arg.import_wikipedia = True
    flags.arg.parse_wikipedia = True
    flags.arg.extract_names = True
    flags.arg.build_kb = True
    flags.arg.build_nametab = True
    flags.arg.build_phrasetab = True

  # Start task monitor.
  if flags.arg.monitor > 0:
    workflow.start_monitor(flags.arg.monitor)

  # Download corpora.
  download_corpora()

  # Run workflows.
  import_wiki()
  parse_wikipedia()
  build_knowledge_base()

  # Done.
  workflow.save_workflow_log(flags.arg.logdir)
  log("Done")

