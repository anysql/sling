// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "nlp/document/lexicon.h"

#include "base/types.h"
#include "nlp/document/affix.h"
#include "stream/memory.h"
#include "util/vocabulary.h"

namespace sling {
namespace nlp {

void Lexicon::InitWords(const char *data, size_t size) {
  // Initialize mapping from words to ids.
  const static char kTerminator = '\n';
  vocabulary_.Init(data, size, kTerminator);

  // Initialize mapping from ids to words.
  words_.resize(vocabulary_.size());
  const char *current = data;
  const char *end = data + size;
  int index = 0;
  while (current < end) {
    // Find next word.
    const char *next = current;
    while (next < end && *next != kTerminator) next++;
    if (next == end) break;

    // Initialize item for word.
    words_[index].assign(current, next - current);

    current = next + 1;
    index++;
  }
}

void Lexicon::InitPrefixes(const char *data, size_t size) {
  ArrayInputStream stream(data, size);
  prefixes_.Read(&stream);
}

void Lexicon::InitSuffixes(const char *data, size_t size) {
  ArrayInputStream stream(data, size);
  suffixes_.Read(&stream);
}

int Lexicon::LookupWord(const string &word) const {
  // Lookup word in vocabulary.
  int id = vocabulary_.Lookup(word);

  if (id == -1 && normalize_digits_) {
    // Check if word has digits.
    bool has_digits = false;
    for (char c : word) if (c >= '0' && c <= '9') has_digits = true;

    if (has_digits) {
      // Normalize digits and lookup the normalized word.
      string normalized = word;
      for (char &c : normalized) if (c >= '0' && c <= '9') c = '9';
      id = vocabulary_.Lookup(normalized);
    }
  }

  return id != -1 ? id : oov_;
}

}  // namespace nlp
}  // namespace sling

