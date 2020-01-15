#!/bin/bash

LANGUAGE=${LANGUAGE:-en}
PORT=${PORT:-8080}

SPEC='{
  annotator: "mentions"
  annotator: "anaphora"
  annotator: "types"
  annotator: "clear-references"

  inputs: {
    commons: {
      file: "local/data/e/wiki/kb.sling"
      format: "store/frame"
    }
    aliases: {
      file: "local/data/e/wiki/LANG/phrase-table.repo"
      format: "repository"
    }
    dictionary: {
      file: "local/data/e/silver/LANG/idf.repo"
      format: "repository"
    }
  }
  parameters: {
    language: "LANG"
    resolve: true
    detailed: false
  }
}'

bazel-bin/sling/nlp/document/corpus-browser \
  --kb \
  --names local/data/e/wiki/$LANGUAGE/name-table.repo \
  --spec "${SPEC//LANG/$LANGUAGE}" \
  --port $PORT $@ \
  local/data/e/wiki/$LANGUAGE/documents@10.rec

