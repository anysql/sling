// Copyright 2020 Ringgaard Research ApS
// Licensed under the Apache License, Version 2

import {Component} from "/common/lib/component.js";
import {MdSearchResult} from "/common/lib/material.js";

import {store, settings} from "./global.js";

const n_is = store.is;

export class OmniBox extends Component {
  constructor(state) {
    super(state);
    this.backends = new Array();
  }

  add(backend) {
    this.backends.push(backend);
  }

  onconnected() {
    this.bind("md-search", "query", e => this.onquery(e));
  }

  async onquery(e) {
    let detail = e.detail
    let target = e.target;
    let query = detail.trim();

    // Do full match if query ends with period.
    let full = false;
    if (query.endsWith(".")) {
      full = true;
      query = query.slice(0, -1);
    }

    // Collect search results from backends.
    let items = new Array();
    for (let backend of this.backends) {
      await backend(query, full, items);
    }

    // Convert items to search results filtering out duplicates.
    let results = new Array();
    let seen = new Set();
    for (let item of items) {
      if (item.ref && seen.has(item.ref)) continue;
      if (item.topic) {
        for (let ref of item.topic.all(n_is)) seen.add(ref.id);
      }
      results.push(new MdSearchResult(item));
    }

    target.populate(detail, results);
  }

  query() {
    return this.find("md-search").query();
  }

  clear() {
    return this.find("md-search").clear();
  }

  render() {
    return `
      <form>
        <md-search placeholder="Search for topic..." min-length=2>
        </md-search>
      </form>
    `;
  }

  static stylesheet() {
    return `
      $ {
        display: block;
        width: 100%;
        max-width: 800px;
        padding-left: 10px;
        padding-right: 3px;
      }

      $ form {
        display: flex;
        width: 100%;
      }
    `;
  }
}

Component.register(OmniBox);

