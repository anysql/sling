// Copyright 2020 Ringgaard Research ApS
// Licensed under the Apache License, Version 2

// Local case database.

import {Store, Encoder, Decoder} from "/common/lib/frame.js";
import {store} from "./global.js";

const n_name = store.lookup("name");
const n_description = store.lookup("description");
const n_main = store.lookup("main");
const n_caseno = store.lookup("caseno");
const n_created = store.lookup("created");
const n_modified = store.lookup("modified");
const n_topics = store.lookup("topics");
const n_share = store.lookup("share");
const n_publish = store.lookup("publish");
const n_link = store.lookup("link");

class CaseDatabase {
  // Open database.
  open() {
    return new Promise((resolve, reject) => {
      // Check if database has already been opened.
      if (this.db) resolve(this);

      // Open database.
      let request = window.indexedDB.open("Case", 1);
      request.onerror = e => {
        reject(this);
      };

      // Create database if needed.
      request.onupgradeneeded = e => {
        console.log("Database upgrade");
        let db = event.target.result;

        // Create case directory.
        let casedir = db.createObjectStore("casedir", { keyPath: "id" });
        casedir.transaction.oncomplete = e => {
          console.log("Case directory created");
        };

        // Create case data store.
        let casedata = db.createObjectStore("casedata", { keyPath: "id" });
        casedata.transaction.oncomplete = e => {
          console.log("Case data store created");
        };
      }

      // Store database connection and install global error handler on success.
      request.onsuccess = e => {
        this.db = event.target.result;
        this.db.onerror = e => this.onerror(e);
        resolve(this);
      };
    });
  }

  onerror(e) {
    console.log("Database error", e.target.error);
  }

  read(caseid) {
    return new Promise((resolve, reject) => {
      let tx = this.db.transaction(["casedata"]);
      let casedata = tx.objectStore("casedata");
      let request = casedata.get(caseid);
      request.onsuccess = e => {
        let rec = e.target.result;
        if (rec) {
          let decoder = new Decoder(store, rec.data);
          let casefile = decoder.readAll();
          resolve(casefile);
        } else {
          resolve(null);
        }
      }
      request.onerror = e => {
        reject(e);
      }
    });
  }

  writemeta(casefile) {
    // Build case directory record.
    let caseno = casefile.get(n_caseno);
    let main = casefile.get(n_main);
    let rec = {
      id: caseno,
      name: main.get(n_name),
      description: main.get(n_description),
      created: new Date(casefile.get(n_created)),
      modified: new Date(casefile.get(n_modified)),
      share: !!casefile.get(n_share),
      publish: !!casefile.get(n_publish),
      link: !!casefile.get(n_link),
    };

    // Write record to database.
    let tx = this.db.transaction(["casedir"], "readwrite");
    let casedir = tx.objectStore("casedir");
    let dirrequest = casedir.put(rec);
    dirrequest.onsuccess = e => {
      console.log("Wrote record", e.target.result, "to case directory");
    }
    return rec;
  }

  write(casefile) {
    // Encode case data.
    let encoder = new Encoder(store);
    for (let topic of casefile.get(n_topics)) {
      encoder.encode(topic);
    }
    encoder.encode(casefile);
    let data = {id: caseno, data: encoder.output()};

    // Write case data.
    let tx = this.db.transaction(["casedata"], "readwrite");
    let casedata = tx.objectStore("casedata");
    let datarequest = casedata.put(data);
    datarequest.onsuccess = e => {
      console.log("Wrote record", e.target.result, "to case store");
    }
    datarequest.onerror = e => {
      console.log("Error writing to case store", e.target.result);
    }

    // Write case metadata to case directory.
    return this.writemeta(casefile);
  }

  remove(caseid, link) {
    // Remove case from directory.
    let tx = this.db.transaction(["casedir", "casedata"], "readwrite");
    let casedir = tx.objectStore("casedir");
    let dirrequest = casedir.delete(caseid);
    dirrequest.onsuccess = e => {
      console.log("Removed record", caseid, "from case directory");
    }

    if (!link) {
      // Remove case from data store.
      let casedata = tx.objectStore("casedata");
      let datarequest = casedata.delete(caseid);
      datarequest.onsuccess = e => {
        console.log("Removed record", caseid, "from case store");
      }
      datarequest.onerror = e => {
        console.log("Error removing data from case store", e.target.result);
      }
    }
  }

  readdir() {
    return new Promise((resolve, reject) => {
      // Read case directory and add to list.
      let caselist = new Array();
      let casedir = this.db.transaction("casedir").objectStore("casedir");
      casedir.openCursor().onsuccess = e => {
        var cursor = e.target.result;
        if (cursor) {
          caselist.push(cursor.value);
          cursor.continue();
        } else {
          resolve(caselist);
        }
      };
    });
  }
};

export var casedb = new CaseDatabase();

