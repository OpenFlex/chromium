// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/safe_browsing_store_file.h"

#include "base/callback.h"

namespace {

// NOTE(shess): kFileMagic should not be a byte-wise palindrome, so
// that byte-order changes force corruption.
const int32 kFileMagic = 0x600D71FE;
const int32 kFileVersion = 7;  // SQLite storage was 6...

// Header at the front of the main database file.
struct FileHeader {
  int32 magic, version;
  int32 add_chunk_count, sub_chunk_count;
  int32 add_prefix_count, sub_prefix_count;
  int32 add_hash_count, sub_hash_count;
};

// Header for each chunk in the chunk-accumulation file.
struct ChunkHeader {
  int32 add_prefix_count, sub_prefix_count;
  int32 add_hash_count, sub_hash_count;
};

// Rewind the file.  Using fseek(2) because rewind(3) errors are
// weird.
bool FileRewind(FILE* fp) {
  int rv = fseek(fp, 0, SEEK_SET);
  DCHECK_EQ(rv, 0);
  return rv == 0;
}

// Read an array of |nmemb| items from |fp| into |ptr|.  Return true
// on success.
template <class T>
bool ReadArray(T* ptr, size_t nmemb, FILE* fp) {
  const size_t ret = fread(ptr, sizeof(T), nmemb, fp);
  if (ret != nmemb)
    return false;
  return true;
}

// Write an array of |nmemb| items from |ptr| to |fp|.  Return true on
// success.
template <class T>
bool WriteArray(const T* ptr, size_t nmemb, FILE* fp) {
  const size_t ret = fwrite(ptr, sizeof(T), nmemb, fp);
  if (ret != nmemb)
    return false;
  return true;
}

// Expand |values| to fit |count| new items, and read those items from
// |fp|.  Returns true on success.
template <class T>
bool ReadToVector(std::vector<T>* values, size_t count, FILE* fp) {
  // Pointers into an empty vector may not be valid.
  if (!count)
    return true;

  // Grab the size for purposes of finding where to read to.  The
  // resize could invalidate any iterator captured here.
  const size_t original_size = values->size();
  values->resize(original_size + count);

  // Sayeth Herb Sutter: Vectors are guaranteed to be contiguous.  So
  // get a pointer to where to read the data to.
  T* ptr = &((*values)[original_size]);
  if (!ReadArray(ptr, count, fp)) {
    values->resize(original_size);
    return false;
  }

  return true;
}

// Write all of |values| to |fp|.  Returns true on succsess.
template <class T>
bool WriteVector(const std::vector<T>& values, FILE* fp) {
  // Pointers into empty vectors may not be valid.
  if (values.empty())
    return true;

  // Sayeth Herb Sutter: Vectors are guaranteed to be contiguous.  So
  // get a pointer to where to write from.
  const T* ptr = &(values[0]);
  return WriteArray(ptr, values.size(), fp);
}

// Remove deleted items (|chunk_id| in |del_set|) from the vector
// starting at |offset| running to |end()|.
template <class T>
void RemoveDeleted(std::vector<T>* vec, size_t offset,
                   const base::hash_set<int32>& del_set) {
  DCHECK(vec);

  // Scan through the items read, dropping the items in |del_set|.
  typename std::vector<T>::iterator add_iter = vec->begin() + offset;
  for (typename std::vector<T>::iterator iter = add_iter;
       iter != vec->end(); ++iter) {
    if (del_set.count(iter->chunk_id) == 0) {
      *add_iter = *iter;
      ++add_iter;
    }
  }
  vec->erase(add_iter, vec->end());
}

// Combine |ReadToVector()| and |RemoveDeleted()|.  Returns true on
// success.
template <class T>
bool ReadToVectorAndDelete(std::vector<T>* values, size_t count, FILE* fp,
                           const base::hash_set<int32>& del_set) {
  const size_t original_size = values->size();
  if (!ReadToVector(values, count, fp))
    return false;

  RemoveDeleted(values, original_size, del_set);
  return true;
}

// Read an array of |count| integers and add them to |values|.
// Returns true on success.
bool ReadToChunkSet(std::set<int32>* values, size_t count, FILE* fp) {
  if (!count)
    return true;

  std::vector<int32> flat_values;
  if (!ReadToVector(&flat_values, count, fp))
    return false;

  values->insert(flat_values.begin(), flat_values.end());
  return true;
}

// Write the contents of |values| as an array of integers.  Returns
// true on success.
bool WriteChunkSet(const std::set<int32>& values, FILE* fp) {
  if (values.empty())
    return true;

  const std::vector<int32> flat_values(values.begin(), values.end());
  return WriteVector(flat_values, fp);
}

// Delete the chunks in |deleted| from |chunks|.
void DeleteChunksFromSet(const base::hash_set<int32>& deleted,
                         std::set<int32>* chunks) {
  for (std::set<int32>::iterator iter = chunks->begin();
       iter != chunks->end();) {
    std::set<int32>::iterator prev = iter++;
    if (deleted.count(*prev) > 0)
      chunks->erase(prev);
  }
}

}  // namespace

SafeBrowsingStoreFile::SafeBrowsingStoreFile()
    : chunks_written_(0),
      file_(NULL) {
}
SafeBrowsingStoreFile::~SafeBrowsingStoreFile() {
  Close();
}

bool SafeBrowsingStoreFile::Delete() {
  // The database should not be open at this point.  But, just in
  // case, close everything before deleting.
  if (!Close()) {
    NOTREACHED();
    return false;
  }

  if (!file_util::Delete(filename_, false) &&
      file_util::PathExists(filename_)) {
    NOTREACHED();
    return false;
  }

  const FilePath new_filename = TemporaryFileForFilename(filename_);
  if (!file_util::Delete(new_filename, false) &&
      file_util::PathExists(new_filename)) {
    NOTREACHED();
    return false;
  }

  return true;
}

void SafeBrowsingStoreFile::Init(const FilePath& filename,
                                 Callback0::Type* corruption_callback) {
  filename_ = filename;
  corruption_callback_.reset(corruption_callback);
}

bool SafeBrowsingStoreFile::OnCorruptDatabase() {
  if (corruption_callback_.get())
    corruption_callback_->Run();

  // Return false as a convenience to callers.
  return false;
}

bool SafeBrowsingStoreFile::Close() {
  ClearUpdateBuffers();

  // Make sure the files are closed.
  file_.reset();
  new_file_.reset();
  return true;
}

bool SafeBrowsingStoreFile::BeginUpdate() {
  DCHECK(!file_.get() && !new_file_.get());

  // Structures should all be clear unless something bad happened.
  DCHECK(add_chunks_cache_.empty());
  DCHECK(sub_chunks_cache_.empty());
  DCHECK(add_del_cache_.empty());
  DCHECK(sub_del_cache_.empty());
  DCHECK(add_prefixes_.empty());
  DCHECK(sub_prefixes_.empty());
  DCHECK(add_hashes_.empty());
  DCHECK(sub_hashes_.empty());
  DCHECK_EQ(chunks_written_, 0);

  const FilePath new_filename = TemporaryFileForFilename(filename_);
  file_util::ScopedFILE new_file(file_util::OpenFile(new_filename, "wb+"));
  if (new_file.get() == NULL)
    return false;

  file_util::ScopedFILE file(file_util::OpenFile(filename_, "rb"));
  empty_ = (file.get() == NULL);
  if (empty_) {
    // If the file exists but cannot be opened, try to delete it (not
    // deleting directly, the bloom filter needs to be deleted, too).
    if (file_util::PathExists(filename_))
      return OnCorruptDatabase();

    new_file_.swap(new_file);
    return true;
  }

  FileHeader header;
  if (!ReadArray(&header, 1, file.get()))
    return OnCorruptDatabase();

  if (header.magic != kFileMagic || header.version != kFileVersion)
    return OnCorruptDatabase();

  if (!ReadToChunkSet(&add_chunks_cache_, header.add_chunk_count, file.get()) ||
      !ReadToChunkSet(&sub_chunks_cache_, header.sub_chunk_count, file.get()))
    return OnCorruptDatabase();

  file_.swap(file);
  new_file_.swap(new_file);
  return true;
}

bool SafeBrowsingStoreFile::FinishChunk() {
  if (!add_prefixes_.size() && !sub_prefixes_.size() &&
      !add_hashes_.size() && !sub_hashes_.size())
    return true;

  ChunkHeader header;
  header.add_prefix_count = add_prefixes_.size();
  header.sub_prefix_count = sub_prefixes_.size();
  header.add_hash_count = add_hashes_.size();
  header.sub_hash_count = sub_hashes_.size();
  if (!WriteArray(&header, 1, new_file_.get()))
    return false;

  if (!WriteVector(add_prefixes_, new_file_.get()) ||
      !WriteVector(sub_prefixes_, new_file_.get()) ||
      !WriteVector(add_hashes_, new_file_.get()) ||
      !WriteVector(sub_hashes_, new_file_.get()))
    return false;

  ++chunks_written_;

  // Clear everything to save memory.
  return ClearChunkBuffers();
}

bool SafeBrowsingStoreFile::DoUpdate(
    const std::vector<SBAddFullHash>& pending_adds,
    std::vector<SBAddPrefix>* add_prefixes_result,
    std::vector<SBAddFullHash>* add_full_hashes_result) {
  DCHECK(file_.get() || empty_);
  DCHECK(new_file_.get());

  std::vector<SBAddPrefix> add_prefixes;
  std::vector<SBSubPrefix> sub_prefixes;
  std::vector<SBAddFullHash> add_full_hashes;
  std::vector<SBSubFullHash> sub_full_hashes;

  // Read |file_| into the vectors.
  if (!empty_) {
    DCHECK(file_.get());

    if (!FileRewind(file_.get()))
      return OnCorruptDatabase();

    // Read the file header and make sure it looks right.
    FileHeader header;
    if (!ReadArray(&header, 1, file_.get()))
      return OnCorruptDatabase();

    if (header.magic != kFileMagic || header.version != kFileVersion)
      return OnCorruptDatabase();

    // Re-read the chunks-seen data to get to the later data in the
    // file.  No new elements should be added to the sets.
    // NOTE(shess): Reading rather than fseek() because calculating
    // checksums (future CL) will need to scan all data.  The code
    // could just remember state from |BeginUpdate()|, but that call
    // may be far removed from this call in time, so this seems like a
    // reasonable trade-off.
    if (!ReadToChunkSet(&add_chunks_cache_, header.add_chunk_count,
                        file_.get()) ||
        !ReadToChunkSet(&sub_chunks_cache_, header.sub_chunk_count,
                        file_.get()))
      return OnCorruptDatabase();

    if (!ReadToVectorAndDelete(&add_prefixes, header.add_prefix_count,
                               file_.get(), add_del_cache_) ||
        !ReadToVectorAndDelete(&sub_prefixes, header.sub_prefix_count,
                               file_.get(), sub_del_cache_) ||
        !ReadToVectorAndDelete(&add_full_hashes, header.add_hash_count,
                               file_.get(), add_del_cache_) ||
        !ReadToVectorAndDelete(&sub_full_hashes, header.sub_hash_count,
                               file_.get(), sub_del_cache_))
      return OnCorruptDatabase();

    // Close the file so we can later rename over it.
    file_.reset();
  }
  DCHECK(!file_.get());

  // Rewind the temporary storage.
  if (!FileRewind(new_file_.get()))
    return false;

  // Append the accumulated chunks onto the vectors from file_.
  for (int i = 0; i < chunks_written_; ++i) {
    ChunkHeader header;

    if (!ReadArray(&header, 1, new_file_.get()))
      return false;

    // TODO(shess): If the vectors were kept sorted, then this code
    // could use std::inplace_merge() to merge everything together in
    // sorted order.  That might still be slower than just sorting at
    // the end if there were a large number of chunks.  In that case
    // some sort of recursive binary merge might be in order (merge
    // chunks pairwise, merge those chunks pairwise, and so on, then
    // merge the result with the main list).
    if (!ReadToVectorAndDelete(&add_prefixes, header.add_prefix_count,
                               new_file_.get(), add_del_cache_) ||
        !ReadToVectorAndDelete(&sub_prefixes, header.sub_prefix_count,
                               new_file_.get(), sub_del_cache_) ||
        !ReadToVectorAndDelete(&add_full_hashes, header.add_hash_count,
                               new_file_.get(), add_del_cache_) ||
        !ReadToVectorAndDelete(&sub_full_hashes, header.sub_hash_count,
                               new_file_.get(), sub_del_cache_))
      return false;
  }

  // Append items from |pending_adds| which haven't been deleted.
  for (std::vector<SBAddFullHash>::const_iterator iter = pending_adds.begin();
       iter != pending_adds.end(); ++iter) {
    if (add_del_cache_.count(iter->chunk_id) == 0)
      add_full_hashes.push_back(*iter);
  }

  // Knock the subs from the adds.
  SBProcessSubs(&add_prefixes, &sub_prefixes,
                &add_full_hashes, &sub_full_hashes);

  // We no longer need to track deleted chunks.
  DeleteChunksFromSet(add_del_cache_, &add_chunks_cache_);
  DeleteChunksFromSet(sub_del_cache_, &sub_chunks_cache_);

  // Write the new data to new_file_.
  // TODO(shess): If we receive a lot of subs relative to adds,
  // overwriting the temporary chunk data in new_file_ with the
  // permanent data could leave additional data at the end.  Won't
  // cause any problems, but does waste space.  There is no truncate()
  // for stdio.  Could use ftruncate() or re-open the file.  Or maybe
  // ignore it, since we'll likely rewrite the file soon enough.
  if (!FileRewind(new_file_.get()))
    return false;

  FileHeader header;
  header.magic = kFileMagic;
  header.version = kFileVersion;
  header.add_chunk_count = add_chunks_cache_.size();
  header.sub_chunk_count = sub_chunks_cache_.size();
  header.add_prefix_count = add_prefixes.size();
  header.sub_prefix_count = sub_prefixes.size();
  header.add_hash_count = add_full_hashes.size();
  header.sub_hash_count = sub_full_hashes.size();
  if (!WriteArray(&header, 1, new_file_.get()))
    return false;

  // Write all the chunk data.
  if (!WriteChunkSet(add_chunks_cache_, new_file_.get()) ||
      !WriteChunkSet(sub_chunks_cache_, new_file_.get()) ||
      !WriteVector(add_prefixes, new_file_.get()) ||
      !WriteVector(sub_prefixes, new_file_.get()) ||
      !WriteVector(add_full_hashes, new_file_.get()) ||
      !WriteVector(sub_full_hashes, new_file_.get()))
    return false;

  // Close the file handle and swizzle the file into place.
  new_file_.reset();
  if (!file_util::Delete(filename_, false) &&
      file_util::PathExists(filename_))
    return false;

  const FilePath new_filename = TemporaryFileForFilename(filename_);
  if (!file_util::Move(new_filename, filename_))
    return false;

  // Pass the resulting data off to the caller.
  add_prefixes_result->swap(add_prefixes);
  add_full_hashes_result->swap(add_full_hashes);

  return true;
}

bool SafeBrowsingStoreFile::FinishUpdate(
    const std::vector<SBAddFullHash>& pending_adds,
    std::vector<SBAddPrefix>* add_prefixes_result,
    std::vector<SBAddFullHash>* add_full_hashes_result) {
  bool ret = DoUpdate(pending_adds,
                      add_prefixes_result, add_full_hashes_result);

  if (!ret) {
    CancelUpdate();
    return false;
  }

  DCHECK(!new_file_.get());
  DCHECK(!file_.get());

  return Close();
}

bool SafeBrowsingStoreFile::CancelUpdate() {
  return Close();
}
