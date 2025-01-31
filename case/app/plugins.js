// Copyright 2020 Ringgaard Research ApS
// Licensed under the Apache License, Version 2

// SLING case plug-ins.

import {store, settings} from "./global.js";
import {xref_patterns} from "./social.js";

// Actions.
export const SEARCH    = 0;
export const SEARCHURL = 1;
export const PASTE     = 2;
export const PASTEURL  = 3;

// Case plug-ins.
var plugins = [

// Wikidata item.
{
  name: "wikidata",
  module: "wikidata.js",
  actions: [SEARCHURL],
  patterns: [
    /^https:\/\/(www\.)?wikidata\.org\/wiki\//,
  ],
},


// Wikipedia page.
{
  name: "wikipedia",
  module: "wikipedia.js",
  actions: [PASTEURL, SEARCHURL],
  patterns: [
    /^https:\/\/\w+\.wikipedia\.org\/wiki\//,
  ],
},

// Twitter profiles.
{
  name: "twitter",
  module: "twitter.js",
  actions: [PASTEURL, SEARCHURL],
  patterns: [
    /^https:\/\/(mobile\.)?twitter\.com\//,
  ],
},

// CVR entities.
{
  name: "cvr",
  module: "cvr.js",
  actions: [PASTEURL, SEARCHURL],
  patterns: [
    /^https:\/\/datacvr\.virk\.dk\/data\/visenhed/,
  ],
},

// Linktree profiles.
{
  name: "linktree",
  module: "linktree.js",
  actions: [PASTEURL, SEARCHURL],
  patterns: [
    /^https:\/\/linktr.ee\//,
  ],
},

// Babepedia profiles.
{
  name: "babepedia",
  module: "babepedia.js",
  actions: [PASTEURL, SEARCHURL],
  patterns: [
    /^https:\/\/www\.babepedia\.com\/babe\//,
  ],
},

// Se & Hør profiles from hjemmestrik.dk.
{
  name: "hjemmestrik",
  module: "hjemmestrik.js",
  actions: [PASTEURL, SEARCHURL],
  patterns: [
    /^https:\/\/hjemmestrik.dk\/pige\//,
  ],
},

// Social media profiles from r/BeautifulFemales etc.
{
  name: "beautyfem",
  module: "beautyfem.js",
  actions: [PASTEURL, SEARCHURL],
  patterns: [
    /^https:\/\/www\.reddit\.com\/r\/BeautifulFemales\/comments\//,
    /^https:\/\/www\.reddit\.com\/r\/(HoneyDrip|HoneyDripSFW)\/comments\//,
  ],
},

// Photo albums from Reddit and Imgur.
{
  name: "albums",
  module: "albums.js",
  actions: [PASTEURL],
  patterns: [
    /^https?:\/\/(i\.|www\|m\.)?imgur.com\//,
    /^https?:\/\/(www\.|old\.)?reddit\.com\/gallery\//,
    /^https?:\/\/(www\.|old\.)?reddit\.com\/\w+\/\w+\/comments\//,
    /^https?:\/\/i\.redd\.it\//,
    /^https?:\/\/i\.redditmedia\.com\//,
    /^https?:\/\/preview\.redd\.it\//,
    /^https?:\/\/asset\.dr\.dk\//,
  ],
},

// Video transcoding.
{
  name: "transcode",
  module: "transcode.js",
  actions: [PASTEURL],
  patterns: [
    /^https?:\/\/.+\.(avi|wmv|mp4)(\/file)?$/,
  ],
},

// Images from forum posts.
{
  name: "forum",
  module: "forum.js",
  actions: [PASTEURL],
  patterns: [
    /^https?:\/\/[A-Za-z0-9\-\_\.]+\/showpost.php\?/,
    /^https?:\/\/[A-Za-z0-9\-\_\.]+\/(galleries|gallery|albums|girls|gals)\//,
    /^https?:\/\/[A-Za-z0-9\-\_\.]+\/[a-z\-]+\-(gallery|galley)$/,
    /^https?:\/\/www\.in\-the\-raw\.org\//,
    /^https?:\/\/forum\.burek\.com\//,
    /^https?:\/\/glam0ur\.net\//,
    /^https?:\/\/celeb\.gate\.cc\//,
    /^https?:\/\/forum\.phun\.org\/threads\//,
    /^https?:\/\/.+\/gallery.html$/,
    /^https?:\/\/thekameraclub\.co\.uk\//,
  ],
},

// Knowledge base links.
{
  name: "kb",
  module: "kb.js",
  actions: [PASTEURL],
  patterns: [
    /^https?:\/\/ringgaard\.com\/kb\//,
  ],
},

// Cross-reference links.
{
  name: "xref",
  module: "xrefs.js",
  actions: [PASTEURL, SEARCHURL],
  patterns: xref_patterns(),
},

// Media files.
{
  name: "images",
  module: "images.js",
  actions: [PASTEURL],
  patterns: [
    /^https?:\/\/.*\.(jpg|jpeg|gif|png|mp4|webm)([\/\?].+)?$/i,
  ],
},

// Web articles.
{
  name: "article",
  module: "article.js",
  actions: [SEARCHURL],
  patterns: [
    /^https?:\/\//,
  ],
},

];

function parse_url(url) {
  try {
    return new URL(url);
  } catch (_) {
    return null;
  }
}

export class Context {
  constructor(topic, casefile, editor) {
    this.topic = topic;
    this.casefile = casefile;
    this.editor = editor;
    this.select = true;
    this.added = null;
    this.updates = null;
  }

  new_topic() {
    let topic = editor.new_topic();
    if (!topic) return null;
    this.added = topic;
    return topic;
  }

  updated(topic) {
    if (topic == this.added) return;
    if (!this.updates) this.updates = new Array();
    if (!this.updates.includes(topic)) this.updates.push(topic);
    this.editor.mark_dirty();
  }

  async refresh() {
    if (this.added) {
      await this.editor.update_topics();
      if (this.select) {
        await this.editor.navigate_to(this.added);
      }
    } else if (this.updates) {
      for (let topic of this.updates) {
        this.editor.update_topic(topic);
      }
    }
  }

  service(name, params) {
    let url = `/case/service/${name}`;
    if (params) {
      let qs = new URLSearchParams(params);
      url += "?" + qs.toString();
    }
    return url;
  }

  proxy(url) {
    return `/case/proxy?url=${encodeURIComponent(url)}`;
  }

  kblookup(query, params) {
    let qs = new URLSearchParams(params);
    qs.append("q", query);
    qs.append("fmt", "cjson");
    let url = `${settings.kbservice}/kb/query?${qs}`;
    return fetch(url);
  }

  async idlookup(prop, identifier) {
    let query = prop.id + "/" + identifier;
    let response = await this.kblookup(query, {fullmatch: 1});
    let result = await response.json();
    if (result.matches.length == 1) {
      return store.lookup(result.matches[0].ref);
    }
  }
};

export async function process(action, query, context) {
  // Check for URL query.
  let url = parse_url(query);
  if (url) {
    if (action == SEARCH) action = SEARCHURL;
    if (action == PASTE) action = PASTEURL;
  } else if (action == SEARCHURL || action == PASTEURL) {
    return false;
  }

  // Try to find plug-in with a matching pattern.
  let result = null;
  for (let plugin of plugins) {
    let match = false;
    if (plugin.actions.includes(action)) {
      for (let pattern of plugin.patterns) {
        if (query.match(pattern)) {
          match = true;
          break;
        }
      }
    }
    if (!match) continue;

    // Load module if not already done.
    if (!plugin.instance) {
      let module_url = `/case/plugin/${plugin.module}`;
      console.log(`Load plugin ${plugin.name} from ${module_url}`);
      const { default: component } = await import(module_url);
      plugin.instance = new component();
    }

    // Let plugin process the query.
    console.log(`Run plugin ${plugin.name} for '${query}'`);
    let r = await plugin.instance.process(action, query, context);
    if (!r) continue;
    if (action == SEARCH || action == SEARCHURL) {
      if (!result) result = new Array();
      result.push(r);
    } else {
      return r;
    }
  }

  return result;
}

export async function search_plugins(context, query, full, results) {
  let result = await process(SEARCH, query, context);
  if (result instanceof Array) {
    for (let r of result) results.push(r);
  } else if (result) {
    results.push(result);
  }
}

