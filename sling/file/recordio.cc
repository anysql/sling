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

#include "sling/file/recordio.h"

#include <algorithm>
#include <vector>

#include "sling/base/logging.h"
#include "sling/base/types.h"
#include "sling/util/fingerprint.h"
#include "sling/util/snappy.h"
#include "sling/util/varint.h"

namespace sling {

namespace {

// Default record file options.
RecordFileOptions default_options;

// Slice compression source.
class SliceSource : public snappy::Source {
 public:
  SliceSource(const Slice &slice) : slice_(slice) {}

  size_t Available() const override {
    return slice_.size() - pos_;
  }

  const char *Peek(size_t *len) override {
    *len = slice_.size() - pos_;
    return slice_.data() + pos_;
  }

  void Skip(size_t n) override {
    pos_ += n;
  }

 private:
  Slice slice_;
  int pos_ = 0;
};

}  // namespace

RecordBuffer::~RecordBuffer() {}

void RecordBuffer::Append(const char *bytes, size_t n) {
  Write(bytes, n);
}

char *RecordBuffer::GetAppendBuffer(size_t length, char *scratch) {
  Ensure(length);
  return end();
}

char *RecordBuffer::GetAppendBufferVariable(
    size_t min_size, size_t desired_size_hint,
    char *scratch, size_t scratch_size,
    size_t *allocated_size) {
  if (available() < min_size) {
    Ensure(desired_size_hint > 0 ? desired_size_hint : min_size);
  }
  *allocated_size = remaining();
  return end();
}

size_t RecordBuffer::Available() const {
  return available();
}

const char *RecordBuffer::Peek(size_t *len) {
  *len = available();
  return begin();
}

void RecordBuffer::Skip(size_t n) {
  DCHECK_LE(n, available());
  Consume(n);
}

RecordFile::IndexPage::IndexPage(uint64 pos, const Slice &data) {
  position = pos;
  size_t bytes = data.size();
  DCHECK_EQ(bytes % sizeof(IndexEntry), 0);
  size =  bytes / sizeof(IndexEntry);
  entries = reinterpret_cast<IndexEntry *>(malloc(bytes));
  CHECK(entries != nullptr) << "Out of memory";
  memcpy(entries, data.data(), bytes);
}

RecordFile::IndexPage::~IndexPage() {
  free(entries);
}

int RecordFile::IndexPage::Find(uint64 fp) const {
  int lo = 0;
  int hi = size - 1;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (entries[mid].fingerprint > fp) {
      hi = mid - 1;
    } else if (mid < hi && entries[mid + 1].fingerprint < fp) {
      lo = mid + 1;
    } else {
      break;
    }
  }
  while (lo < hi && entries[lo + 1].fingerprint < fp) lo++;
  return lo;
}

size_t RecordFile::ReadHeader(const char *data, Header *header) {
  // Read record type.
  const char *p = data;
  header->record_type = static_cast<RecordType>(*p++);
  if (header->record_type > TSDATA_RECORD) return -1;

  // Read record length.
  p = Varint::Parse64(p, &header->record_size);
  if (!p) return -1;

  // Read key length.
  if (header->record_type == FILLER_RECORD) {
    header->key_size = 0;
  } else {
    p = Varint::Parse64(p, &header->key_size);
  }

  // Read timestamp.
  if (header->record_type == TSDATA_RECORD) {
    p = Varint::Parse64(p, &header->timestamp);
  } else {
    header->timestamp = -1;
  }

  // Return number of bytes consumed.
  return p - data;
}

size_t RecordFile::WriteHeader(const Header &header, char *data) {
  // Write record type.
  char *p = data;
  *p++ = header.record_type;

  // Write record length.
  p = Varint::Encode64(p, header.record_size);

  // Write key length.
  if (header.record_type != FILLER_RECORD) {
    p = Varint::Encode64(p, header.key_size);
  }

  // Write timestamp.
  if (header.record_type == TSDATA_RECORD) {
    p = Varint::Encode64(p, header.timestamp);
  }

  // Return number of bytes written.
  return p - data;
}

RecordReader::RecordReader(File *file,
                           const RecordFileOptions &options,
                           bool owned)
    : file_(file), owned_(owned) {
  // Allocate input buffer.
  CHECK_GE(options.buffer_size, sizeof(FileHeader));
  input_.Reset(options.buffer_size);
  position_ = 0;
  CHECK(Fill(sizeof(FileHeader)));

  // Read record file header.
  CHECK_GE(input_.available(), 8)
      << "Record file truncated: " << file->filename();
  memset(&info_, 0, sizeof(FileHeader));
  memcpy(&info_, input_.begin(), 8);
  CHECK(info_.magic == MAGIC1 || info_.magic == MAGIC2)
      << "Not a record file: " << file->filename();
  CHECK_GE(input_.available(), info_.hdrlen);
  size_t hdrlen = info_.hdrlen;
  memcpy(&info_, input_.begin(), std::min(hdrlen, sizeof(FileHeader)));
  input_.Consume(hdrlen);
  position_ = hdrlen;

  // Get size of file. The index records are always at the end of the file.
  if (info_.index_start != 0) {
    size_ = info_.index_start;
  } else {
    CHECK(file_->GetSize(&size_));
  }
}

RecordReader::RecordReader(const string &filename,
                           const RecordFileOptions &options)
    : RecordReader(File::OpenOrDie(filename, "r"), options) {}

RecordReader::RecordReader(File *file)
    : RecordReader(file, default_options) {}

RecordReader::RecordReader(const string &filename)
    : RecordReader(filename, default_options) {}

RecordReader::~RecordReader() {
  CHECK(Close());
}

Status RecordReader::Close() {
  if (owned_ && file_) {
    Status s = file_->Close();
    file_ = nullptr;
    if (!s.ok()) return s;
  }
  return Status::OK;
}

Status RecordReader::Fill(uint64 needed) {
  // Flush input buffer to make room for more data.
  input_.Flush();

  // Determine how many bytes need to be read.
  DCHECK_LE(needed, input_.capacity());
  uint64 requested =
      readahead_ ? input_.remaining() : needed - input_.available();
  DCHECK_GT(requested, 0);

  // Fill buffer from file.
  uint64 read;
  Status s = file_->Read(input_.end(), requested, &read);
  if (!s.ok()) return s;
  input_.Append(read);
  return Status::OK;
}

Status RecordReader::Read(Record *record) {
  for (;;) {
    // Fill input buffer if it is nearly empty.
    if (input_.available() < MAX_HEADER_LEN) {
      Status s = Fill(MAX_HEADER_LEN);
      if (!s.ok()) return s;
    }

    // Read record header.
    Header hdr;
    size_t hdrsize = ReadHeader(input_.begin(), &hdr);
    if (hdrsize < 0) return Status(1, "Corrupt record header");

    // Skip filler records.
    if (hdr.record_type == FILLER_RECORD) {
      Status s = Skip(hdr.record_size);
      if (!s.ok()) return s;
      continue;
    } else {
      input_.Consume(hdrsize);
      record->position = position_;
      record->type = hdr.record_type;
      record->timestamp = hdr.timestamp;
      position_ += hdrsize;
    }

    // Read record into input buffer.
    if (hdr.record_size > input_.available()) {
      // Expand input buffer if needed.
      if (hdr.record_size > input_.capacity()) {
        input_.Resize(hdr.record_size);
      }

      // Read more data into input buffer.
      Status s = Fill(hdr.record_size);
      if (!s.ok()) return s;

      // Make sure we have enough data.
      if (hdr.record_size > input_.available()) {
        return Status(1, "Record truncated");
      }
    }

    // Get record key.
    if (hdr.key_size > 0) {
      record->key = Slice(input_.begin(), hdr.key_size);
      input_.Consume(hdr.key_size);
    } else {
      record->key = Slice();
    }

    // Get record value.
    size_t value_size = hdr.record_size - hdr.key_size;
    if (info_.compression == SNAPPY) {
      // Decompress record value.
      decompressed_data_.Clear();
      snappy::ByteArraySource source(input_.Consume(value_size), value_size);
      CHECK(snappy::Uncompress(&source, &decompressed_data_));
      record->value = decompressed_data_.data();
    } else if (info_.compression == UNCOMPRESSED) {
      record->value = Slice(input_.Consume(value_size), value_size);
    } else {
      return Status(1, "Unknown compression type");
    }

    position_ += hdr.record_size;
    readahead_ = true;
    return Status::OK;
  }
}

Status RecordReader::Skip(int64 n) {
  // Check if we can skip to position in input buffer.
  if (n == 0) return Status::OK;
  position_ += n;
  if (n >= 0 && n <= input_.available()) {
    input_.Consume(n);
    return Status::OK;
  }

  // Clear input buffer and seek to new position.
  int64 offset = n - input_.available();
  input_.Clear();
  readahead_ = false;
  return file_->Skip(offset);
}

Status RecordReader::Seek(uint64 pos) {
  // Check if we can skip to position in input buffer.
  if (pos == 0) pos = info_.hdrlen;
  if (pos == position_) return Status::OK;
  position_ = pos;

  int64 offset = pos - position_;
  if (offset >= 0 && offset <= input_.available()) {
    input_.Consume(offset);
    return Status::OK;
  }

  // Clear input buffer and seek to new position.
  input_.Clear();
  readahead_ = false;
  return file_->Seek(pos);
}

RecordFile::IndexPage *RecordReader::ReadIndexPage(uint64 position) {
  Record record;
  CHECK(Seek(position));
  CHECK(Read(&record));
  return new IndexPage(position, record.value);
}

RecordIndex::RecordIndex(RecordReader *reader,
                         const RecordFileOptions &options) {
  reader_ = reader;
  cache_size_ = std::max(options.index_cache_size, 2);
  if (reader->info().index_root != 0 && reader->info().index_depth == 3) {
    root_ = reader->ReadIndexPage(reader->info().index_root);
  } else {
    root_ = nullptr;
  }
}

RecordIndex::~RecordIndex() {
  delete root_;
  for (auto *p : cache_) delete p;
}

bool RecordIndex::Lookup(const Slice &key, Record *record, uint64 fp) {
  if (root_ != nullptr) {
    // Look up key in index. Multiple keys can have the same fingerprint so we
    // move forward until a match is found.
    for (int l1 = root_->Find(fp); l1 < root_->size; ++l1) {
      if (root_->entries[l1].fingerprint > fp) return false;
      IndexPage *dir = GetIndexPage(root_->entries[l1].position);
      for (int l2 = dir->Find(fp); l2 < dir->size; ++l2) {
        if (dir->entries[l2].fingerprint > fp) return false;
        IndexPage *leaf = GetIndexPage(access(dir)->entries[l2].position);
        for (int l3 = leaf->Find(fp); l3 < leaf->size; ++l3) {
          if (leaf->entries[l3].fingerprint > fp) return false;
          if (leaf->entries[l3].fingerprint == fp) {
            CHECK(reader_->Seek(leaf->entries[l3].position));
            CHECK(reader_->Read(record));
            if (record->key == key) return true;
          }
        }
      }
    }
  } else {
    // No index; find record using sequential scanning.
    CHECK(reader_->Rewind());
    while (!reader_->Done()) {
      CHECK(reader_->Read(record));
      if (record->key == key) return true;
    }
  }

  return false;
}

bool RecordIndex::Lookup(const Slice &key, Record *record) {
  return Lookup(key, record, Fingerprint(key.data(), key.size()));
}

RecordFile::IndexPage *RecordIndex::GetIndexPage(uint64 position) {
  // Try to find index page in cache.
  for (auto *p : cache_) {
    if (p->position == position) return access(p);
  }

  // Read new index page.
  IndexPage *page = access(reader_->ReadIndexPage(position));

  // Insert or replace page in cache.
  if (cache_.size() < cache_size_) {
    cache_.push_back(page);
  } else {
    // Replace oldest entry in cache.
    int oldest = 0;
    for (int i = 1; i < cache_.size(); ++i) {
      if (cache_[i]->lru < cache_[oldest]->lru) oldest = i;
    }
    delete cache_[oldest];
    cache_[oldest] = page;
  }

  return page;
}

RecordDatabase::RecordDatabase(const string &filepattern,
                               const RecordFileOptions &options) {
  std::vector<string> filenames;
  CHECK(File::Match(filepattern, &filenames));
  CHECK(!filenames.empty()) << "No files match " << filepattern;
  for (const string &filename : filenames) {
    RecordReader *reader = new RecordReader(filename, options);
    RecordIndex *index = new RecordIndex(reader, options);
    reader->Rewind();
    shards_.push_back(index);
  }
  Forward();
}

RecordDatabase::RecordDatabase(const std::vector<string> &filenames,
                               const RecordFileOptions &options) {
  for (const string &filename : filenames) {
    RecordReader *reader = new RecordReader(filename, options);
    RecordIndex *index = new RecordIndex(reader, options);
    reader->Rewind();
    shards_.push_back(index);
  }
  Forward();
}

RecordDatabase::~RecordDatabase() {
  for (auto *s : shards_) {
    delete s->reader();
    delete s;
  }
}

void RecordDatabase::Forward() {
  while (current_shard_ < shards_.size()) {
    RecordReader *reader = shards_[current_shard_]->reader();
    if (!reader->Done()) break;
    current_shard_++;
  }
}

bool RecordDatabase::Read(int shard, int64 position, Record *record) {
  RecordReader *reader = shards_[shard]->reader();
  current_shard_ = shard;
  return reader->Seek(position) && reader->Read(record);
}

bool RecordDatabase::Lookup(const Slice &key, Record *record) {
  // Compute key fingerprint and shard number.
  uint64 fp = Fingerprint(key.data(), key.size());
  current_shard_ = fp % shards_.size();
  return shards_[current_shard_]->Lookup(key, record, fp);
}

bool RecordDatabase::Next(Record *record) {
  CHECK(!Done());
  RecordReader *reader = shards_[current_shard_]->reader();
  bool ok = reader->Read(record);
  Forward();
  return ok;
}

Status RecordDatabase::Rewind() {
  for (auto *shard : shards_) {
    Status s = shard->reader()->Rewind();
    if (!s.ok()) return s;
  }
  current_shard_ = 0;
  Forward();
  return Status::OK;
}

RecordWriter::RecordWriter(File *file, const RecordFileOptions &options)
    : file_(file) {
  // Allocate output buffer.
  output_.Reset(options.buffer_size);
  position_ = 0;

  // Read existing header in append mode.
  size_t size = file->Size();
  if (options.append && size > 0) {
    // Read record file header.
    CHECK(file->Seek(0));
    CHECK(file->Read(&info_, sizeof(FileHeader)));
    CHECK(info_.magic == MAGIC1 || info_.magic == MAGIC2)
        << "Not a record file: " << file->filename();
    CHECK_EQ(info_.hdrlen, sizeof(FileHeader));
    CHECK(info_.index_start == 0) << "Cannot append to indexed record file";

    // Seek to end of file.
    CHECK(file_->Seek(size));
    position_ = size;
  } else {
    // Write file header.
    memset(&info_, 0, sizeof(info_));
    info_.magic = MAGIC2;
    info_.hdrlen = sizeof(info_);
    info_.compression = options.compression;
    info_.chunk_size = options.chunk_size;
    if (options.indexed) {
      info_.index_page_size = options.index_page_size;
    }
    output_.Write(&info_, sizeof(info_));
    position_ += sizeof(info_);
  }
}

RecordWriter::RecordWriter(const string &filename,
                           const RecordFileOptions &options)
    : RecordWriter(File::OpenOrDie(filename, options.append ? "r+" : "w"),
                   options) {}

RecordWriter::RecordWriter(File *file)
    : RecordWriter(file, default_options) {}

RecordWriter::RecordWriter(const string &filename)
    : RecordWriter(filename, default_options) {}

RecordWriter::RecordWriter(RecordReader *reader,
                           const RecordFileOptions &options) {
  reader_ = reader;
  output_.Reset(options.buffer_size);
  file_ = reader->file();
  info_ = reader->info();
  if (options.indexed) {
    info_.index_page_size = options.index_page_size;
  }
  position_ = reader->size();
}

RecordWriter::~RecordWriter() {
  CHECK(Close());
}

Status RecordWriter::Close() {
  // Check if file has already been closed.
  if (file_ == nullptr) return Status::OK;

  // Write index to disk.
  if (info_.index_page_size > 0) {
    Status s = WriteIndex();
    if (!s.ok()) return s;
  }

  // Flush output buffer.
  Status s = Flush();
  if (!s.ok()) return s;

  if (reader_ != nullptr) {
    // Transfer ownership of file to shared reader.
    reader_->owned_ = true;
  } else {
    // Close output file.
    s = file_->Close();
    if (!s.ok()) return s;
  }

  file_ = nullptr;
  return Status::OK;
}

Status RecordWriter::Flush() {
  if (output_.empty()) return Status::OK;
  Status s = file_->Write(output_.begin(), output_.available());
  if (!s.ok()) return s;
  output_.Clear();
  if (reader_ != nullptr) reader_->size_ = position_;
  return Status::OK;
}

Status RecordWriter::Write(const Record &record, uint64 *position) {
  // Compress record value if requested.
  Slice value;
  if (info_.compression == SNAPPY) {
    // Compress record value.
    SliceSource source(record.value);
    compressed_data_.Clear();
    snappy::Compress(&source, &compressed_data_);
    value = compressed_data_.data();
  } else if (info_.compression == UNCOMPRESSED) {
    // Store uncompressed record value.
    value = record.value;
  } else {
    return Status(1, "Unknown compression type");
  }

  // Compute on-disk record size estimate.
  size_t maxsize = MAX_HEADER_LEN + record.key.size() + value.size();

  // Flush output buffer if it does not have room for record.
  if (maxsize > output_.remaining()) {
    Status s = Flush();
    if (!s.ok()) return s;
  }

  // Check if record will cross chunk boundary.
  if (info_.chunk_size != 0) {
    // Records cannot be bigger than the chunk size.
    size_t size_with_skip = maxsize + MAX_SKIP_LEN;
    CHECK_LE(size_with_skip, info_.chunk_size)
        << "Record too big (" << size_with_skip << " bytes), "
        << "maximum is " << info_.chunk_size << " bytes";

    uint64 chunk_used = position_ % info_.chunk_size;
    if (chunk_used + size_with_skip > info_.chunk_size) {
      // Write filler record. For a filler record, the record size includes
      // the header.
      Header filler;
      filler.record_type = FILLER_RECORD;
      filler.record_size = info_.chunk_size - chunk_used;
      filler.key_size = 0;
      output_.Ensure(MAX_HEADER_LEN);
      size_t hdrsize = WriteHeader(filler, output_.end());
      output_.Append(hdrsize);

      // Flush output buffer.
      Status s = Flush();
      if (!s.ok()) return s;

      // Skip to next chunk boundary.
      position_ += filler.record_size;
      s = file_->Seek(position_);
      if (!s.ok()) return s;
    }
  }

  // Add record to index.
  if (info_.index_page_size > 0 && record.type != INDEX_RECORD) {
    uint64 fp = Fingerprint(record.key.data(), record.key.size());
    index_.emplace_back(fp, position_);
  }

  // Write record header.
  Header hdr;
  hdr.record_type = record.type;
  hdr.record_size = record.key.size() + value.size();
  hdr.key_size = record.key.size();
  hdr.timestamp = record.timestamp;
  if (hdr.timestamp != -1 && hdr.record_type == DATA_RECORD) {
    hdr.record_type = TSDATA_RECORD;
  }
  output_.Ensure(maxsize);
  size_t hdrsize = WriteHeader(hdr, output_.end());
  output_.Append(hdrsize);
  if (position != nullptr) *position = position_;
  position_ += hdrsize;

  // Write record key.
  if (record.key.size() > 0) {
    output_.Write(record.key);
    position_ += record.key.size();
  }

  // Write record value.
  output_.Write(value);
  position_ += value.size();

  return Status::OK;
}

Status RecordWriter::WriteIndex() {
  // Sort index.
  std::sort(index_.begin(), index_.end(),
    [](const IndexEntry &a, const IndexEntry &b) {
      return a.fingerprint < b.fingerprint;
    }
  );

  // Record index start.
  info_.index_start = position_;

  // Write leaf index pages and build index directory.
  Index directory;
  Status s = WriteIndexLevel(index_, &directory, info_.index_page_size);
  if (!s.ok()) return s;

  // Write index directory.
  Index root;
  s = WriteIndexLevel(directory, &root, info_.index_page_size);
  if (!s.ok()) return s;

  // Write index root.
  info_.index_root = position_;
  s = WriteIndexLevel(root, nullptr, root.size());
  if (!s.ok()) return s;

  // Update record file header.
  info_.index_depth = 3;
  s = Flush();
  if (!s.ok()) return s;
  s = file_->Seek(0);
  if (!s.ok()) return s;
  s = file_->Write(&info_, sizeof(info_));
  if (!s.ok()) return s;

  return Status::OK;
}

Status RecordWriter::WriteIndexLevel(const Index &level, Index *parent,
                                     int page_size) {
  for (int n = 0; n < level.size(); n += page_size) {
    // Add entry to parent level.
    if (parent != nullptr) {
      parent->emplace_back(level[n].fingerprint, position_);
    }

    // Write index page.
    Record page;
    int size = level.size() - n;
    if (size > page_size) size = page_size;
    page.value = Slice(level.data() + n, size * sizeof(IndexEntry));
    page.type = INDEX_RECORD;
    Status s = Write(page);
    if (!s.ok()) return s;
  }

  return Status::OK;
}

Status RecordWriter::AddIndex(const string &filename,
                              const RecordFileOptions &options) {
  // Open file in read/write mode.
  File *file;
  Status s = File::Open(filename, "r+", &file);
  if (!s.ok()) return s;

  // Open reader and writer using shared file.
  RecordReader *reader = new RecordReader(file, options, false);
  if (reader->info().index_start != 0) {
    // Record file already has an index.
    delete reader;
    file->Close();
    return Status::OK;
  }

  // Check version.
  if (reader->info().magic == MAGIC1) {
    return Status(1, "Record files v1 do not support indexing",  filename);
  }

  // Open writer that takes ownership of the underlying file.
  CHECK(options.indexed);
  RecordWriter *writer = new RecordWriter(reader, options);

  // Build record index.
  Record record;
  while (!reader->Done()) {
    uint64 pos = reader->Tell();
    s = reader->Read(&record);
    if (!s.ok()) return s;
    uint64 fp = Fingerprint(record.key.data(), record.key.size());
    writer->index_.emplace_back(fp, pos);
  }

  // Write index.
  s = file->Seek(reader->size());
  if (!s.ok()) return s;
  writer->position_ = reader->size();
  s = writer->Close();
  if (!s.ok()) return s;

  delete reader;
  delete writer;
  return Status::OK;
}

}  // namespace sling

