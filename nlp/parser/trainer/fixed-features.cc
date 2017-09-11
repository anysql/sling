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

#include "nlp/parser/trainer/feature.h"

#include "file/file.h"
#include "nlp/document/affix.h"
#include "nlp/parser/trainer/workspace.h"
#include "stream/file.h"
#include "stream/file-input.h"
#include "string/strcat.h"
#include "util/unicode.h"

namespace sling {
namespace nlp {

using syntaxnet::dragnn::ComponentSpec;

static constexpr char kUnknown[] = "<UNKNOWN>";

class PrecomputedFeature : public SemparFeature {
 public:
   void RequestWorkspaces(WorkspaceRegistry *registry) override {
    workspace_id_ = registry->Request<VectorIntWorkspace>(name());
  }

  void Preprocess(SemparState *state) override {
    auto *workspaces = state->instance()->workspaces;
    if (workspaces->Has<VectorIntWorkspace>(workspace_id_)) return;

    int size = state->num_tokens();
    VectorIntWorkspace *workspace = new VectorIntWorkspace(size);
    for (int i = 0; i < size; ++i) {
      const string &s = state->document()->token(i).text();
      workspace->set_element(i, Get(i, s));
    }
    workspaces->Set<VectorIntWorkspace>(workspace_id_, workspace);
  }

  void Extract(Args *args) override {
    int index = args->state->current() + argument();
    int begin = args->state->begin();
    if (index >= begin && index < args->state->end()) {
      int64 id = args->workspaces()->Get<VectorIntWorkspace>(
          workspace_id_).element(index + begin);
      if (id != -1) args->Output(id);
    }
  }

 protected:
  virtual int64 Get(int index, const string &word) = 0;

  // Workspace index.
  int workspace_id_ = -1;
};

namespace {

bool HasSpaces(const string &word) {
  for (char c : word) {
    if (c == ' ') return true;
  }
  return false;
}

void NormalizeDigits(string *form) {
  for (size_t i = 0; i < form->size(); ++i) {
    if ((*form)[i] >= '0' && (*form)[i] <= '9') (*form)[i] = '9';
  }
}

}  // namespace

// Feature that returns the id of the current word (offset via argument()).
class WordFeature : public PrecomputedFeature {
 public:
  void TrainInit(SharedResources *resources,
                 const ComponentSpec &spec,
                 const string &output_folder) override {
    vocabulary_file_ = StrCat(output_folder, "/", spec.name(), "-word-vocab");
    Add(kUnknown);
  }

  void TrainProcess(const Document &document) override {
    for (int t = 0; t < document.num_tokens(); ++t) {
      const auto &token = document.token(t);
      string word = token.text();
      NormalizeDigits(&word);
      if (word.empty() || HasSpaces(word)) continue;
      Add(word);
    }
  }

  int TrainFinish(ComponentSpec *spec) override {
    // Write vocabulary to file. Note that kUnknown is the first entry.
    string contents;
    DCHECK_GT(id_to_word_.size(), 0);
    for (const string &w : id_to_word_) {
      StrAppend(&contents, contents.empty() ? "" : "\n", w);
    }
    CHECK(File::WriteContents(vocabulary_file_, contents));

    // Add path to the vocabulary to the spec.
    AddResourceToSpec("word-vocab", vocabulary_file_, spec);

    return id_to_word_.size();  // includes kUnknown
  }

  void Init(const ComponentSpec &spec, SharedResources *resources) override {
    string file = GetResource(spec, "word-vocab");
    CHECK(!file.empty()) << spec.DebugString();
    FileInput input(file);
    string word;
    int64 count = 0;
    while (input.ReadLine(&word)) {
      if (!word.empty() && word.back() == '\n') word.pop_back();
      if (word == kUnknown) oov_ = count;
      Add(word);
      count++;
    }
    CHECK_NE(oov_, -1) << kUnknown << " not in " << file;
    CHECK_EQ(oov_, 0) << kUnknown << " wasn't the first entry in " << file;
    LOG(INFO) << "WordFeature: " << id_to_word_.size() << " words read, "
              << " OOV feature id: " << oov_;
  }

  string FeatureToString(int64 id) const override {
    return (id == oov_) ? kUnknown : id_to_word_.at(id);
  }

 protected:
  int64 Get(int index, const string &word) override {
    string s = word;
    NormalizeDigits(&s);
    const auto &it = words_.find(s);
    return it == words_.end() ? oov_ : it->second;
  }

 private:
  virtual void Add(const string &word) {
    const auto &it = words_.find(word);
    if (it == words_.end()) {
      int64 id = words_.size();
      words_[word] = id;
      id_to_word_.emplace_back(word);
      CHECK_EQ(id_to_word_.size(), 1 + id);
    }
  }

  // Unknown word id.
  int64 oov_ = -1;

  // Path of vocabulary under construction.
  string vocabulary_file_;

  // Word -> Id.
  std::unordered_map<string, int64> words_;

  // Id -> Word.
  std::vector<string> id_to_word_;
};

REGISTER_SEMPAR_FEATURE("word", WordFeature);

class PrefixFeature : public PrecomputedFeature {
 public:
  ~PrefixFeature() override {
    delete affixes_;
  }

  void TrainInit(SharedResources *resources,
                 const ComponentSpec &spec,
                 const string &output_folder) override {
    vocabulary_file_ =
        StrCat(output_folder, "/", spec.name(), "-", VocabularyName());
    length_ = GetIntParam("length", 3);
    affixes_ = new AffixTable(AffixType(), length_);
  }

  void TrainProcess(const Document &document) override {
    for (int t = 0; t < document.num_tokens(); ++t) {
      const auto &token = document.token(t);
      string word = token.text();
      NormalizeDigits(&word);
      if (!word.empty() && !HasSpaces(word)) {
        affixes_->AddAffixesForWord(word);
      }
    }
  }

  int TrainFinish(ComponentSpec *spec) override {
    // Write affix table to file.
    FileOutputStream output(vocabulary_file_);
    affixes_->Write(&output);
    CHECK(output.Close());

    // Add path to the vocabulary to the spec.
    AddResourceToSpec(VocabularyName(), vocabulary_file_, spec);

    return affixes_->size() + 1;  // +1 for kUnknown
  }

  void Init(const ComponentSpec &spec, SharedResources *resources) override {
    vocabulary_file_ = GetResource(spec, VocabularyName());
    CHECK(!vocabulary_file_.empty()) << spec.DebugString();

    length_ = GetIntParam("length", 3);
    affixes_ = new AffixTable(AffixType(), length_);
    FileInputStream input(vocabulary_file_);
    affixes_->Read(&input);
    oov_ = affixes_->size();
  }

  string FeatureToString(int64 id) const override {
    return (id == oov_) ? kUnknown : affixes_->AffixForm(id);
  }

 protected:
  virtual AffixTable::Type AffixType() const {
    return AffixTable::PREFIX;
  }

  virtual string VocabularyName() const {
    return "prefix-table";
  }

  int64 Get(int index, const string &word) override {
    const char *start = word.data();
    const char *end = word.data() + word.size();
    const char *p = start;
    int n = 0;
    while (n < length_ && p < end) {
      int len = UTF8::CharLen(p);
      if (p + len > end) return oov_;  // invalid utf8
      p += len;
      n++;
    }

    while (n > 0) {
      string affix(start, p - start);
      int affix_id = affixes_->AffixId(affix);
      if (affix_id != -1) return affix_id;
      p = UTF8::Previous(p, start);
      n--;
    }

    return oov_;
  }

  AffixTable *affixes_ = nullptr;
  int length_ = 0;
  int oov_ = -1;
  string vocabulary_file_;
};

REGISTER_SEMPAR_FEATURE("prefix", PrefixFeature);

class SuffixFeature : public PrefixFeature {
 protected:
  AffixTable::Type AffixType() const override {
    return AffixTable::SUFFIX;
  }

  string VocabularyName() const override {
    return "suffix-table";
  }

  int64 Get(int index, const string &word) override {
    const char *start = word.data();
    const char *end = word.data() + word.size();
    const char *p = end;
    int n = 0;
    while (n < length_ && p > start) {
      p = UTF8::Previous(p, start);
      n++;
    }

    while (n > 0) {
      string affix(p, end - p);
      int affix_id = affixes_->AffixId(affix);
      if (affix_id != -1) return affix_id;
      p = UTF8::Next(p);
      n--;
    }

    return oov_;
  }
};

REGISTER_SEMPAR_FEATURE("suffix", SuffixFeature);

class HyphenFeature : public PrecomputedFeature {
 public:
  // Enumeration of values.
  enum Category {
    NO_HYPHEN = 0,
    HAS_HYPHEN = 1,
    CARDINALITY = 2,
  };

  // Returns the final domain size of the feature.
  int TrainFinish(ComponentSpec *spec) override {
    return CARDINALITY;
  }

  string FeatureToString(int64 id) const override {
    if (id == NO_HYPHEN) return "NO_HYPHEN";
    if (id == HAS_HYPHEN) return "HAS_HYPHEN";
    return "<INVALID_HYPHEN>";
  }

 protected:
  int64 Get(int index, const string &word) override {
    return (word.find('-') != string::npos ? HAS_HYPHEN : NO_HYPHEN);
  }
};

REGISTER_SEMPAR_FEATURE("hyphen", HyphenFeature);

// Feature that categorizes the capitalization of the word. If the option
// utf8=true is specified, lowercase and uppercase checks are done with UTF8
// compliant functions.
class CapitalizationFeature : public PrecomputedFeature {
 public:
  enum Category {
    LOWERCASE = 0,                     // normal word
    UPPERCASE = 1,                     // all-caps
    CAPITALIZED = 2,                   // has one cap and one non-cap
    CAPITALIZED_SENTENCE_INITIAL = 3,  // same as above but sentence-initial
    NON_ALPHABETIC = 4,                // contains no alphabetic characters
    CARDINALITY = 5,
  };

  // Returns the final domain size of the feature.
  int TrainFinish(ComponentSpec *spec) override {
    return CARDINALITY;
  }

  // Returns a string representation of the enum value.
  string FeatureToString(int64 id) const override {
    Category category = static_cast<Category>(id);
    switch (category) {
      case LOWERCASE: return "LOWERCASE";
      case UPPERCASE: return "UPPERCASE";
      case CAPITALIZED: return "CAPITALIZED";
      case CAPITALIZED_SENTENCE_INITIAL: return "CAPITALIZED_SENTENCE_INITIAL";
      case NON_ALPHABETIC: return "NON_ALPHABETIC";
      default: return "<INVALID_CAPITALIZATION>";
    }
  }

 protected:
  int64 Get(int index, const string &word) override {
    bool has_upper = false;
    bool has_lower = false;
    const char *str = word.c_str();
    for (int i = 0; i < word.length(); ++i) {
      char c = str[i];
      has_upper = (has_upper || (c >= 'A' && c <= 'Z'));
      has_lower = (has_lower || (c >= 'a' && c <= 'z'));
    }

    // Compute simple values.
    if (!has_upper && has_lower) return LOWERCASE;
    if (has_upper && !has_lower) return UPPERCASE;
    if (!has_upper && !has_lower) return NON_ALPHABETIC;

    // Else has_upper && has_lower; a normal capitalized word.  Check the break
    // level to determine whether the capitalized word is sentence-initial.
    return (index == 0) ? CAPITALIZED_SENTENCE_INITIAL : CAPITALIZED;
  }
};

REGISTER_SEMPAR_FEATURE("capitalization", CapitalizationFeature);

// A feature for computing whether the focus token contains any punctuation
// for ternary features.
class PunctuationAmountFeature : public PrecomputedFeature {
 public:
  // Enumeration of values.
  enum Category {
    NO_PUNCTUATION = 0,
    SOME_PUNCTUATION = 1,
    ALL_PUNCTUATION = 2,
    CARDINALITY = 3,
  };

  // Returns the final domain size of the feature.
  int TrainFinish(ComponentSpec *spec) override {
    return CARDINALITY;
  }

  string FeatureToString(int64 id) const override {
    Category category = static_cast<Category>(id);
    switch (category) {
      case NO_PUNCTUATION: return "NO_PUNCTUATION";
      case SOME_PUNCTUATION: return "SOME_PUNCTUATION";
      case ALL_PUNCTUATION: return "ALL_PUNCTUATION";
      default: return "<INVALID_PUNCTUATION>";
    }
  }

 protected:
  int64 Get(int index, const string &word) override {
    bool has_punctuation = false;
    bool all_punctuation = true;

    const char *s = word.data();
    const char *end = word.data() + word.size();
    while (s < end) {
      int code = UTF8::Decode(s, end - s);
      if (code < 0) break;
      bool is_punct = Unicode::IsPunctuation(code);
      all_punctuation &= is_punct;
      has_punctuation |= is_punct;
      if (!all_punctuation && has_punctuation) return SOME_PUNCTUATION;
      s = UTF8::Next(s);
    }
    if (!all_punctuation) return NO_PUNCTUATION;
    return ALL_PUNCTUATION;
  }
};

REGISTER_SEMPAR_FEATURE("punctuation", PunctuationAmountFeature);

// A feature for a feature that returns whether the word is an open or
// close quotation mark, based on its relative position to other quotation marks
// in the sentence.
class QuoteFeature : public PrecomputedFeature {
 public:
  // Enumeration of values.
  enum Category {
    NO_QUOTE = 0,
    OPEN_QUOTE = 1,
    CLOSE_QUOTE = 2,
    UNKNOWN_QUOTE = 3,
    CARDINALITY = 4,
  };

  // Returns the final domain size of the feature.
  int TrainFinish(ComponentSpec *spec) override {
    return CARDINALITY;
  }

  string FeatureToString(int64 id) const override {
    Category category = static_cast<Category>(id);
    switch (category) {
      case NO_QUOTE: return "NO_QUOTE";
      case OPEN_QUOTE: return "OPEN_QUOTE";
      case CLOSE_QUOTE: return "CLOSE_QUOTE";
      case UNKNOWN_QUOTE: return "UNKNOWN_QUOTE";
      default: return "<INVALID_QUOTE>";
    }
  }

  // Override preprocess to compute open and close quotes from prior context of
  // the sentence.
  void Preprocess(SemparState *state) override {
    auto *workspaces = state->instance()->workspaces;
    if (workspaces->Has<VectorIntWorkspace>(workspace_id_)) return;

    // For double quote ", it is unknown whether they are open or closed without
    // looking at the prior tokens in the sentence.  in_quote is true iff an odd
    // number of " marks have been seen so far in the sentence (similar to the
    // behavior of some tokenizers).
    int size = state->num_tokens();
    VectorIntWorkspace *workspace = new VectorIntWorkspace(size);
    bool in_quote = false;
    for (int i = 0; i < size; ++i) {
      const string &s = state->document()->token(i).text();
      int64 id = Get(i, s);
      if (id == UNKNOWN_QUOTE) {
        // Update based on in_quote and flip in_quote.
        id = in_quote ? CLOSE_QUOTE : OPEN_QUOTE;
        in_quote = !in_quote;
      }
      workspace->set_element(i, id);
    }
    workspaces->Set<VectorIntWorkspace>(workspace_id_, workspace);
  }

 protected:
  int64 Get(int index, const string &word) override {
    // Penn Treebank open and close quotes are multi-character.
    if (word == "``") return OPEN_QUOTE;
    if (word == "''") return CLOSE_QUOTE;
    int code = UTF8::Decode(word.data(), word.size());
    if (code < 0) return NO_QUOTE;
    switch (Unicode::Category(code)) {
      case CHARCAT_INITIAL_QUOTE_PUNCTUATION: return OPEN_QUOTE;
      case CHARCAT_FINAL_QUOTE_PUNCTUATION: return CLOSE_QUOTE;
      case CHARCAT_OTHER_PUNCTUATION:
        if (word == "'" || word == "\"") return UNKNOWN_QUOTE;
        return NO_QUOTE;
      default:
        return NO_QUOTE;
    }
  }
};

REGISTER_SEMPAR_FEATURE("quote", QuoteFeature);

// Feature that computes whether a word has digits or not.
class DigitFeature : public PrecomputedFeature {
 public:
  // Enumeration of values.
  enum Category {
    NO_DIGIT = 0,
    SOME_DIGIT = 1,
    ALL_DIGIT = 2,
    CARDINALITY = 3,
  };

  // Returns the final domain size of the feature.
  int TrainFinish(ComponentSpec *spec) override {
    return CARDINALITY;
  }

  string FeatureToString(int64 id) const override {
    Category category = static_cast<Category>(id);
    switch (category) {
      case NO_DIGIT: return "NO_DIGIT";
      case SOME_DIGIT: return "SOME_DIGIT";
      case ALL_DIGIT: return "ALL_DIGIT";
      default: return "<INVALID_DIGIT>";
    }
  }

 protected:
  int64 Get(int index, const string &word) override {
    bool has_digit = isdigit(word[0]);
    bool all_digit = has_digit;
    for (size_t i = 1; i < word.length(); ++i) {
      bool char_is_digit = isdigit(word[i]);
      all_digit = all_digit && char_is_digit;
      has_digit = has_digit || char_is_digit;
      if (!all_digit && has_digit) return SOME_DIGIT;
    }
    if (!all_digit) return NO_DIGIT;
    return ALL_DIGIT;
  }
};

REGISTER_SEMPAR_FEATURE("digit", DigitFeature);

// Short-hand for a list of edges.
typedef std::vector<std::tuple<int, int, int>> Edges;

namespace {

// Returns a list of (i, r, j) links between all frames i and j, where
// i and j are attention indices, and r is a role index.
// i and j are values in 'frame_to_attention', and j can be -1.
// 'roles' maps role handles to indices.
void GetEdges(const ParserState *s,
              const std::unordered_map<int, int> &frame_to_attention,
              const HandleMap<int> &roles,
              Edges *edges) {
  for (const auto &kv : frame_to_attention) {
    // Attention index of the source frame.
    int source = kv.second;

    // Go over each slot of the source frame.
    Handle handle = s->frame(kv.first);
    const sling::FrameDatum *frame = s->store()->GetFrame(handle);
    for (const Slot *slot = frame->begin(); slot < frame->end(); ++slot) {
      int target = -1;
      if (slot->value.IsIndex()) {
        const auto &it = frame_to_attention.find(slot->value.AsIndex());
        if (it != frame_to_attention.end()) target = it->second;
      }

      const auto &it2 = roles.find(slot->name);
      if (it2 != roles.end()) {
        edges->push_back(std::make_tuple(source, it2->second, target));
      }
    }
  }
}

}  // namespace

// Abstract feature that uses existing links between frames.
class RoleFeature : public SemparFeature {
 public:
  void TrainInit(SharedResources *resources,
                 const ComponentSpec &spec,
                 const string &output_folder) override {
    // Collect all roles from role-parameterized actions.
    Store *global = resources->global;
    for (int i = 0; i < resources->table.NumActions(); ++i) {
      const auto &action = resources->table.Action(i);
      if (action.type == ParserAction::CONNECT ||
          action.type == ParserAction::ASSIGN ||
          action.type == ParserAction::EMBED ||
          action.type == ParserAction::ELABORATE) {
        if (roles_.find(action.role) == roles_.end()) {
          int index = roles_.size();
          roles_[action.role] = index;
          role_ids_.emplace_back(Frame(global, action.role).Id().str());
        }
      }
    }

    // We restrict to the first 'frame-limit frames in the attention buffer.
    frame_limit_ = GetIntParam("frame-limit", 5);
  }

  // Returns the domain size of the feature.
  int TrainFinish(ComponentSpec *spec) override {
    return DomainSize();
  }

  // Initializes the feature at runtime.
  void Init(const ComponentSpec &spec, SharedResources *resources) override {
    ComponentSpec unused_spec;
    TrainInit(resources, unused_spec, "" /* output_folder; unused */);
  }

  // Extracts implementation-specific feature ids from the edges.
  void Extract(SemparFeature::Args *args) override {
    CHECK(!args->state->shift_only());
    const ParserState *s = args->parser_state();

    // Construct a mapping from absolute frame index -> attention index.
    std::unordered_map<int, int> frame_to_attention;
    for (int i = 0; i < frame_limit_; ++i) {
      if (i < s->AttentionSize()) {
        frame_to_attention[s->Attention(i)] = i;
      } else {
        break;
      }
    }
    Edges edges;
    GetEdges(s, frame_to_attention, roles_, &edges);
    Extract(edges, args);
  }

 protected:
  // Returns the domain size of the feature.
  virtual int DomainSize() = 0;

  // Returns feature ids from the edges.
  virtual void Extract(const Edges &edges, SemparFeature::Args *args) = 0;

  // Number of top-attention frames to restrict to.
  int frame_limit_ = 0;

  // Set of roles considered.
  HandleMap<int> roles_;
  std::vector<string> role_ids_;
};

// Outputs (source frame id, role) features.
class OutRoleFeature : public RoleFeature {
 public:
  string FeatureToString(int64 id) const override {
    int r = roles_.size();
    return StrCat("(S=", id / r,  " -> R=", role_ids_[id % r], ")");
  }

 protected:
  int DomainSize() override {
    return frame_limit_ * roles_.size();
  }

  void Extract(const Edges &edges, SemparFeature::Args *args) override {
    for (const auto &e : edges) {
      args->Output(std::get<0>(e) * roles_.size() + std::get<1>(e));
    }
  }
};

REGISTER_SEMPAR_FEATURE("out-roles", OutRoleFeature);

// Outputs (role, target frame id) features if target is valid.
class InRoleFeature : public RoleFeature {
 public:
  string FeatureToString(int64 id) const override {
    int r = roles_.size();
    return StrCat("(T=", id / r,  " <- R=", role_ids_[id % r], ")");
  }

 protected:
  int DomainSize() override {
    return frame_limit_ * roles_.size();
  }

  void Extract(const Edges &edges, SemparFeature::Args *args) override {
    for (const auto &e : edges) {
      int target = std::get<2>(e);
      if (target != -1) {
        args->Output(target * roles_.size() + std::get<1>(e));
      }
    }
  }
};

REGISTER_SEMPAR_FEATURE("in-roles", InRoleFeature);

// Outputs (source frame, target frame) features if target is valid.
class UnlabeledRoleFeature : public RoleFeature {
 public:
  string FeatureToString(int64 id) const override {
    return StrCat("(S=", id % frame_limit_,
                  " -> T=", role_ids_[id / frame_limit_], ")");
  }

 protected:
  int DomainSize() override {
    return frame_limit_ * frame_limit_;
  }

  void Extract(const Edges &edges, SemparFeature::Args *args) override {
    for (const auto &e : edges) {
      int target = std::get<2>(e);
      if (target != -1) {
        args->Output(target * frame_limit_ + std::get<0>(e));
      }
    }
  }
};

REGISTER_SEMPAR_FEATURE("unlabeled-roles", UnlabeledRoleFeature);

// Outputs (source frame, role, target frame) features if target is valid.
class LabeledRoleFeature : public RoleFeature {
 public:
  string FeatureToString(int64 id) const override {
    int r = roles_.size();
    int fr = frame_limit_ * roles_.size();
    return StrCat("(S=", id / fr, " -> R=", role_ids_[(id % fr) % r],
                  " -> T=", (id % fr) / r, ")");
  }

 protected:
  int DomainSize() override {
    return frame_limit_ * frame_limit_ * roles_.size();
  }

  void Extract(const Edges &edges, SemparFeature::Args *args) override {
    for (const auto &e : edges) {
      int target = std::get<2>(e);
      if (target != -1) {
        args->Output(std::get<0>(e) * frame_limit_ * roles_.size() +
                     target * roles_.size() + std::get<1>(e));
      }
    }
  }
};

REGISTER_SEMPAR_FEATURE("labeled-roles", LabeledRoleFeature);

// Amalgamation of all four RoleFeatures above. The embeddings for the four
// different types of features will be summed up, so having this feature is
// 'lossy' in theory, compared to four separate role features from above.
class FrameRolesFeature : public RoleFeature {
 public:
  void TrainInit(SharedResources *resources,
                 const ComponentSpec &spec,
                 const string &output_folder) override {
    RoleFeature::TrainInit(resources, spec, output_folder);

    // Compute the offsets for the four types of features. These are laid out
    // in this order: all (i, r) features, all (r, j) features, all (i, j)
    // features, all (i, r, j) features.
    // We restrict i, j to be < frame-limit, a feature parameter.
    int combinations = frame_limit_ * roles_.size();
    outlink_offset_ = 0;
    inlink_offset_ = outlink_offset_ + combinations;
    unlabeled_link_offset_ = inlink_offset_ + combinations;
    labeled_link_offset_ = unlabeled_link_offset_ + frame_limit_ * frame_limit_;
  }

  string FeatureToString(int64 id) const override {
    CHECK_GE(id, outlink_offset_);

    int r = roles_.size();
    int fr = frame_limit_ * roles_.size();

    if (id < inlink_offset_) {
      id -= outlink_offset_;
      return StrCat("(S=", id / r,  " -> R=", role_ids_[id % r], ")");
    } else if (id < unlabeled_link_offset_) {
      id -= inlink_offset_;
      return StrCat("(T=", id / r, " <- R=", role_ids_[id % r], ")");
    } else if (id < labeled_link_offset_) {
      id -= unlabeled_link_offset_;
      return StrCat("(S=", id / frame_limit_, " -> T=", id % frame_limit_, ")");
    } else {
      id -= labeled_link_offset_;
      return StrCat("(S=", id / fr, " -> R=", role_ids_[(id % fr) % r],
                    " -> T=", (id % fr) / r, ")");
    }
  }


 protected:
  int DomainSize() override {
    return labeled_link_offset_ + frame_limit_ * frame_limit_ * roles_.size();
  }

  // Returns the four types of features.
  void Extract(const Edges &edges, SemparFeature::Args *args) override {
    int num_roles = roles_.size();
    for (const auto &e : edges) {
      int source = std::get<0>(e);
      int role = std::get<1>(e);
      int target = std::get<2>(e);

      args->Output(outlink_offset_ + source * num_roles + role);
      if (target != -1) {
        args->Output(inlink_offset_ + target * num_roles + role);
        args->Output(unlabeled_link_offset_ + source * frame_limit_ + target);
        args->Output(labeled_link_offset_ +
                     source * frame_limit_ * num_roles +
                     target * num_roles + role);
      }
    }
  }

 private:
  // Starting offset for (source, role) features.
  int outlink_offset_;

  // Starting offset for (role, target) features.
  int inlink_offset_;

  // Starting offset for (source, target) features.
  int unlabeled_link_offset_;

  // Starting offset for (source, role, target) features.
  int labeled_link_offset_;
};

REGISTER_SEMPAR_FEATURE("roles", FrameRolesFeature);

}  // namespace nlp
}  // namespace sling
