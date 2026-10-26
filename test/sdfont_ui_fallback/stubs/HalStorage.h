#pragma once

// Host stub of lib/hal/HalStorage.h for the SD-font UI fallback tests: an
// in-memory file tree with real directory iteration, mirroring the SdFat-like
// HalFile surface the production SdCardFontRegistry/SdCardFontSystem expect
// (getName/openNextFile/isDirectory/rewindDirectory, seek/read, Print base).

#include <Arduino.h>
#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using oflag_t = uint8_t;
inline constexpr oflag_t O_RDONLY = 0;
inline constexpr oflag_t O_RDWR = 1;

namespace host_fs {

struct Node {
  bool isDir = false;
  std::vector<uint8_t> bytes;
  std::map<std::string, Node, std::less<>> children;  // child name -> node
  size_t readCalls = 0;
};

inline Node root;

inline void reset() { root = Node{true, {}, {}, 0}; }

inline Node* find(const std::string& path) {
  if (path.empty() || path == "/") return &root;
  Node* node = &root;
  size_t start = path[0] == '/' ? 1 : 0;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part.empty()) return nullptr;
    const auto it = node->children.find(part);
    if (it == node->children.end()) return nullptr;
    node = &it->second;
    if (slash == std::string::npos) return node;
    start = slash + 1;
  }
  return nullptr;
}

inline void put(const std::string& path, std::vector<uint8_t> bytes) {
  Node* node = &root;
  size_t start = path[0] == '/' ? 1 : 0;
  while (start < path.size()) {
    const size_t slash = path.find('/', start);
    const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    node = &node->children[part];
    if (slash == std::string::npos) break;
    node->isDir = true;
    start = slash + 1;
  }
  node->isDir = false;
  node->bytes = std::move(bytes);
  node->readCalls = 0;
}

inline size_t totalReadCalls() {
  size_t total = 0;
  // Recursive walk over the tree.
  std::vector<const Node*> stack{&root};
  while (!stack.empty()) {
    const Node* node = stack.back();
    stack.pop_back();
    total += node->readCalls;
    for (const auto& [name, child] : node->children) stack.push_back(&child);
  }
  return total;
}

}  // namespace host_fs

class HalFile : public Print {
  friend class HalStorage;

 public:
  HalFile() = default;

  size_t size() const { return node_ && !node_->isDir ? node_->bytes.size() : 0; }
  size_t fileSize() const { return size(); }
  uint64_t fileSize64() const { return size(); }
  int available() const {
    return node_ && !node_->isDir ? static_cast<int>(node_->bytes.size() - std::min(cursor_, node_->bytes.size())) : 0;
  }
  size_t position() const { return cursor_; }

  int read() {
    uint8_t byte = 0;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  int read(void* out, const size_t length) {
    if (!node_ || node_->isDir) return 0;
    ++node_->readCalls;
    const size_t readable = std::min(length, node_->bytes.size() - std::min(cursor_, node_->bytes.size()));
    if (readable > 0) std::copy_n(node_->bytes.data() + cursor_, readable, static_cast<uint8_t*>(out));
    cursor_ += readable;
    return static_cast<int>(readable);
  }

  bool seek(const size_t position) {
    if (!node_ || node_->isDir || position > node_->bytes.size()) return false;
    cursor_ = position;
    return true;
  }
  bool seekCur(const int offset) { return offset >= 0 && seek(cursor_ + static_cast<size_t>(offset)); }
  bool seekSet(const size_t position) { return seek(position); }

  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const void* in, const size_t length) {
    return write(static_cast<const uint8_t*>(in), length);
  }
  size_t write(const uint8_t* in, const size_t length) override {
    if (!node_ || node_->isDir || length == 0) return 0;
    if (cursor_ + length > node_->bytes.size()) node_->bytes.resize(cursor_ + length);
    std::copy_n(in, length, node_->bytes.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ += length;
    return length;
  }

  bool allocationFailed() const { return false; }
  bool isDirectory() const { return node_ != nullptr && node_->isDir; }
  bool iterationFailed() const { return false; }
  void rewindDirectory() {
    iterCursor_ = std::nullopt;
  }
  HalFile openNextFile() {
    if (!node_ || !node_->isDir) return HalFile{};
    if (iterCursor_) ++*iterCursor_;
    else iterCursor_ = 0;
    if (*iterCursor_ >= node_->children.size()) return HalFile{};
    auto it = node_->children.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(*iterCursor_));
    HalFile file;
    file.node_ = &it->second;
    file.name_ = it->first;
    return file;
  }
  size_t getName(char* name, const size_t len) {
    if (!name || len == 0) return 0;
    name[0] = '\0';
    if (name_.empty()) return 0;
    const size_t copied = std::min(len - 1, name_.size());
    std::copy_n(name_.begin(), copied, name);
    name[copied] = '\0';
    return copied;
  }

  bool sync() const { return node_ != nullptr; }
  bool close() {
    node_ = nullptr;
    name_.clear();
    cursor_ = 0;
    rewindDirectory();
    return true;
  }
  explicit operator bool() const { return node_ != nullptr; }

 private:
  host_fs::Node* node_ = nullptr;
  std::string name_;
  size_t cursor_ = 0;
  std::optional<size_t> iterCursor_;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  void reset() const { host_fs::reset(); }
  bool mkdir(const char* path, bool = true) const {
    std::string dir(path);
    if (dir.empty()) return false;
    if (host_fs::Node* node = host_fs::find(dir)) {
      node->isDir = true;
      return true;
    }
    // Create intermediate directories as needed.
    host_fs::Node* node = &host_fs::root;
    size_t start = dir[0] == '/' ? 1 : 0;
    while (start < dir.size()) {
      const size_t slash = dir.find('/', start);
      const std::string part =
          dir.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
      if (part.empty()) return false;
      node = &node->children[part];
      node->isDir = true;
      if (slash == std::string::npos) break;
      start = slash + 1;
    }
    return true;
  }
  bool exists(const char* path) const {
    const host_fs::Node* node = host_fs::find(path);
    return node != nullptr;
  }
  bool remove(const char* path) const {
    const std::string file(path);
    const size_t slash = file.find_last_of('/');
    if (slash == std::string::npos) return false;
    host_fs::Node* dir = host_fs::find(file.substr(0, slash == 0 ? std::string::npos : slash));
    if (!dir || !dir->isDir) return false;
    return dir->children.erase(file.substr(slash + 1)) > 0;
  }
  bool rename(const char* from, const char* to) const {
    host_fs::Node* src = host_fs::find(from);
    if (!src || src->isDir) return false;
    host_fs::put(to, std::move(src->bytes));
    return remove(from);
  }

  HalFile open(const char* path, oflag_t = O_RDONLY) const {
    HalFile file;
    host_fs::Node* node = host_fs::find(path);
    if (!node) return file;
    if (node->isDir) {
      file.node_ = node;
      const std::string dir(path);
      const size_t slash = dir.find_last_of('/');
      file.name_ = slash == std::string::npos ? dir : dir.substr(slash + 1);
      return file;
    }
    file.node_ = node;
    const std::string dir(path);
    const size_t slash = dir.find_last_of('/');
    file.name_ = slash == std::string::npos ? dir : dir.substr(slash + 1);
    return file;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) const {
    file = open(path.c_str(), O_RDONLY);
    return static_cast<bool>(file) && !file.isDirectory();
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) const {
    host_fs::put(path, {});
    file = open(path.c_str(), O_RDWR);
    return static_cast<bool>(file);
  }

 private:
  HalStorage() = default;
};

inline HalStorage& Storage = HalStorage::getInstance();
